#include "runtime/BacktestRuntime.hpp"

#include "market/HistoricalLOBStore.hpp"
#include "market/JsonlReader.hpp"
#include "market/L2CacheReader.hpp"
#include "scheduler/SchedulerRuntime.hpp"
#include "trading/TradingEngine.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cmf::runtime {
namespace {

[[nodiscard]] TimestampNs checked_delivery_time(TimestampNs exchange_time,
                                                TimestampNs latency) {
  TimestampNs result{};
  if (__builtin_add_overflow(exchange_time, latency, &result)) {
    throw market::SourceError({}, 0, {}, "market delivery timestamp overflow");
  }
  return result;
}

[[nodiscard]] bool equal_levels(const std::vector<BookLevel> &left,
                                const std::vector<BookLevel> &right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const BookLevel &a, const BookLevel &b) {
                      return a.price == b.price && a.quantity == b.quantity;
                    });
}

struct CachedDepth {
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};

class JsonlScheduledSource {
public:
  JsonlScheduledSource(std::string path,
                       market::JsonlReader::InstrumentMap instruments,
                       market::HistoricalLOBStore &books, DateRange range,
                       BacktestConfig config)
      : path_(path), reader_(std::move(path), std::move(instruments)),
        books_(books), range_(range), config_(config) {
    group_.reserve(8);
    bids_.reserve(config.book_depth);
    asks_.reserve(config.book_depth);
    trades_.reserve(8);
    price_cross_signals_.reserve(8);
  }

  bool next(ScheduledEvent &scheduled) {
    if (group_staged_) {
      throw std::logic_error(
          "historical source advanced before staged group dispatch");
    }
    // The scheduler calls next() only after the prior delivery is
    // acknowledged, so this is the first point where its spans may be reused.
    bids_.clear();
    asks_.clear();
    trades_.clear();
    price_cross_signals_.clear();

    for (;;) {
      market::MarketDataEvent first;
      if (!reader_.next(first)) {
        return false;
      }

      const InstrumentId instrument_id = first.instrument_id;
      const TimestampNs exchange_time = first.exchange_ts_ns;
      group_.clear();
      group_.push_back(first);
      while (!group_.back().is_last_in_group()) {
        market::MarketDataEvent next_event;
        if (!reader_.next(next_event)) {
          throw market::SourceError(
              path_, reader_.row(), {},
              "unterminated atomic market group at end of file");
        }
        if (next_event.instrument_id != instrument_id ||
            next_event.exchange_ts_ns != exchange_time) {
          throw market::SourceError(
              path_, reader_.row(), {},
              "atomic market group changed instrument or exchange timestamp");
        }
        group_.push_back(next_event);
      }

      if (exchange_time < range_.start_ts_ns) {
        for (const auto &event : group_) {
          books_.apply(event);
        }
        seed_depth_cache(instrument_id);
        continue;
      }
      if (exchange_time > range_.end_ts_ns) {
        return false;
      }

      const TimestampNs engine_time =
          checked_delivery_time(exchange_time, config_.market_data_latency_ns);
      group_staged_ = true;
      scheduled = ScheduledEvent{MarketDelivery{
          instrument_id,
          exchange_time,
          engine_time,
          group_.back().source_sequence,
          std::nullopt,
          {},
          {},
      }};
      return true;
    }
  }

  void prepare_for_dispatch(ScheduledEvent &scheduled) {
    if (!group_staged_) {
      throw std::logic_error("historical source has no staged market group");
    }
    const auto *staged = std::get_if<MarketDelivery>(&scheduled.payload());
    if (staged == nullptr || group_.empty() ||
        staged->instrument_id != group_.front().instrument_id ||
        staged->exchange_ts_ns != group_.front().exchange_ts_ns ||
        staged->source_sequence != group_.back().source_sequence) {
      throw std::logic_error(
          "scheduled market event does not match staged group");
    }

    bool contains_clear = false;
    for (const auto &event : group_) {
      books_.apply(event);
      contains_clear =
          contains_clear || event.action == market::MarketAction::Clear;
      if (event.action == market::MarketAction::Trade) {
        trades_.push_back(TradeView{
            event.instrument_id,
            event.exchange_ts_ns,
            staged->engine_ts_ns,
            event.source_sequence,
            event.side,
            event.price_ticks.value(),
            event.quantity,
        });
        price_cross_signals_.push_back(PriceCrossSignal{
            event.instrument_id,
            event.exchange_ts_ns,
            staged->engine_ts_ns,
            event.source_sequence,
            PriceCrossSource::Trade,
            std::nullopt,
            std::nullopt,
            event.price_ticks,
        });
      } else {
        const auto best_bid = books_.best_bid(event.instrument_id);
        const auto best_ask = books_.best_ask(event.instrument_id);
        price_cross_signals_.push_back(PriceCrossSignal{
            event.instrument_id,
            event.exchange_ts_ns,
            staged->engine_ts_ns,
            event.source_sequence,
            PriceCrossSource::BestQuote,
            best_bid.has_value() ? std::optional<PriceTicks>{best_bid->price}
                                 : std::nullopt,
            best_ask.has_value() ? std::optional<PriceTicks>{best_ask->price}
                                 : std::nullopt,
            std::nullopt,
        });
      }
    }

    const InstrumentId instrument_id = staged->instrument_id;
    books_.write_top_bids(instrument_id, config_.book_depth, bids_);
    books_.write_top_asks(instrument_id, config_.book_depth, asks_);

    auto &previous = cached_depth(instrument_id);
    const bool changed = !equal_levels(previous.bids, bids_) ||
                         !equal_levels(previous.asks, asks_);
    std::optional<BookUpdateView> book_update;
    if (changed) {
      previous.bids.assign(bids_.begin(), bids_.end());
      previous.asks.assign(asks_.begin(), asks_.end());
      book_update.emplace(BookUpdateView{
          instrument_id,
          staged->exchange_ts_ns,
          staged->engine_ts_ns,
          staged->source_sequence,
          contains_clear,
          bids_,
          asks_,
      });
    }

    scheduled = ScheduledEvent{MarketDelivery{
        instrument_id,
        staged->exchange_ts_ns,
        staged->engine_ts_ns,
        staged->source_sequence,
        book_update,
        trades_,
        price_cross_signals_,
    }};
    group_staged_ = false;
  }

private:
  CachedDepth &cached_depth(InstrumentId instrument_id) {
    auto [iterator, inserted] =
        previous_depth_.try_emplace(instrument_id, CachedDepth{});
    if (inserted) {
      iterator->second.bids.reserve(config_.book_depth);
      iterator->second.asks.reserve(config_.book_depth);
    }
    return iterator->second;
  }

  void seed_depth_cache(InstrumentId instrument_id) {
    auto &cached = cached_depth(instrument_id);
    books_.write_top_bids(instrument_id, config_.book_depth, cached.bids);
    books_.write_top_asks(instrument_id, config_.book_depth, cached.asks);
  }

  std::string path_;
  market::JsonlReader reader_;
  market::HistoricalLOBStore &books_;
  DateRange range_;
  BacktestConfig config_;
  std::unordered_map<InstrumentId, CachedDepth> previous_depth_;
  std::vector<market::MarketDataEvent> group_;
  std::vector<BookLevel> bids_;
  std::vector<BookLevel> asks_;
  std::vector<TradeView> trades_;
  std::vector<PriceCrossSignal> price_cross_signals_;
  bool group_staged_{};
};

class L2CacheScheduledSource {
public:
  L2CacheScheduledSource(std::string manifest_path,
                         market::L2CacheReader::InstrumentMap instruments,
                         market::HistoricalLOBStore &books, DateRange range,
                         BacktestConfig config)
      : reader_(std::move(manifest_path), std::move(instruments), range),
        books_(books), range_(range), config_(config) {
    bids_.reserve(config.book_depth);
    asks_.reserve(config.book_depth);
    trades_.reserve(1);
    signals_.reserve(1);
  }

  bool next(ScheduledEvent &scheduled) {
    if (staged_) {
      throw std::logic_error("L2 source advanced before staged dispatch");
    }
    bids_.clear();
    asks_.clear();
    trades_.clear();
    signals_.clear();
    for (;;) {
      if (!reader_.next(event_)) {
        return false;
      }
      if (event_.event_ts_ns < range_.start_ts_ns) {
        if (event_.kind == market::L2EventKind::Snapshot) {
          books_.replace_snapshot(event_.instrument_id, event_.bids,
                                  event_.asks, event_.merged_sequence);
          seed_depth_cache();
        }
        continue;
      }
      if (event_.event_ts_ns > range_.end_ts_ns) {
        return false;
      }
      const auto engine_time = checked_delivery_time(
          event_.event_ts_ns, config_.market_data_latency_ns);
      staged_ = true;
      scheduled = ScheduledEvent{MarketDelivery{event_.instrument_id,
                                                event_.event_ts_ns,
                                                engine_time,
                                                event_.merged_sequence,
                                                std::nullopt,
                                                {},
                                                {}}};
      return true;
    }
  }

  void prepare_for_dispatch(ScheduledEvent &scheduled) {
    if (!staged_) {
      throw std::logic_error("L2 source has no staged event");
    }
    const auto *delivery = std::get_if<MarketDelivery>(&scheduled.payload());
    if (delivery == nullptr ||
        delivery->instrument_id != event_.instrument_id ||
        delivery->exchange_ts_ns != event_.event_ts_ns ||
        delivery->source_sequence != event_.merged_sequence) {
      throw std::logic_error(
          "scheduled delivery does not match staged L2 event");
    }

    std::optional<BookUpdateView> book_update;
    if (event_.kind == market::L2EventKind::Snapshot) {
      books_.replace_snapshot(event_.instrument_id, event_.bids, event_.asks,
                              event_.merged_sequence);
      books_.write_top_bids(event_.instrument_id, config_.book_depth, bids_);
      books_.write_top_asks(event_.instrument_id, config_.book_depth, asks_);
      auto &previous = cached_depth();
      if (!equal_levels(previous.bids, bids_) ||
          !equal_levels(previous.asks, asks_)) {
        previous.bids.assign(bids_.begin(), bids_.end());
        previous.asks.assign(asks_.begin(), asks_.end());
        book_update.emplace(BookUpdateView{
            event_.instrument_id, event_.event_ts_ns, delivery->engine_ts_ns,
            event_.merged_sequence, true, bids_, asks_});
      }
      const auto bid = books_.best_bid(event_.instrument_id);
      const auto ask = books_.best_ask(event_.instrument_id);
      signals_.push_back(PriceCrossSignal{
          event_.instrument_id, event_.event_ts_ns, delivery->engine_ts_ns,
          event_.merged_sequence, PriceCrossSource::BestQuote,
          bid.has_value() ? std::optional<PriceTicks>{bid->price}
                          : std::nullopt,
          ask.has_value() ? std::optional<PriceTicks>{ask->price}
                          : std::nullopt,
          std::nullopt});
    } else {
      trades_.push_back(TradeView{event_.instrument_id, event_.event_ts_ns,
                                  delivery->engine_ts_ns,
                                  event_.merged_sequence, event_.side,
                                  event_.trade_price, event_.trade_quantity});
      signals_.push_back(PriceCrossSignal{
          event_.instrument_id, event_.event_ts_ns, delivery->engine_ts_ns,
          event_.merged_sequence, PriceCrossSource::Trade, std::nullopt,
          std::nullopt, event_.trade_price});
    }

    scheduled = ScheduledEvent{MarketDelivery{
        event_.instrument_id, event_.event_ts_ns, delivery->engine_ts_ns,
        event_.merged_sequence, book_update, trades_, signals_}};
    staged_ = false;
  }

private:
  CachedDepth &cached_depth() {
    auto [iterator, inserted] =
        previous_depth_.try_emplace(event_.instrument_id, CachedDepth{});
    if (inserted) {
      iterator->second.bids.reserve(config_.book_depth);
      iterator->second.asks.reserve(config_.book_depth);
    }
    return iterator->second;
  }

  void seed_depth_cache() {
    auto &cache = cached_depth();
    books_.write_top_bids(event_.instrument_id, config_.book_depth, cache.bids);
    books_.write_top_asks(event_.instrument_id, config_.book_depth, cache.asks);
  }

  market::L2CacheReader reader_;
  market::HistoricalLOBStore &books_;
  DateRange range_;
  BacktestConfig config_;
  market::L2InputEvent event_;
  std::unordered_map<InstrumentId, CachedDepth> previous_depth_;
  std::vector<BookLevel> bids_;
  std::vector<BookLevel> asks_;
  std::vector<TradeView> trades_;
  std::vector<PriceCrossSignal> signals_;
  bool staged_{};
};

void validate(DateRange range, BacktestConfig config,
              const std::vector<InstrumentMeta> &instruments) {
  if (range.start_ts_ns > range.end_ts_ns) {
    throw std::invalid_argument("date range start must not exceed end");
  }
  if (config.market_data_latency_ns < 0 || config.order_latency_ns <= 0 ||
      config.book_depth == 0) {
    throw std::invalid_argument("market latency must be non-negative, order "
                                "latency and depth positive");
  }
  if (instruments.empty()) {
    throw std::invalid_argument("at least one instrument is required");
  }
  std::unordered_set<InstrumentId> ids;
  ids.reserve(instruments.size());
  for (const auto &meta : instruments) {
    if (meta.instrument_id <= 0 || meta.tick_size_ticks <= 0 ||
        meta.price_scale <= 0 || meta.contract_multiplier <= 0) {
      throw std::invalid_argument("instrument metadata must be positive");
    }
    if (!ids.insert(meta.instrument_id).second) {
      throw std::invalid_argument("duplicate instrument metadata");
    }
  }
}

template <typename Source>
void execute_source(Source &source, trading::TradingEngine &engine,
                    market::HistoricalLOBStore &books,
                    results::ResultRecorder &recorder, DateRange range) {
  scheduler::SchedulerRuntime scheduler(
      scheduler::SchedulerRuntimeConfig{range, 1, 64, 4096});
  scheduler.run(source, [&](const ScheduledEvent &event,
                            scheduler::CommandSink &commands) {
    engine(event, commands);
    const auto *delivery = std::get_if<MarketDelivery>(&event.payload());
    if (delivery == nullptr || !delivery->book_update.has_value()) {
      return;
    }
    const auto bid = books.best_bid(delivery->instrument_id);
    const auto ask = books.best_ask(delivery->instrument_id);
    recorder.on_book_mark(
        delivery->instrument_id, delivery->engine_ts_ns,
        bid.has_value() ? std::optional<PriceTicks>{bid->price} : std::nullopt,
        ask.has_value() ? std::optional<PriceTicks>{ask->price} : std::nullopt);
  });
}

} // namespace

std::vector<InstrumentMeta>
discover_databento_instruments(const std::string &data_path) {
  if (market::L2CacheReader::is_l2_manifest(data_path)) {
    return {market::L2CacheReader::discover_instrument(data_path)};
  }
  market::JsonlReader reader(data_path,
                             market::JsonlReader::databento_nanounit_policy());
  std::unordered_set<InstrumentId> unique_ids;
  market::MarketDataEvent event;
  while (reader.next(event)) {
    unique_ids.insert(event.instrument_id);
  }
  std::vector<InstrumentMeta> instruments;
  instruments.reserve(unique_ids.size());
  const auto policy = market::JsonlReader::databento_nanounit_policy();
  for (const InstrumentId instrument_id : unique_ids) {
    instruments.push_back(InstrumentMeta{
        instrument_id,
        policy.tick_size_ticks,
        policy.price_scale,
        policy.contract_multiplier,
    });
  }
  std::sort(instruments.begin(), instruments.end(),
            [](const InstrumentMeta &left, const InstrumentMeta &right) {
              return left.instrument_id < right.instrument_id;
            });
  return instruments;
}

results::FrozenResults run_backtest(trading::Strategy &strategy,
                                    const std::string &data_path,
                                    DateRange date_range, BacktestConfig config,
                                    std::vector<InstrumentMeta> instruments) {
  validate(date_range, config, instruments);
  market::JsonlReader::InstrumentMap metadata;
  metadata.reserve(instruments.size());
  for (const auto &meta : instruments) {
    metadata.emplace(meta.instrument_id, meta);
  }

  market::HistoricalLOBStore books;
  results::ResultRecorder recorder(
      instruments, results::ResultReserveEstimate{64, 128, 128, 16});
  trading::TradingEngine engine(instruments, config, books, strategy, recorder);
  if (market::L2CacheReader::is_l2_manifest(data_path)) {
    L2CacheScheduledSource source(data_path, std::move(metadata), books,
                                  date_range, config);
    execute_source(source, engine, books, recorder, date_range);
  } else {
    JsonlScheduledSource source(data_path, std::move(metadata), books,
                                date_range, config);
    execute_source(source, engine, books, recorder, date_range);
  }
  return recorder.freeze();
}

} // namespace cmf::runtime
