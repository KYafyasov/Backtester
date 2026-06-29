#pragma once

#include "types/BasicTypes.hpp"

namespace cmf::hedging
{

// A held position whose risk the hedging engine is responsible for.
struct Position
{
    MarketSecurityId instrument = MktSecId::None;
    Quantity quantity = 0.0; // signed: > 0 long, < 0 short
    Price average_price = 0.0;
};

} // namespace cmf::hedging
