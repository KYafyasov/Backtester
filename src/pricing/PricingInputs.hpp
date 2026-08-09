#pragma once

#include "types/BasicTypes.hpp"

namespace cmf::pricing
{

// Market state fed into a pricing model at valuation time. All rates and the
// volatility are annualised and expressed as continuous decimals (0.05 == 5%).
struct PricingInputs
{
    Price spot = 0.0;        // underlying spot price
    double rate = 0.0;       // risk-free rate
    double dividend = 0.0;   // continuous dividend yield
    double volatility = 0.0; // implied / forecast volatility
    NanoTime now = 0;        // valuation timestamp (ns since epoch)
};

} // namespace cmf::pricing
