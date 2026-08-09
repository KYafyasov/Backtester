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
