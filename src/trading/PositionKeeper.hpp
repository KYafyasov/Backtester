#pragma once

#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"

#include <deque>
#include <stdexcept>
#include <unordered_map>

namespace cmf::trading {

class PositionError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class PositionKeeper {
public:
  void register_instrument(const InstrumentMeta &meta);
  void apply_fill(InstrumentId instrument_id, Side side, PriceTicks price,
                  Quantity quantity, std::int64_t fee_micros = 0);
  [[nodiscard]] PositionSnapshot position(InstrumentId instrument_id) const;

private:
  struct Position {
    struct Lot {
      Side side{Side::None};
      PriceTicks price{};
      Quantity quantity{};
    };

    InstrumentMeta meta;
    Quantity net_quantity{};
    std::int64_t realized_numerator{};
    std::int64_t fee_micros{};
    std::deque<Lot> lots;
  };

  std::unordered_map<InstrumentId, Position> positions_;
};

} // namespace cmf::trading
