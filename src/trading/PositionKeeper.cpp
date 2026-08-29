#include "trading/PositionKeeper.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cmf::trading {
namespace {

[[nodiscard]] bool valid_side(Side side) noexcept {
  return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] __int128 checked_multiply(__int128 left, __int128 right) {
  __int128 result{};
  if (__builtin_mul_overflow(left, right, &result)) {
    throw PositionError("realized PnL multiplication overflow");
  }
  return result;
}

[[nodiscard]] std::int64_t checked_accumulate(std::int64_t current,
                                              __int128 delta) {
  __int128 accumulated{};
  if (__builtin_add_overflow(static_cast<__int128>(current), delta,
                             &accumulated) ||
      accumulated < std::numeric_limits<std::int64_t>::min() ||
      accumulated > std::numeric_limits<std::int64_t>::max()) {
    throw PositionError("realized PnL numerator overflow");
  }
  return static_cast<std::int64_t>(accumulated);
}

} // namespace

void PositionKeeper::register_instrument(const InstrumentMeta &meta) {
  if (meta.instrument_id <= 0 || meta.tick_size_ticks <= 0 ||
      meta.price_scale <= 0 || meta.contract_multiplier <= 0) {
    throw std::invalid_argument("instrument metadata values must be positive");
  }
  const auto [iterator, inserted] =
      positions_.emplace(meta.instrument_id, Position{meta, 0, 0, 0, {}});
  (void)iterator;
  if (!inserted) {
    throw std::invalid_argument("duplicate instrument metadata");
  }
}

void PositionKeeper::apply_fill(InstrumentId instrument_id, Side side,
                                PriceTicks price, Quantity quantity,
                                std::int64_t fee_micros) {
  auto iterator = positions_.find(instrument_id);
  if (iterator == positions_.end()) {
    throw std::invalid_argument("fill references unknown instrument");
  }
  if (!valid_side(side) || price <= 0 || quantity <= 0) {
    throw std::invalid_argument("invalid fill");
  }

  auto &position = iterator->second;
  Quantity validation_remaining = quantity;
  std::int64_t validated_realized = position.realized_numerator;
  for (const auto &lot : position.lots) {
    if (validation_remaining == 0 || lot.side == side) {
      break;
    }
    const Quantity closing = std::min(validation_remaining, lot.quantity);
    const __int128 delta =
        lot.side == Side::Buy
            ? static_cast<__int128>(price) - static_cast<__int128>(lot.price)
            : static_cast<__int128>(lot.price) - static_cast<__int128>(price);
    const __int128 price_quantity =
        checked_multiply(delta, static_cast<__int128>(closing));
    const __int128 realized = checked_multiply(
        price_quantity,
        static_cast<__int128>(position.meta.contract_multiplier));
    validated_realized = checked_accumulate(validated_realized, realized);
    validation_remaining -= closing;
  }

  const Quantity signed_fill = side == Side::Buy ? quantity : -quantity;
  Quantity validated_net{};
  if (__builtin_add_overflow(position.net_quantity, signed_fill,
                             &validated_net)) {
    throw PositionError("position quantity overflow");
  }
  std::int64_t validated_fees{};
  if (__builtin_add_overflow(position.fee_micros, fee_micros,
                             &validated_fees)) {
    throw PositionError("position fee overflow");
  }

  Quantity remaining = quantity;
  while (remaining > 0 && !position.lots.empty() &&
         position.lots.front().side != side) {
    auto &lot = position.lots.front();
    const Quantity closing = std::min(remaining, lot.quantity);
    lot.quantity -= closing;
    remaining -= closing;
    if (lot.quantity == 0) {
      position.lots.pop_front();
    }
  }
  if (remaining > 0) {
    position.lots.push_back(Position::Lot{side, price, remaining});
  }
  position.realized_numerator = validated_realized;
  position.fee_micros = validated_fees;
  position.net_quantity = validated_net;
}

PositionSnapshot PositionKeeper::position(InstrumentId instrument_id) const {
  const auto iterator = positions_.find(instrument_id);
  if (iterator == positions_.end()) {
    return PositionSnapshot{instrument_id};
  }
  const auto &position = iterator->second;
  long double weighted_ticks = 0;
  long double open_quantity = 0;
  for (const auto &lot : position.lots) {
    weighted_ticks += static_cast<long double>(lot.price) *
                      static_cast<long double>(lot.quantity);
    open_quantity += static_cast<long double>(lot.quantity);
  }
  const double average =
      open_quantity == 0 ? 0.0
                         : static_cast<double>(weighted_ticks / open_quantity);
  return PositionSnapshot{
      instrument_id, position.net_quantity, average,
      static_cast<double>(position.realized_numerator) /
              static_cast<double>(position.meta.price_scale) -
          static_cast<double>(position.fee_micros) / 1'000'000.0,
      0.0};
}

} // namespace cmf::trading
