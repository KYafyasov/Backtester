#pragma once

#include "runtime/BacktestRuntime.hpp"

#include <algorithm>
#include <compare>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cmf::runtime {

enum class MarketRecordKind : std::uint8_t {
  Book = 1,
  Trade = 2,
};

struct MarketAuditRecord {
  Sequence source_local_sequence{};
  TimestampNs event_ts_ns{};
  MarketRecordKind kind{MarketRecordKind::Book};
  InstrumentId instrument_id{};
};

struct MarketGroupKey {
  TimestampNs event_ts_ns{};
  SourcePriority source_priority{};
  SourceId source_id{};
  Sequence source_local_group_sequence{};

  auto operator<=>(const MarketGroupKey &) const = default;
};

class MarketSource {
public:
  virtual ~MarketSource() = default;
  [[nodiscard]] virtual SourceId source_id() const noexcept = 0;
  [[nodiscard]] virtual SourcePriority source_priority() const noexcept = 0;
  [[nodiscard]] virtual std::string_view format() const noexcept = 0;
  [[nodiscard]] virtual TimestampSemantics
  timestamp_semantics() const noexcept = 0;
  [[nodiscard]] virtual std::span<const InstrumentId>
  instrument_ids() const noexcept = 0;
  virtual bool next(ScheduledEvent &event) = 0;
  virtual void prepare_for_dispatch(ScheduledEvent &event) = 0;
  virtual void prepare_for_warmup() = 0;
  [[nodiscard]] virtual std::span<const MarketAuditRecord>
  staged_audit_records() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t expected_records() const noexcept = 0;
  [[nodiscard]] virtual TimestampNs min_event_ts_ns() const noexcept = 0;
  [[nodiscard]] virtual TimestampNs max_event_ts_ns() const noexcept = 0;
  [[nodiscard]] virtual Sequence min_source_sequence() const noexcept = 0;
  [[nodiscard]] virtual Sequence max_source_sequence() const noexcept = 0;
};

class NWayMarketMerger {
public:
  NWayMarketMerger(std::vector<std::unique_ptr<MarketSource>> sources,
                   DateRange range, RunStatistics *statistics)
      : sources_(std::move(sources)), range_(range), statistics_(statistics) {
    if (sources_.size() < 2) {
      throw std::invalid_argument("N-way merger requires at least two sources");
    }
    heads_.resize(sources_.size());
    last_head_keys_.resize(sources_.size());
    heap_.reserve(sources_.size());
    validate_sources();
    if (statistics_ != nullptr) {
      statistics_->multi_source_input = true;
      statistics_->sources.reserve(sources_.size());
      for (const auto &source : sources_) {
        statistics_->sources.push_back(SourceRunStatistics{
            source->source_id(),
            source->source_priority(),
            std::string(source->format()),
            timestamp_semantics_name(source->timestamp_semantics()),
            {source->instrument_ids().begin(), source->instrument_ids().end()},
            source->expected_records(),
            source->min_event_ts_ns(),
            source->max_event_ts_ns(),
            source->min_source_sequence(),
            source->max_source_sequence()});
      }
    }
  }

  bool next(ScheduledEvent &event) {
    if (!initialized_) {
      initialize();
    } else if (winner_.has_value()) {
      if (!winner_prepared_) {
        throw std::logic_error("merged source advanced before preparation");
      }
      advance(*winner_);
      winner_.reset();
      winner_identity_.reset();
      winner_prepared_ = false;
    }

    for (;;) {
      if (heap_.empty()) {
        return false;
      }
      const auto index = pop_winner();
      const auto key = group_key(index);
      const auto input_sequence =
          next_checked(global_input_sequence_, "global input sequence");
      account_read(index);
      if (key.event_ts_ns < range_.start_ts_ns) {
        account_warmup(index);
        sources_[index]->prepare_for_warmup();
        advance(index);
        continue;
      }
      if (key.event_ts_ns > range_.end_ts_ns) {
        account_after_end(index);
        while (!heap_.empty()) {
          const auto after_end_index = pop_winner();
          (void)next_checked(global_input_sequence_, "global input sequence");
          account_read(after_end_index);
          account_after_end(after_end_index);
        }
        return false;
      }

      const auto replay_sequence =
          next_checked(global_replay_sequence_, "global replay sequence");
      account_replay(index, input_sequence, replay_sequence);
      auto delivery = std::get<MarketDelivery>(heads_[index]->payload());
      delivery.source_id = sources_[index]->source_id();
      delivery.global_input_sequence = input_sequence;
      delivery.global_market_sequence = replay_sequence;
      event = ScheduledEvent{delivery};
      winner_ = index;
      winner_identity_ = WinningIdentity{event.key(),
                                         key,
                                         delivery.instrument_id,
                                         delivery.source_id,
                                         delivery.global_input_sequence,
                                         delivery.global_market_sequence};
      return true;
    }
  }

  void prepare_for_dispatch(ScheduledEvent &event) {
    if (!winner_.has_value() || winner_prepared_) {
      throw std::logic_error("merged source has no unprepared winner");
    }
    if (!winner_identity_.has_value()) {
      throw std::logic_error("merged source winner identity is missing");
    }
    sources_[*winner_]->prepare_for_dispatch(event);
    const auto *delivery = std::get_if<MarketDelivery>(&event.payload());
    if (delivery == nullptr) {
      throw std::logic_error("leaf preparation changed merged group identity");
    }
    const auto prepared_group_key = MarketGroupKey{
        delivery->exchange_ts_ns, sources_[*winner_]->source_priority(),
        delivery->source_id, delivery->source_sequence};
    const auto &identity = *winner_identity_;
    if (event.key() != identity.scheduled_key ||
        prepared_group_key != identity.group_key ||
        delivery->instrument_id != identity.instrument_id ||
        delivery->source_id != identity.source_id ||
        delivery->global_input_sequence != identity.global_input_sequence ||
        delivery->global_market_sequence != identity.global_market_sequence) {
      throw std::logic_error("leaf preparation changed merged group identity");
    }
    winner_prepared_ = true;
  }

private:
  struct WinningIdentity {
    ScheduledKey scheduled_key;
    MarketGroupKey group_key;
    InstrumentId instrument_id{};
    SourceId source_id{};
    Sequence global_input_sequence{};
    Sequence global_market_sequence{};
  };

  [[nodiscard]] static Sequence next_checked(Sequence &value,
                                             const char *name) {
    if (value == std::numeric_limits<Sequence>::max()) {
      throw std::overflow_error(std::string(name) + " exhausted");
    }
    return ++value;
  }

  void validate_sources() {
    std::vector<SourceId> ids;
    std::vector<SourcePriority> priorities;
    std::vector<InstrumentId> instruments;
    std::optional<TimestampSemantics> timestamp_semantics;
    for (const auto &source : sources_) {
      if (source == nullptr || source->source_id() == 0 ||
          source->source_priority() == 0) {
        throw std::invalid_argument("invalid N-way source identity");
      }
      if (std::find(ids.begin(), ids.end(), source->source_id()) != ids.end() ||
          std::find(priorities.begin(), priorities.end(),
                    source->source_priority()) != priorities.end()) {
        throw std::invalid_argument("duplicate N-way source ID or priority");
      }
      ids.push_back(source->source_id());
      priorities.push_back(source->source_priority());
      if (!timestamp_semantics.has_value()) {
        timestamp_semantics = source->timestamp_semantics();
      } else if (source->timestamp_semantics() != *timestamp_semantics) {
        throw std::invalid_argument(
            "N-way sources have incompatible timestamp semantics");
      }
      for (const auto instrument_id : source->instrument_ids()) {
        if (instrument_id <= 0 ||
            std::find(instruments.begin(), instruments.end(), instrument_id) !=
                instruments.end()) {
          throw std::invalid_argument("overlapping N-way instrument ownership");
        }
        instruments.push_back(instrument_id);
      }
    }
  }

  void initialize() {
    for (std::size_t index = 0; index < sources_.size(); ++index) {
      if (sources_[index]->next(heads_[index].emplace(MarketDelivery{}))) {
        validate_head(index);
        heap_.push_back(index);
      } else {
        heads_[index].reset();
      }
    }
    std::make_heap(heap_.begin(), heap_.end(), HeapCompare{this});
    initialized_ = true;
  }

  void advance(std::size_t index) {
    ScheduledEvent event{MarketDelivery{}};
    if (!sources_[index]->next(event)) {
      heads_[index].reset();
      return;
    }
    heads_[index] = std::move(event);
    validate_head(index);
    heap_.push_back(index);
    std::push_heap(heap_.begin(), heap_.end(), HeapCompare{this});
  }

  void validate_head(std::size_t index) {
    const auto *delivery =
        std::get_if<MarketDelivery>(&heads_[index]->payload());
    if (delivery == nullptr || delivery->source_sequence == 0 ||
        delivery->source_id != sources_[index]->source_id() ||
        delivery->global_input_sequence != 0 ||
        delivery->global_market_sequence != 0) {
      throw std::logic_error("leaf returned an invalid staged market group");
    }
    const auto key = group_key(index);
    if (last_head_keys_[index].has_value() &&
        (key.event_ts_ns < last_head_keys_[index]->event_ts_ns ||
         key.source_local_group_sequence <=
             last_head_keys_[index]->source_local_group_sequence)) {
      throw std::logic_error("leaf market ordering regressed");
    }
    last_head_keys_[index] = key;
  }

  [[nodiscard]] MarketGroupKey group_key(std::size_t index) const {
    const auto &delivery = std::get<MarketDelivery>(heads_[index]->payload());
    return {delivery.exchange_ts_ns, sources_[index]->source_priority(),
            sources_[index]->source_id(), delivery.source_sequence};
  }

  struct HeapCompare {
    const NWayMarketMerger *owner{};
    bool operator()(std::size_t left, std::size_t right) const {
      return owner->group_key(left) > owner->group_key(right);
    }
  };

  [[nodiscard]] std::size_t pop_winner() {
    std::pop_heap(heap_.begin(), heap_.end(), HeapCompare{this});
    const auto index = heap_.back();
    heap_.pop_back();
    return index;
  }

  [[nodiscard]] SourceRunStatistics *source_statistics(std::size_t index) {
    return statistics_ == nullptr ? nullptr : &statistics_->sources[index];
  }

  void account_read(std::size_t index) {
    const auto records = sources_[index]->staged_audit_records().size();
    if (records == 0) {
      throw std::logic_error("staged group has no audit records");
    }
    if (auto *source = source_statistics(index); source != nullptr) {
      source->records_read += records;
      ++source->groups_read;
    }
    if (statistics_ != nullptr) {
      statistics_->source_records_read += records;
      statistics_->global_input_groups = global_input_sequence_;
      if (!statistics_->first_global_input_sequence.has_value()) {
        statistics_->first_global_input_sequence = global_input_sequence_;
      }
      statistics_->last_global_input_sequence = global_input_sequence_;
    }
  }

  void account_warmup(std::size_t index) {
    const auto records = sources_[index]->staged_audit_records().size();
    if (auto *source = source_statistics(index); source != nullptr) {
      source->records_warmed += records;
      ++source->groups_warmed;
    }
    if (statistics_ != nullptr) {
      statistics_->source_records_warmed += records;
    }
  }

  void account_after_end(std::size_t index) {
    const auto records = sources_[index]->staged_audit_records().size();
    if (auto *source = source_statistics(index); source != nullptr) {
      source->records_after_end += records;
      ++source->groups_after_end;
    }
    if (statistics_ != nullptr) {
      statistics_->source_records_after_end += records;
    }
  }

  void hash_byte(std::uint8_t value) {
    provenance_digest_ ^= value;
    provenance_digest_ *= 1099511628211ULL;
  }

  template <typename Value> void hash_little_endian(Value value) {
    using Unsigned = std::make_unsigned_t<Value>;
    auto bits = static_cast<std::uint64_t>(static_cast<Unsigned>(value));
    for (std::size_t byte = 0; byte < sizeof(Unsigned); ++byte) {
      hash_byte(static_cast<std::uint8_t>(bits & 0xffU));
      bits >>= 8U;
    }
  }

  void account_replay(std::size_t index, Sequence input_sequence,
                      Sequence replay_sequence) {
    const auto records = sources_[index]->staged_audit_records();
    if (auto *source = source_statistics(index); source != nullptr) {
      source->records_replayed += records.size();
      ++source->groups_replayed;
    }
    for (const auto &record : records) {
      hash_little_endian(sources_[index]->source_id());
      hash_little_endian(record.source_local_sequence);
      hash_little_endian(record.event_ts_ns);
      hash_little_endian(static_cast<std::uint8_t>(record.kind));
      hash_little_endian(record.instrument_id);
      if (statistics_ != nullptr) {
        if (record.kind == MarketRecordKind::Trade) {
          ++statistics_->replayed_trade_records;
        } else {
          ++statistics_->replayed_book_records;
        }
      }
    }
    hash_byte(0xffU);
    if (statistics_ != nullptr) {
      statistics_->source_records_replayed += records.size();
      statistics_->global_replay_groups = replay_sequence;
      if (!statistics_->first_global_replay_sequence.has_value()) {
        statistics_->first_global_replay_sequence = replay_sequence;
      }
      statistics_->last_global_replay_sequence = replay_sequence;
      statistics_->provenance_digest = provenance_digest_;
      (void)input_sequence;
    }
  }

  std::vector<std::unique_ptr<MarketSource>> sources_;
  DateRange range_;
  RunStatistics *statistics_{};
  std::vector<std::optional<ScheduledEvent>> heads_;
  std::vector<std::optional<MarketGroupKey>> last_head_keys_;
  std::vector<std::size_t> heap_;
  std::optional<std::size_t> winner_;
  std::optional<WinningIdentity> winner_identity_;
  Sequence global_input_sequence_{};
  Sequence global_replay_sequence_{};
  std::uint64_t provenance_digest_{14695981039346656037ULL};
  bool initialized_{};
  bool winner_prepared_{};
};

} // namespace cmf::runtime
