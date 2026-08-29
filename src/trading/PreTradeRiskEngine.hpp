#pragma once

#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"

#include <span>
#include <stdexcept>
#include <unordered_map>

namespace cmf::trading {

class RiskError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class PreTradeRiskEngine {
public:
  PreTradeRiskEngine(std::span<const InstrumentMeta> instruments,
                     const BacktestConfig &config);

  [[nodiscard]] RejectReason check(InstrumentId instrument_id, Side side,
                                   Quantity quantity) const noexcept;
  void reserve(ClOrdId client_order_id, InstrumentId instrument_id, Side side,
               Quantity quantity);
  void apply_fill(ClOrdId client_order_id, Quantity quantity);
  void release(ClOrdId client_order_id);
  [[nodiscard]] RiskSnapshot snapshot(InstrumentId instrument_id) const;

private:
  struct InstrumentRisk {
    Quantity net_position{};
    Quantity reserved_buy_quantity{};
    Quantity reserved_sell_quantity{};
    std::uint32_t active_orders{};
  };
  struct Reservation {
    InstrumentId instrument_id{};
    Side side{Side::None};
    Quantity remaining_quantity{};
  };

  Quantity max_order_quantity_{};
  Quantity max_abs_position_{};
  Quantity max_open_quantity_{};
  std::uint32_t max_active_orders_{};
  std::unordered_map<InstrumentId, InstrumentRisk> instruments_;
  std::unordered_map<ClOrdId, Reservation> reservations_;
};

} // namespace cmf::trading
