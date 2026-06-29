#pragma once

#include "types/BasicTypes.hpp"

namespace cmf::hedging
{

// An order proposed by a hedging strategy to bring risk back within target.
struct HedgeOrder
{
    MarketSecurityId instrument = MktSecId::None;
    Side side = Side::None;
    Quantity quantity = 0.0;
    OrderType type = OrderType::Market;
    Price limit_price = 0.0; // only used when type == OrderType::Limit
};

} // namespace cmf::hedging
