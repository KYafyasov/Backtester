#pragma once

#include "types/BasicTypes.hpp"

namespace cmf::pricing
{

enum class OptionRight : signed char
{
    None = 0,
    Call = 1,
    Put = -1,
};

// Static contract definition for a single option. Everything here is known up
// front and does not change with the market.
struct OptionSpec
{
    OptionRight right = OptionRight::None;
    Price strike = 0.0;  // strike in price units
    NanoTime expiry = 0; // expiry timestamp (ns since epoch)
};

} // namespace cmf::pricing
