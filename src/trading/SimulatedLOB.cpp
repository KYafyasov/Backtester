#include "trading/SimulatedLOB.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace cmf::trading {

bool EngineView::BuyFirst::operator()(const RestingKey &left,
                                      const RestingKey &right) const noexcept {
  return std::tuple{-left.price, left.arrival_sequence, left.client_order_id} <
         std::tuple{-right.price, right.arrival_sequence,
                    right.client_order_id};
}

bool EngineView::SellFirst::operator()(const RestingKey &left,
                                       const RestingKey &right) const noexcept {
  return std::tie(left.price, left.arrival_sequence, left.client_order_id) <
         std::tie(right.price, right.arrival_sequence, right.client_order_id);
}

EngineView::EngineView(std::span<const InstrumentMeta> instruments) {
  resting_.reserve(instruments.size());
  for (const auto &instrument : instruments) {
    if (!resting_.try_emplace(instrument.instrument_id).second) {
      throw std::invalid_argument("duplicate EngineView instrument");
    }
  }
}

SimulatedLOB::SimulatedLOB(std::span<const InstrumentMeta> instruments,
                           FillModel fill_model)
    : SimulatedLOB(instruments, BacktestConfig{0, 1, 15, false, fill_model}) {}

SimulatedLOB::SimulatedLOB(std::span<const InstrumentMeta> instruments,
                           const BacktestConfig &config)
    : view_(instruments), fill_model_(config.fill_model),
      execution_costs_(instruments, config) {
  if (fill_model_ != FillModel::FillAtTouch &&
      fill_model_ != FillModel::QueueAware) {
    throw std::invalid_argument("unknown fill model");
  }
  fills_.reserve(8);
}

std::span<const SyntheticFill>
SimulatedLOB::accept(ClOrdId client_order_id, InstrumentId instrument_id,
                     Side side, PriceTicks limit_price,
                     Quantity remaining_quantity, Sequence arrival_sequence,
                     const market::LimitOrderBook *book) {
  const auto same_side_level =
      book == nullptr ? std::nullopt : book->level(side, limit_price);
  return accept_with_touch(
      client_order_id, instrument_id, side, limit_price, remaining_quantity,
      arrival_sequence,
      book == nullptr || !book->best_bid().has_value()
          ? std::nullopt
          : std::optional<PriceTicks>{book->best_bid()->price},
      book == nullptr || !book->best_ask().has_value()
          ? std::nullopt
          : std::optional<PriceTicks>{book->best_ask()->price},
      same_side_level.has_value()
          ? std::optional<Quantity>{same_side_level->quantity}
          : std::nullopt,
      book == nullptr ? 0 : book->last_book_source_sequence());
}

std::span<const SyntheticFill> SimulatedLOB::accept_from_store(
    ClOrdId client_order_id, InstrumentId instrument_id, Side side,
    PriceTicks limit_price, Quantity remaining_quantity,
    Sequence arrival_sequence, const market::HistoricalLOBStore *books) {
  const auto bid =
      books == nullptr ? std::nullopt : books->best_bid(instrument_id);
  const auto ask =
      books == nullptr ? std::nullopt : books->best_ask(instrument_id);
  const auto same_side_level =
      books == nullptr ? std::nullopt
                       : books->level(instrument_id, side, limit_price);
  return accept_with_touch(
      client_order_id, instrument_id, side, limit_price, remaining_quantity,
      arrival_sequence,
      bid.has_value() ? std::optional<PriceTicks>{bid->price} : std::nullopt,
      ask.has_value() ? std::optional<PriceTicks>{ask->price} : std::nullopt,
      same_side_level.has_value()
          ? std::optional<Quantity>{same_side_level->quantity}
          : std::nullopt,
      books == nullptr ? 0 : books->last_book_source_sequence(instrument_id));
}

std::span<const SyntheticFill> SimulatedLOB::accept_with_touch(
    ClOrdId client_order_id, InstrumentId instrument_id, Side side,
    PriceTicks limit_price, Quantity remaining_quantity,
    Sequence arrival_sequence, std::optional<PriceTicks> best_bid,
    std::optional<PriceTicks> best_ask,
    std::optional<Quantity> same_side_quantity, Sequence source_sequence) {
  fills_.clear();
  if (view_.resting_.find(instrument_id) == view_.resting_.end()) {
    throw SimulatedLOBError("accepted order references unknown instrument");
  }
  Quantity queue_threshold{};
  if (fill_model_ == FillModel::QueueAware) {
    const QueueKey key{instrument_id, side, limit_price};
    const Quantity traded = executed_volume(key);
    const Quantity displayed = same_side_quantity.value_or(0);
    if (displayed < 0 ||
        traded > std::numeric_limits<Quantity>::max() - displayed) {
      throw SimulatedLOBError("queue threshold overflow");
    }
    queue_threshold = traded + displayed;
    for (const auto &[id, existing] : view_.orders_) {
      (void)id;
      if (existing.instrument_id != instrument_id || existing.side != side ||
          existing.limit_price != limit_price) {
        continue;
      }
      if (existing.remaining_quantity >
          std::numeric_limits<Quantity>::max() - queue_threshold) {
        throw SimulatedLOBError("private queue threshold overflow");
      }
      queue_threshold += existing.remaining_quantity;
    }
  }
  EngineView::PrivateOrder order{
      instrument_id,    client_order_id,    side,
      limit_price,      remaining_quantity, remaining_quantity,
      arrival_sequence, queue_threshold};
  const auto [iterator, inserted] =
      view_.orders_.emplace(client_order_id, order);
  if (!inserted) {
    throw SimulatedLOBError("duplicate private order");
  }
  insert_resting(iterator->second);
  if (best_bid.has_value() || best_ask.has_value()) {
    match_prices(instrument_id, best_ask, best_bid, LiquiditySource::QuoteCross,
                 source_sequence);
  }
  return fills_;
}

std::span<const SyntheticFill>
SimulatedLOB::on_signal(const PriceCrossSignal &signal) {
  fills_.clear();
  if (signal.source == PriceCrossSource::BestQuote) {
    if (signal.trade_price.has_value()) {
      throw SimulatedLOBError("best-quote signal carries a trade price");
    }
    match_prices(signal.instrument_id, signal.best_ask, signal.best_bid,
                 LiquiditySource::QuoteCross, signal.source_sequence,
                 signal.source_id, signal.global_market_sequence);
  } else if (signal.source == PriceCrossSource::Trade) {
    if (!signal.trade_price.has_value() || signal.best_bid.has_value() ||
        signal.best_ask.has_value()) {
      throw SimulatedLOBError("trade signal has invalid price fields");
    }
    if (fill_model_ == FillModel::QueueAware) {
      match_queue_trade(signal);
    } else {
      match_prices(signal.instrument_id, signal.trade_price, signal.trade_price,
                   LiquiditySource::TradeCross, signal.source_sequence,
                   signal.source_id, signal.global_market_sequence);
    }
  } else {
    throw SimulatedLOBError("price-cross signal has invalid source");
  }
  return fills_;
}

void SimulatedLOB::cancel(ClOrdId client_order_id) {
  const auto iterator = view_.orders_.find(client_order_id);
  if (iterator == view_.orders_.end()) {
    throw SimulatedLOBError("cancel references unknown private order");
  }
  release_cancelled_queue(iterator->second);
  erase_resting(iterator->second);
  view_.orders_.erase(iterator);
}

std::optional<Quantity>
SimulatedLOB::queue_ahead(ClOrdId client_order_id) const {
  const auto iterator = view_.orders_.find(client_order_id);
  if (iterator == view_.orders_.end()) {
    return std::nullopt;
  }
  return fill_model_ == FillModel::QueueAware
             ? std::optional<Quantity>{queue_ahead_quantity(iterator->second)}
             : std::optional<Quantity>{0};
}

Quantity SimulatedLOB::executed_volume(const QueueKey &key) const noexcept {
  const auto iterator = executed_volume_.find(key);
  return iterator == executed_volume_.end() ? 0 : iterator->second;
}

Quantity SimulatedLOB::queue_ahead_quantity(
    const EngineView::PrivateOrder &order) const noexcept {
  const Quantity traded = executed_volume(
      QueueKey{order.instrument_id, order.side, order.limit_price});
  return order.queue_threshold > traded ? order.queue_threshold - traded : 0;
}

void SimulatedLOB::match_queue_trade(const PriceCrossSignal &signal) {
  if (signal.trade_quantity <= 0) {
    throw SimulatedLOBError("queue-aware trade must have positive quantity");
  }
  const Side passive_side =
      signal.trade_aggressor_side == Side::Buy    ? Side::Sell
      : signal.trade_aggressor_side == Side::Sell ? Side::Buy
                                                  : Side::None;
  if (passive_side == Side::None) {
    return;
  }

  const QueueKey key{signal.instrument_id, passive_side, *signal.trade_price};
  Quantity &traded = executed_volume_[key];
  if (traded > std::numeric_limits<Quantity>::max() - signal.trade_quantity) {
    throw SimulatedLOBError("queue traded-volume overflow");
  }
  traded += signal.trade_quantity;

  auto instrument = view_.resting_.find(signal.instrument_id);
  if (instrument == view_.resting_.end()) {
    throw SimulatedLOBError("queue trade references unknown instrument");
  }
  auto process = [&](auto &index) {
    const EngineView::RestingKey first_at_price{*signal.trade_price, 0, 0};
    for (auto iterator = index.lower_bound(first_at_price);
         iterator != index.end() &&
         iterator->first.price == *signal.trade_price;) {
      const ClOrdId id = iterator->second;
      ++iterator;
      auto own = view_.orders_.find(id);
      if (own == view_.orders_.end()) {
        throw SimulatedLOBError("resting-order index is inconsistent");
      }
      auto &order = own->second;
      if (order.side != passive_side ||
          order.limit_price != *signal.trade_price) {
        continue;
      }
      const Quantity eligible =
          traded > order.queue_threshold ? traded - order.queue_threshold : 0;
      const Quantity target = std::min(order.initial_quantity, eligible);
      const Quantity already_filled =
          order.initial_quantity - order.remaining_quantity;
      if (target <= already_filled) {
        continue;
      }
      const Quantity fill_quantity = target - already_filled;
      order.remaining_quantity -= fill_quantity;
      const auto execution = execution_costs_.apply(
          order.instrument_id, order.side, order.limit_price,
          *signal.trade_price, fill_quantity, LiquiditySource::TradeCross);
      fills_.push_back(SyntheticFill{
          order.client_order_id, execution.fill_price_ticks, fill_quantity,
          LiquiditySource::TradeCross, signal.source_sequence, signal.source_id,
          signal.global_market_sequence, execution.reference_price_ticks,
          execution.liquidity_role, execution.slippage_ticks,
          execution.fee_micros});
      if (order.remaining_quantity == 0) {
        erase_resting(order);
        view_.orders_.erase(own);
      }
    }
  };
  if (passive_side == Side::Buy) {
    process(instrument->second.buys);
  } else {
    process(instrument->second.sells);
  }
}

void SimulatedLOB::release_cancelled_queue(
    const EngineView::PrivateOrder &cancelled) {
  if (fill_model_ != FillModel::QueueAware ||
      cancelled.remaining_quantity == 0) {
    return;
  }
  for (auto &[id, order] : view_.orders_) {
    (void)id;
    if (order.instrument_id != cancelled.instrument_id ||
        order.side != cancelled.side ||
        order.limit_price != cancelled.limit_price ||
        order.arrival_sequence <= cancelled.arrival_sequence) {
      continue;
    }
    if (order.queue_threshold < cancelled.remaining_quantity) {
      throw SimulatedLOBError("private queue threshold underflow");
    }
    order.queue_threshold -= cancelled.remaining_quantity;
  }
}

void SimulatedLOB::match_prices(InstrumentId instrument_id,
                                std::optional<PriceTicks> buy_trigger,
                                std::optional<PriceTicks> sell_trigger,
                                LiquiditySource liquidity_source,
                                Sequence trigger_source_sequence,
                                SourceId trigger_source_id,
                                Sequence trigger_global_market_sequence) {
  auto instrument = view_.resting_.find(instrument_id);
  if (instrument == view_.resting_.end()) {
    throw SimulatedLOBError("price-cross signal references unknown instrument");
  }

  auto process = [&](auto &index, std::optional<PriceTicks> trigger,
                     Side side) {
    while (trigger.has_value() && !index.empty()) {
      const ClOrdId id = index.begin()->second;
      auto own = view_.orders_.find(id);
      if (own == view_.orders_.end()) {
        throw SimulatedLOBError("resting-order index is inconsistent");
      }
      const bool crossed = side == Side::Buy
                               ? *trigger <= own->second.limit_price
                               : *trigger >= own->second.limit_price;
      if (!crossed) {
        break;
      }
      const Quantity fill_quantity = own->second.remaining_quantity;
      const auto execution = execution_costs_.apply(
          own->second.instrument_id, own->second.side, own->second.limit_price,
          *trigger, fill_quantity, liquidity_source);
      erase_resting(own->second);
      own->second.remaining_quantity = 0;
      fills_.push_back(SyntheticFill{
          own->second.client_order_id, execution.fill_price_ticks,
          fill_quantity, liquidity_source, trigger_source_sequence,
          trigger_source_id, trigger_global_market_sequence,
          execution.reference_price_ticks, execution.liquidity_role,
          execution.slippage_ticks, execution.fee_micros});
      view_.orders_.erase(own);
    }
  };
  process(instrument->second.buys, buy_trigger, Side::Buy);
  process(instrument->second.sells, sell_trigger, Side::Sell);
}

void SimulatedLOB::insert_resting(const EngineView::PrivateOrder &order) {
  if (order.remaining_quantity == 0) {
    return;
  }
  const EngineView::RestingKey key{order.limit_price, order.arrival_sequence,
                                   order.client_order_id};
  auto &instrument = view_.resting_.at(order.instrument_id);
  if (order.side == Side::Buy) {
    instrument.buys.emplace(key, order.client_order_id);
  } else if (order.side == Side::Sell) {
    instrument.sells.emplace(key, order.client_order_id);
  } else {
    throw SimulatedLOBError("private order has invalid side");
  }
}

void SimulatedLOB::erase_resting(const EngineView::PrivateOrder &order) {
  const EngineView::RestingKey key{order.limit_price, order.arrival_sequence,
                                   order.client_order_id};
  auto &instrument = view_.resting_.at(order.instrument_id);
  if (order.side == Side::Buy) {
    instrument.buys.erase(key);
  } else if (order.side == Side::Sell) {
    instrument.sells.erase(key);
  }
}

} // namespace cmf::trading
