#include "hedging/DeltaHedger.hpp"

#include <stdexcept>

namespace cmf::hedging
{

std::vector<HedgeOrder> DeltaHedger::rebalance(const HedgingContext&) const
{
    // TODO(hedging): size an order in the underlying so that the net delta
    // (position delta + hedge delta) stays within delta_tolerance_. Until then
    // the throw documents that the strategy is a scaffold.
    throw std::logic_error("DeltaHedger::rebalance not implemented");
}

} // namespace cmf::hedging
