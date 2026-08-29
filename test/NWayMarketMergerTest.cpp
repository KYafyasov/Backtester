#include "runtime/NWayMarketMerger.hpp"

#include "MiniTest.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace cmf;
using namespace cmf::runtime;

struct FixtureGroup {
  TimestampNs timestamp{};
  Sequence sequence{};
  MarketRecordKind kind{MarketRecordKind::Book};
};

enum class PreparationMutation {
  None,
  Timestamp,
  LocalSequence,
  Instrument,
};

class FixtureSource final : public MarketSource {
public:
  FixtureSource(SourceId source_id, SourcePriority priority,
                InstrumentId instrument_id, std::vector<FixtureGroup> groups,
                std::vector<SourceId> &warm_order,
                PreparationMutation mutation = PreparationMutation::None,
                TimestampSemantics timestamp_semantics =
                    TimestampSemantics::Exchange)
      : source_id_(source_id), priority_(priority),
        instrument_ids_{instrument_id}, groups_(std::move(groups)),
        warm_order_(warm_order), mutation_(mutation),
        timestamp_semantics_(timestamp_semantics) {}

  SourceId source_id() const noexcept override { return source_id_; }
  SourcePriority source_priority() const noexcept override { return priority_; }
  std::string_view format() const noexcept override { return "fixture"; }
  TimestampSemantics timestamp_semantics() const noexcept override {
    return timestamp_semantics_;
  }
  std::span<const InstrumentId> instrument_ids() const noexcept override {
    return instrument_ids_;
  }

  bool next(ScheduledEvent &event) override {
    if (staged_) {
      throw std::logic_error("fixture advanced while staged");
    }
    if (index_ == groups_.size()) {
      return false;
    }
    current_ = groups_[index_++];
    audit_ = {current_.sequence, current_.timestamp, current_.kind,
              instrument_ids_.front()};
    staged_ = true;
    event = ScheduledEvent{MarketDelivery{instrument_ids_.front(),
                                          current_.timestamp,
                                          current_.timestamp,
                                          current_.sequence,
                                          {},
                                          {},
                                          {},
                                          source_id_}};
    return true;
  }

  void prepare_for_dispatch(ScheduledEvent &event) override {
    if (!staged_) {
      throw std::logic_error("fixture has no staged group");
    }
    auto delivery = std::get<MarketDelivery>(event.payload());
    if (mutation_ == PreparationMutation::Timestamp) {
      ++delivery.exchange_ts_ns;
    } else if (mutation_ == PreparationMutation::LocalSequence) {
      ++delivery.source_sequence;
    } else if (mutation_ == PreparationMutation::Instrument) {
      ++delivery.instrument_id;
    }
    event = ScheduledEvent{delivery};
    staged_ = false;
  }

  void prepare_for_warmup() override {
    if (!staged_) {
      throw std::logic_error("fixture has no staged warm-up group");
    }
    warm_order_.push_back(source_id_);
    staged_ = false;
  }

  std::span<const MarketAuditRecord>
  staged_audit_records() const noexcept override {
    return staged_ ? std::span<const MarketAuditRecord>{&audit_, 1}
                   : std::span<const MarketAuditRecord>{};
  }
  std::uint64_t expected_records() const noexcept override {
    return groups_.size();
  }
  TimestampNs min_event_ts_ns() const noexcept override {
    return groups_.empty() ? 0 : groups_.front().timestamp;
  }
  TimestampNs max_event_ts_ns() const noexcept override {
    return groups_.empty() ? 0 : groups_.back().timestamp;
  }
  Sequence min_source_sequence() const noexcept override {
    return groups_.empty() ? 0 : groups_.front().sequence;
  }
  Sequence max_source_sequence() const noexcept override {
    return groups_.empty() ? 0 : groups_.back().sequence;
  }

private:
  SourceId source_id_{};
  SourcePriority priority_{};
  std::vector<InstrumentId> instrument_ids_;
  std::vector<FixtureGroup> groups_;
  std::vector<SourceId> &warm_order_;
  std::size_t index_{};
  FixtureGroup current_{};
  MarketAuditRecord audit_{};
  PreparationMutation mutation_{PreparationMutation::None};
  TimestampSemantics timestamp_semantics_{TimestampSemantics::Exchange};
  bool staged_{};
};

bool preparation_mutation_is_rejected(PreparationMutation mutation) {
  std::vector<SourceId> warm_order;
  std::vector<std::unique_ptr<MarketSource>> sources;
  sources.push_back(std::make_unique<FixtureSource>(
      1, 10, 1, std::vector<FixtureGroup>{{100, 1}}, warm_order, mutation));
  sources.push_back(std::make_unique<FixtureSource>(
      2, 20, 2, std::vector<FixtureGroup>{{200, 1}}, warm_order));
  NWayMarketMerger merger(std::move(sources), DateRange{0, 300}, nullptr);
  ScheduledEvent event{MarketDelivery{}};
  if (!merger.next(event)) {
    return false;
  }
  try {
    merger.prepare_for_dispatch(event);
  } catch (const std::logic_error &) {
    return true;
  }
  return false;
}

std::vector<std::unique_ptr<MarketSource>>
fixture_sources(std::vector<SourceId> &warm_order) {
  std::vector<std::unique_ptr<MarketSource>> sources;
  sources.push_back(std::make_unique<FixtureSource>(
      1, 20, 1,
      std::vector<FixtureGroup>{{90, 1, MarketRecordKind::Book},
                                {110, 2, MarketRecordKind::Book},
                                {120, 3, MarketRecordKind::Trade}},
      warm_order));
  sources.push_back(std::make_unique<FixtureSource>(
      2, 10, 2,
      std::vector<FixtureGroup>{{90, 1, MarketRecordKind::Book},
                                {105, 2, MarketRecordKind::Trade},
                                {120, 3, MarketRecordKind::Book}},
      warm_order));
  return sources;
}

} // namespace

TEST_CASE("N-way merger globally orders warm-up and replay groups",
          "[NWayMarketMerger]") {
  std::vector<SourceId> warm_order;
  RunStatistics statistics;
  NWayMarketMerger merger(fixture_sources(warm_order), DateRange{100, 120},
                          &statistics);

  std::vector<SourceId> replay_sources;
  std::vector<Sequence> replay_sequences;
  ScheduledEvent event{MarketDelivery{}};
  while (merger.next(event)) {
    const auto &delivery = std::get<MarketDelivery>(event.payload());
    replay_sources.push_back(delivery.source_id);
    replay_sequences.push_back(delivery.global_market_sequence);
    merger.prepare_for_dispatch(event);
  }

  REQUIRE(warm_order == std::vector<SourceId>({2, 1}));
  REQUIRE(replay_sources == std::vector<SourceId>({2, 1, 2, 1}));
  REQUIRE(replay_sequences == std::vector<Sequence>({1, 2, 3, 4}));
  REQUIRE(statistics.source_records_read == 6);
  REQUIRE(statistics.source_records_warmed == 2);
  REQUIRE(statistics.source_records_replayed == 4);
  REQUIRE(statistics.global_input_groups == 6);
  REQUIRE(statistics.global_replay_groups == 4);
  REQUIRE(statistics.provenance_digest != 0);
  REQUIRE(statistics.sources.size() == 2);
  for (const auto &source : statistics.sources) {
    REQUIRE(source.records_read == source.records_warmed +
                                       source.records_replayed +
                                       source.records_after_end);
  }
}

TEST_CASE("N-way merger preserves prepare-before-advance lifecycle",
          "[NWayMarketMerger]") {
  std::vector<SourceId> warm_order;
  NWayMarketMerger merger(fixture_sources(warm_order), DateRange{100, 120},
                          nullptr);
  ScheduledEvent event{MarketDelivery{}};
  REQUIRE(merger.next(event));
  bool threw{};
  try {
    (void)merger.next(event);
  } catch (const std::logic_error &) {
    threw = true;
  }
  REQUIRE(threw);
}

TEST_CASE("N-way merger rejects timestamp mutation during preparation",
          "[NWayMarketMerger]") {
  REQUIRE(preparation_mutation_is_rejected(PreparationMutation::Timestamp));
}

TEST_CASE("N-way merger rejects local-sequence mutation during preparation",
          "[NWayMarketMerger]") {
  REQUIRE(preparation_mutation_is_rejected(PreparationMutation::LocalSequence));
}

TEST_CASE("N-way merger rejects instrument mutation during preparation",
          "[NWayMarketMerger]") {
  REQUIRE(preparation_mutation_is_rejected(PreparationMutation::Instrument));
}

TEST_CASE("N-way merger rejects incompatible timestamp semantics",
          "[NWayMarketMerger]") {
  std::vector<SourceId> warm_order;
  std::vector<std::unique_ptr<MarketSource>> sources;
  sources.push_back(std::make_unique<FixtureSource>(
      1, 10, 1, std::vector<FixtureGroup>{{100, 1}}, warm_order));
  sources.push_back(std::make_unique<FixtureSource>(
      2, 20, 2, std::vector<FixtureGroup>{{100, 1}}, warm_order,
      PreparationMutation::None, TimestampSemantics::Receive));
  bool threw{};
  try {
    (void)NWayMarketMerger(std::move(sources), DateRange{0, 200}, nullptr);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  REQUIRE(threw);
}

TEST_CASE("N-way merger handles three sources and a clean empty leaf",
          "[NWayMarketMerger]") {
  std::vector<SourceId> warm_order;
  std::vector<std::unique_ptr<MarketSource>> sources;
  sources.push_back(std::make_unique<FixtureSource>(
      1, 30, 1, std::vector<FixtureGroup>{{100, 1}, {130, 2}}, warm_order));
  sources.push_back(std::make_unique<FixtureSource>(
      2, 10, 2, std::vector<FixtureGroup>{{110, 1}, {130, 2}}, warm_order));
  sources.push_back(std::make_unique<FixtureSource>(
      3, 20, 3, std::vector<FixtureGroup>{{120, 1}}, warm_order));
  sources.push_back(std::make_unique<FixtureSource>(
      4, 40, 4, std::vector<FixtureGroup>{}, warm_order));
  NWayMarketMerger merger(std::move(sources), DateRange{0, 200}, nullptr);

  std::vector<SourceId> order;
  ScheduledEvent event{MarketDelivery{}};
  while (merger.next(event)) {
    order.push_back(std::get<MarketDelivery>(event.payload()).source_id);
    merger.prepare_for_dispatch(event);
  }

  REQUIRE(order == std::vector<SourceId>({1, 2, 3, 2, 1}));
}

TEST_CASE("N-way merger rejects a leaf timestamp regression",
          "[NWayMarketMerger]") {
  std::vector<SourceId> warm_order;
  std::vector<std::unique_ptr<MarketSource>> sources;
  sources.push_back(std::make_unique<FixtureSource>(
      1, 10, 1, std::vector<FixtureGroup>{{110, 1}, {100, 2}}, warm_order));
  sources.push_back(std::make_unique<FixtureSource>(
      2, 20, 2, std::vector<FixtureGroup>{{120, 1}}, warm_order));
  NWayMarketMerger merger(std::move(sources), DateRange{0, 200}, nullptr);

  ScheduledEvent event{MarketDelivery{}};
  REQUIRE(merger.next(event));
  merger.prepare_for_dispatch(event);
  bool threw{};
  try {
    (void)merger.next(event);
  } catch (const std::logic_error &) {
    threw = true;
  }
  REQUIRE(threw);
}

TEST_CASE("N-way merger rejects a leaf local-sequence regression",
          "[NWayMarketMerger]") {
  std::vector<SourceId> warm_order;
  std::vector<std::unique_ptr<MarketSource>> sources;
  sources.push_back(std::make_unique<FixtureSource>(
      1, 10, 1, std::vector<FixtureGroup>{{100, 2}, {110, 1}}, warm_order));
  sources.push_back(std::make_unique<FixtureSource>(
      2, 20, 2, std::vector<FixtureGroup>{{120, 1}}, warm_order));
  NWayMarketMerger merger(std::move(sources), DateRange{0, 200}, nullptr);

  ScheduledEvent event{MarketDelivery{}};
  REQUIRE(merger.next(event));
  merger.prepare_for_dispatch(event);
  bool threw{};
  try {
    (void)merger.next(event);
  } catch (const std::logic_error &) {
    threw = true;
  }
  REQUIRE(threw);
}

TEST_CASE("N-way provenance digest is sensitive to leaf ordering",
          "[NWayMarketMerger]") {
  const auto digest_for_priorities = [](SourcePriority first_priority,
                                        SourcePriority second_priority) {
    std::vector<SourceId> warm_order;
    std::vector<std::unique_ptr<MarketSource>> sources;
    sources.push_back(std::make_unique<FixtureSource>(
        1, first_priority, 1, std::vector<FixtureGroup>{{100, 1}}, warm_order));
    sources.push_back(std::make_unique<FixtureSource>(
        2, second_priority, 2,
        std::vector<FixtureGroup>{{100, 1, MarketRecordKind::Trade}},
        warm_order));
    RunStatistics statistics;
    NWayMarketMerger merger(std::move(sources), DateRange{0, 200}, &statistics);
    ScheduledEvent event{MarketDelivery{}};
    while (merger.next(event)) {
      merger.prepare_for_dispatch(event);
    }
    return statistics.provenance_digest;
  };

  REQUIRE(digest_for_priorities(10, 20) != digest_for_priorities(20, 10));
}
