#include "trading/PreTradeRiskEngine.hpp"

#include <limits>

namespace cmf::trading {

PreTradeRiskEngine::PreTradeRiskEngine(
    std::span<const InstrumentMeta> instruments, const BacktestConfig &config)
    : max_order_quantity_(config.max_order_quantity),
      max_abs_position_(config.max_abs_position),
      max_open_quantity_(config.max_open_quantity),
      max_active_orders_(config.max_active_orders) {
  if (max_order_quantity_ < 0 || max_abs_position_ < 0 ||
      max_open_quantity_ < 0) {
    throw std::invalid_argument("risk limits must not be negative");
  }
  instruments_.reserve(instruments.size());
  for (const auto &meta : instruments) {
    if (!instruments_.try_emplace(meta.instrument_id).second) {
      throw std::invalid_argument("duplicate risk instrument");
    }
  }
}

RejectReason PreTradeRiskEngine::check(InstrumentId instrument_id, Side side,
                                       Quantity quantity) const noexcept {
  const auto found = instruments_.find(instrument_id);
  if (found == instruments_.end() ||
      (side != Side::Buy && side != Side::Sell) || quantity <= 0) {
    return RejectReason::None;
  }
  const auto &risk = found->second;
  if (quantity > max_order_quantity_) {
    return RejectReason::RiskOrderSizeExceeded;
  }
  if (risk.active_orders >= max_active_orders_) {
    return RejectReason::RiskActiveOrderLimitExceeded;
  }
  const __int128 open_quantity =
      static_cast<__int128>(risk.reserved_buy_quantity) +
      risk.reserved_sell_quantity + quantity;
  if (open_quantity > max_open_quantity_) {
    return RejectReason::RiskOpenQuantityExceeded;
  }
  const __int128 worst_long = static_cast<__int128>(risk.net_position) +
                              risk.reserved_buy_quantity +
                              (side == Side::Buy ? quantity : 0);
  const __int128 worst_short = static_cast<__int128>(risk.net_position) -
                               risk.reserved_sell_quantity -
                               (side == Side::Sell ? quantity : 0);
  if (worst_long > max_abs_position_ || worst_short < -max_abs_position_) {
    return RejectReason::RiskPositionLimitExceeded;
  }
  return RejectReason::None;
}

void PreTradeRiskEngine::reserve(ClOrdId client_order_id,
                                 InstrumentId instrument_id, Side side,
                                 Quantity quantity) {
  const RejectReason reason = check(instrument_id, side, quantity);
  if (reason != RejectReason::None) {
    throw RiskError("cannot reserve an order rejected by risk");
  }
  auto found = instruments_.find(instrument_id);
  if (found == instruments_.end()) {
    throw RiskError("risk reservation references unknown instrument");
  }
  if (!reservations_
           .emplace(client_order_id, Reservation{instrument_id, side, quantity})
           .second) {
    throw RiskError("duplicate risk reservation");
  }
  auto &risk = found->second;
  if (side == Side::Buy) {
    risk.reserved_buy_quantity += quantity;
  } else {
    risk.reserved_sell_quantity += quantity;
  }
  ++risk.active_orders;
}

void PreTradeRiskEngine::apply_fill(ClOrdId client_order_id,
                                    Quantity quantity) {
  auto reservation = reservations_.find(client_order_id);
  if (reservation == reservations_.end() || quantity <= 0 ||
      quantity > reservation->second.remaining_quantity) {
    throw RiskError("fill does not match risk reservation");
  }
  auto &risk = instruments_.at(reservation->second.instrument_id);
  if (reservation->second.side == Side::Buy) {
    risk.reserved_buy_quantity -= quantity;
    risk.net_position += quantity;
  } else {
    risk.reserved_sell_quantity -= quantity;
    risk.net_position -= quantity;
  }
  reservation->second.remaining_quantity -= quantity;
  if (reservation->second.remaining_quantity == 0) {
    --risk.active_orders;
    reservations_.erase(reservation);
  }
}

void PreTradeRiskEngine::release(ClOrdId client_order_id) {
  const auto reservation = reservations_.find(client_order_id);
  if (reservation == reservations_.end()) {
    return;
  }
  auto &risk = instruments_.at(reservation->second.instrument_id);
  if (reservation->second.side == Side::Buy) {
    risk.reserved_buy_quantity -= reservation->second.remaining_quantity;
  } else {
    risk.reserved_sell_quantity -= reservation->second.remaining_quantity;
  }
  --risk.active_orders;
  reservations_.erase(reservation);
}

RiskSnapshot PreTradeRiskEngine::snapshot(InstrumentId instrument_id) const {
  const auto found = instruments_.find(instrument_id);
  if (found == instruments_.end()) {
    throw RiskError("risk snapshot references unknown instrument");
  }
  const auto &risk = found->second;
  return {instrument_id,
          risk.net_position,
          risk.reserved_buy_quantity,
          risk.reserved_sell_quantity,
          risk.active_orders,
          risk.net_position + risk.reserved_buy_quantity,
          risk.net_position - risk.reserved_sell_quantity};
}

} // namespace cmf::trading
