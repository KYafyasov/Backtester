#include "results/ResultRecorder.hpp"

#include "MiniTest.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace {

using namespace cmf;
using namespace cmf::results;

static_assert(std::is_same_v<std::underlying_type_t<Side>, std::int8_t>);
static_assert(std::is_same_v<std::underlying_type_t<OrderState>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<OrderLogEventType>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<RejectReason>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<LiquiditySource>, std::uint8_t>);

FillResultRow fill(TimestampNs time, InstrumentId instrument_id,
                   ClOrdId order_id, Side side, PriceTicks price,
                   Quantity quantity, Quantity remaining = 0) {
  return FillResultRow{
      time - 1, time,      instrument_id,
      order_id, side,      price,
      quantity, remaining, LiquiditySource::HistoricalDisplayed,
      17};
}

OrderLogResultRow order_event(TimestampNs time, OrderLogEventType event_type,
                              OrderState state) {
  return OrderLogResultRow{time,
                           static_cast<Sequence>(time),
                           7,
                           42,
                           event_type,
                           state == OrderState::PendingNew
                               ? OrderState::PendingNew
                               : OrderState::Open,
                           state,
                           Side::Buy,
                           1'005,
                           10,
                           state == OrderState::PartiallyFilled ? 4 : 0,
                           state == OrderState::PartiallyFilled ? 6 : 10,
                           0,
                           RejectReason::None};
}

std::vector<std::int64_t> exact_numerators(const FrozenResults &result) {
  std::vector<std::int64_t> values;
  for (const auto amount : result.exact_pnl()) {
    values.push_back(amount.numerator);
  }
  return values;
}

} // namespace

TEST_CASE("Result columns preserve every frozen field and equal lengths",
          "[Results]") {
  const std::array instruments{InstrumentMeta{7, 5, 100, 10}};
  ResultRecorder recorder(instruments, ResultReserveEstimate{1, 3, 2, 1});
  recorder.on_order_event(
      order_event(100, OrderLogEventType::Submit, OrderState::PendingNew));
  recorder.on_order_event(
      order_event(101, OrderLogEventType::Accepted, OrderState::Open));
  recorder.on_fill(fill(102, 7, 42, Side::Buy, 1'005, 4, 6));
  recorder.on_order_event(
      order_event(102, OrderLogEventType::Fill, OrderState::PartiallyFilled));

  const FrozenResults result = recorder.freeze();
  const auto fills = result.fills();
  REQUIRE(fills.size() == 1);
  REQUIRE(fills.exchange_ts_ns.size() == fills.size());
  REQUIRE(fills.engine_ts_ns.size() == fills.size());
  REQUIRE(fills.instrument_id.size() == fills.size());
  REQUIRE(fills.client_order_id.size() == fills.size());
  REQUIRE(fills.side.size() == fills.size());
  REQUIRE(fills.price_ticks.size() == fills.size());
  REQUIRE(fills.quantity.size() == fills.size());
  REQUIRE(fills.remaining_quantity.size() == fills.size());
  REQUIRE(fills.liquidity_source.size() == fills.size());
  REQUIRE(fills.trigger_source_sequence.size() == fills.size());
  REQUIRE(fills.exchange_ts_ns[0] == 101);
  REQUIRE(fills.engine_ts_ns[0] == 102);
  REQUIRE(fills.instrument_id[0] == 7);
  REQUIRE(fills.client_order_id[0] == 42);
  REQUIRE(fills.side[0] == Side::Buy);
  REQUIRE(fills.price_ticks[0] == 1'005);
  REQUIRE(fills.quantity[0] == 4);
  REQUIRE(fills.remaining_quantity[0] == 6);
  REQUIRE(fills.liquidity_source[0] == LiquiditySource::HistoricalDisplayed);
  REQUIRE(fills.trigger_source_sequence[0] == 17);

  const auto orders = result.order_log();
  REQUIRE(orders.size() == 3);
  REQUIRE(orders.transition_sequence.size() == orders.size());
  REQUIRE(orders.instrument_id.size() == orders.size());
  REQUIRE(orders.client_order_id.size() == orders.size());
  REQUIRE(orders.event_type.size() == orders.size());
  REQUIRE(orders.previous_state.size() == orders.size());
  REQUIRE(orders.state.size() == orders.size());
  REQUIRE(orders.side.size() == orders.size());
  REQUIRE(orders.limit_price_ticks.size() == orders.size());
  REQUIRE(orders.order_quantity.size() == orders.size());
  REQUIRE(orders.filled_quantity.size() == orders.size());
  REQUIRE(orders.remaining_quantity.size() == orders.size());
  REQUIRE(orders.queue_ahead_quantity.size() == orders.size());
  REQUIRE(orders.reject_reason.size() == orders.size());
  REQUIRE(orders.event_type[0] == OrderLogEventType::Submit);
  REQUIRE(orders.event_type[1] == OrderLogEventType::Accepted);
  REQUIRE(orders.event_type[2] == OrderLogEventType::Fill);
  REQUIRE(orders.transition_sequence[2] == 102);
  REQUIRE(orders.previous_state[2] == OrderState::Open);
  REQUIRE(orders.state[2] == OrderState::PartiallyFilled);
}

TEST_CASE("FIFO PnL handles long partial close close and flip exactly",
          "[Results]") {
  const std::array instruments{InstrumentMeta{1, 1, 10, 3}};
  ResultRecorder recorder(instruments);
  recorder.on_fill(fill(10, 1, 1, Side::Buy, 100, 2));
  REQUIRE(recorder.position(1) == 2);
  REQUIRE(recorder.on_book_mark(1, 11, 100, 101));
  recorder.on_fill(fill(12, 1, 2, Side::Sell, 110, 1));
  REQUIRE(recorder.position(1) == 1);
  recorder.on_fill(fill(13, 1, 3, Side::Sell, 90, 2));
  REQUIRE(recorder.position(1) == -1);
  recorder.on_fill(fill(14, 1, 4, Side::Buy, 80, 1));
  REQUIRE(recorder.position(1) == 0);

  const auto result = recorder.freeze();
  const auto exact = result.exact_pnl();
  REQUIRE(exact.size() == 5);
  REQUIRE(exact[0].numerator == 0);
  REQUIRE(exact[0].denominator == 1);
  REQUIRE(exact[1].numerator == 3);
  REQUIRE(exact[1].denominator == 10);
  REQUIRE(exact[2].numerator == 63);
  REQUIRE(exact[2].denominator == 20);
  REQUIRE(exact[3].numerator == -63);
  REQUIRE(exact[3].denominator == 20);
  REQUIRE(exact[4].numerator == 3);
  REQUIRE(exact[4].denominator == 1);
  REQUIRE(result.pnl().total_pnl[2] == 3.15);
}

TEST_CASE("FIFO PnL handles short partial close and multiplier scale",
          "[Results]") {
  const std::array instruments{InstrumentMeta{1, 1, 100, 10}};
  ResultRecorder recorder(instruments);
  recorder.on_fill(fill(1, 1, 1, Side::Sell, 1'000, 3));
  REQUIRE(recorder.on_book_mark(1, 2, 989, 990));
  recorder.on_fill(fill(3, 1, 2, Side::Buy, 900, 2));
  recorder.on_fill(fill(4, 1, 3, Side::Buy, 1'100, 2));
  REQUIRE(recorder.position(1) == 1);

  const auto exact = recorder.freeze().exact_pnl();
  REQUIRE(exact.size() == 4);
  REQUIRE(exact[1].numerator == 63);
  REQUIRE(exact[1].denominator == 20);
  REQUIRE(exact[2].numerator == 421);
  REQUIRE(exact[2].denominator == 20);
  REQUIRE(exact[3].numerator == -21);
  REQUIRE(exact[3].denominator == 20);
}

TEST_CASE("Missing side retains stale mark and equal timestamps coalesce",
          "[Results]") {
  const std::array instruments{InstrumentMeta{1, 1, 10, 1}};
  ResultRecorder recorder(instruments);
  recorder.on_fill(fill(10, 1, 1, Side::Buy, 100, 1));
  REQUIRE(recorder.on_book_mark(1, 11, 100, 102));
  REQUIRE_FALSE(recorder.on_book_mark(1, 12, 101, std::nullopt));
  REQUIRE_FALSE(recorder.on_book_mark(1, 12, 100, 102));
  recorder.on_fill(fill(12, 1, 2, Side::Buy, 100, 1));
  REQUIRE(recorder.on_book_mark(1, 12, 102, 104));

  const auto result = recorder.freeze();
  REQUIRE(result.pnl().size() == 3);
  REQUIRE(result.pnl().engine_ts_ns[0] == 10);
  REQUIRE(result.pnl().engine_ts_ns[1] == 11);
  REQUIRE(result.pnl().engine_ts_ns[2] == 12);
  REQUIRE(result.exact_pnl()[2].numerator == 3);
  REQUIRE(result.exact_pnl()[2].denominator == 5);
}

TEST_CASE("Aggregate PnL is exact across instruments and scales", "[Results]") {
  const std::array instruments{InstrumentMeta{1, 1, 10, 1},
                               InstrumentMeta{2, 1, 4, 3}};
  ResultRecorder recorder(instruments);
  recorder.on_fill(fill(1, 1, 1, Side::Buy, 100, 1));
  recorder.on_fill(fill(2, 2, 2, Side::Sell, 40, 2));
  REQUIRE(recorder.on_book_mark(1, 3, 100, 101));
  REQUIRE(recorder.on_book_mark(2, 3, 38, 40));

  const auto result = recorder.freeze();
  const auto exact = result.exact_pnl();
  REQUIRE(exact.back().numerator == 31);
  REQUIRE(exact.back().denominator == 20);
}

TEST_CASE("Overflow and invalid marks leave ledger and columns unchanged",
          "[Results]") {
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array instruments{InstrumentMeta{1, 1, 1, maximum}};
  ResultRecorder recorder(instruments);
  recorder.on_fill(fill(1, 1, 1, Side::Buy, 1, maximum));
  const Quantity position_before = recorder.position(1);

  bool overflowed = false;
  try {
    recorder.on_fill(fill(2, 1, 2, Side::Sell, maximum, maximum));
  } catch (const ResultError &) {
    overflowed = true;
  }
  REQUIRE(overflowed);
  REQUIRE(recorder.position(1) == position_before);

  bool invalid_mark = false;
  try {
    (void)recorder.on_book_mark(1, 3, 2, 1);
  } catch (const std::invalid_argument &) {
    invalid_mark = true;
  }
  REQUIRE(invalid_mark);
  const auto result = recorder.freeze();
  REQUIRE(result.fills().size() == 1);
  REQUIRE(result.pnl().size() == 1);
}

TEST_CASE("Reserved FIFO storage has no per-fill clone or reallocation",
          "[Results]") {
  constexpr std::size_t fill_count = 4'096;
  const std::array instruments{InstrumentMeta{1, 1, 100, 2}};
  ResultRecorder recorder(
      instruments,
      ResultReserveEstimate{fill_count + 2, 0, fill_count + 2, fill_count + 1});
  REQUIRE(recorder.on_book_mark(1, 0, 99, 101));
  for (std::size_t index = 0; index < fill_count; ++index) {
    recorder.on_fill(fill(static_cast<TimestampNs>(index + 1), 1,
                          static_cast<ClOrdId>(index + 1), Side::Buy,
                          100 + static_cast<PriceTicks>(index % 7), 1));
  }
  auto stats = recorder.position_storage_stats(1);
  REQUIRE(stats.active_lots == fill_count);
  REQUIRE(stats.capacity >= fill_count);
  REQUIRE(stats.runtime_reallocations == 0);

  recorder.on_fill(fill(5'000, 1, 5'000, Side::Sell, 110,
                        static_cast<Quantity>(fill_count / 2)));
  stats = recorder.position_storage_stats(1);
  REQUIRE(stats.active_lots == fill_count / 2);
  REQUIRE(stats.runtime_reallocations == 0);
  recorder.on_fill(fill(5'001, 1, 5'001, Side::Buy, 101, 1));
  stats = recorder.position_storage_stats(1);
  REQUIRE(stats.active_lots == fill_count / 2 + 1);
  REQUIRE(stats.runtime_reallocations == 0);

  const auto result = recorder.freeze();
  REQUIRE(result.fills().size() == fill_count + 2);
  REQUIRE(result.pnl().size() == fill_count + 2);
}

TEST_CASE("Frozen empty spans and retained owner keep storage alive",
          "[Results]") {
  const std::array instruments{InstrumentMeta{1, 1, 1, 1}};
  FrozenResults retained;
  {
    auto recorder = std::make_unique<ResultRecorder>(instruments);
    retained = recorder->freeze();
    REQUIRE(retained.fills().empty());
    REQUIRE(retained.order_log().empty());
    REQUIRE(retained.pnl().empty());

    bool append_rejected = false;
    try {
      recorder->on_order_event(
          order_event(1, OrderLogEventType::Submit, OrderState::PendingNew));
    } catch (const ResultError &) {
      append_rejected = true;
    }
    REQUIRE(append_rejected);

    bool mark_rejected = false;
    try {
      (void)recorder->on_book_mark(1, 1, 1, 1);
    } catch (const ResultError &) {
      mark_rejected = true;
    }
    REQUIRE(mark_rejected);
  }
  std::vector<std::uint64_t> pressure(100'000, 7);
  REQUIRE(pressure.back() == 7);
  REQUIRE(retained.fills().empty());
  REQUIRE(retained.pnl().empty());
}

TEST_CASE("Twenty native result runs are byte-order deterministic",
          "[Results]") {
  const std::array instruments{InstrumentMeta{1, 1, 10, 3},
                               InstrumentMeta{2, 1, 100, 2}};
  std::vector<TimestampNs> baseline_times;
  std::vector<std::int64_t> baseline_numerators;
  for (int iteration = 0; iteration < 20; ++iteration) {
    ResultRecorder recorder(instruments, ResultReserveEstimate{4, 0, 8, 4});
    recorder.on_fill(fill(10, 1, 1, Side::Buy, 100, 2));
    (void)recorder.on_book_mark(1, 11, 100, 101);
    recorder.on_fill(fill(12, 2, 2, Side::Sell, 1'000, 3));
    (void)recorder.on_book_mark(2, 13, 990, 1'000);
    recorder.on_fill(fill(14, 1, 3, Side::Sell, 110, 1));
    const auto result = recorder.freeze();
    const std::vector<TimestampNs> times(result.pnl().engine_ts_ns.begin(),
                                         result.pnl().engine_ts_ns.end());
    const auto numerators = exact_numerators(result);
    if (iteration == 0) {
      baseline_times = times;
      baseline_numerators = numerators;
    } else {
      REQUIRE(times == baseline_times);
      REQUIRE(numerators == baseline_numerators);
    }
  }
}

TEST_CASE("Fees rejects and final positions are frozen for analysis",
          "[Results][ExecutionCosts]") {
  const std::array instruments{InstrumentMeta{1, 1, 100, 1}};
  ResultRecorder recorder(instruments);
  auto charged_fill = fill(10, 1, 1, Side::Buy, 100, 2);
  charged_fill.reference_price_ticks = 99;
  charged_fill.liquidity_role = LiquidityRole::Taker;
  charged_fill.slippage_ticks = 1;
  charged_fill.fee_micros = 250'000;
  charged_fill.order_submit_ts_ns = 5;
  charged_fill.order_arrival_ts_ns = 7;
  charged_fill.time_to_fill_ns = 5;
  recorder.on_fill(charged_fill);
  recorder.on_reject(
      RejectView{1, 2, RejectReason::RiskOrderSizeExceeded, 11, 11, 1});

  const auto result = recorder.freeze();
  REQUIRE(result.fills().fee_micros[0] == 250'000);
  REQUIRE(result.fills().reference_price_ticks[0] == 99);
  REQUIRE(result.fills().time_to_fill_ns[0] == 5);
  REQUIRE(result.exact_pnl()[0].numerator == -1);
  REQUIRE(result.exact_pnl()[0].denominator == 4);
  REQUIRE(result.rejects().size() == 1);
  REQUIRE(result.rejects().reason[0] == RejectReason::RiskOrderSizeExceeded);
  REQUIRE(result.final_positions().size() == 1);
  REQUIRE(result.final_positions().net_quantity[0] == 2);
  REQUIRE(result.final_positions().realized_pnl[0] == -0.25);
  REQUIRE(result.final_positions().total_pnl[0] == -0.25);
}
