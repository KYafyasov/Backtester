#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <limits>

namespace cmf {

struct InstrumentMeta {
  InstrumentId instrument_id{};
  PriceTicks tick_size_ticks{1};
  PriceTicks price_scale{1};
  Quantity contract_multiplier{1};
};

struct BacktestConfig {
  TimestampNs market_data_latency_ns{};
  TimestampNs order_latency_ns{};
  std::uint32_t book_depth{15};
  bool allow_unverified_metadata{};
  FillModel fill_model{FillModel::FillAtTouch};
  SlippageModel slippage_model{SlippageModel::None};
  std::uint32_t taker_slippage_tick_count{};
  std::int64_t maker_fee_micros_per_contract{};
  std::int64_t taker_fee_micros_per_contract{};
  Quantity max_order_quantity{std::numeric_limits<Quantity>::max()};
  Quantity max_abs_position{std::numeric_limits<Quantity>::max()};
  Quantity max_open_quantity{std::numeric_limits<Quantity>::max()};
  std::uint32_t max_active_orders{std::numeric_limits<std::uint32_t>::max()};
};

struct DateRange {
  TimestampNs start_ts_ns{std::numeric_limits<TimestampNs>::lowest()};
  TimestampNs end_ts_ns{std::numeric_limits<TimestampNs>::max()};

  [[nodiscard]] constexpr bool
  contains_historical(TimestampNs exchange_ts_ns) const noexcept {
    return exchange_ts_ns >= start_ts_ns && exchange_ts_ns <= end_ts_ns;
  }

  [[nodiscard]] constexpr bool
  allows_command_arrival(TimestampNs scheduled_arrival_ts_ns) const noexcept {
    return scheduled_arrival_ts_ns <= end_ts_ns;
  }
};

} // namespace cmf
