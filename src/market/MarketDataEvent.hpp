#pragma once

#include "core/Types.hpp"

#include <optional>

namespace cmf::market {

enum class MarketAction : std::uint8_t {
  Add,
  Cancel,
  Modify,
  Trade,
  Fill,
  Clear,
};

struct MarketDataEvent {
  TimestampNs receive_ts_ns{};
  TimestampNs exchange_ts_ns{};
  InstrumentId instrument_id{};
  ExchangeOrderId exchange_order_id{};
  Sequence source_sequence{};
  MarketAction action{MarketAction::Add};
  Side side{Side::None};
  std::optional<PriceTicks> price_ticks;
  Quantity quantity{};
  std::uint32_t flags{};

  [[nodiscard]] constexpr bool is_last_in_group() const noexcept {
    constexpr std::uint32_t last_flag = 128;
    return (flags & last_flag) != 0;
  }
};

} // namespace cmf::market
