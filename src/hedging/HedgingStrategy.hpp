#pragma once

#include "hedging/HedgeOrder.hpp"
#include "hedging/Position.hpp"
#include "pricing/Greeks.hpp"

#include <vector>

namespace cmf::hedging
{

// Everything a hedging strategy needs to decide on a rebalance. The greeks come
// straight from the pricing engine, which is the only cross-engine dependency.
struct HedgingContext
{
    Position position;      // the position being hedged
    pricing::Greeks greeks; // its current risk, from the pricing engine
    Price hedge_spot = 0.0; // current spot of the hedging instrument
};

// Public contract implemented by every hedging strategy (delta, delta-gamma,
// ...).
class HedgingStrategy
{
  public:
    virtual ~HedgingStrategy() = default;

    // Returns the orders needed to rebalance the hedge for `ctx`, or an empty
    // vector when the position is already within tolerance.
    [[nodiscard]] virtual std::vector<HedgeOrder>
    rebalance(const HedgingContext& ctx) const = 0;
};

} // namespace cmf::hedging
