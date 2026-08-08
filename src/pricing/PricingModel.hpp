#pragma once

#include "pricing/Greeks.hpp"
#include "pricing/OptionSpec.hpp"
#include "pricing/PricingInputs.hpp"

namespace cmf::pricing
{

// Public contract implemented by every pricing model (Black-Scholes, binomial,
// Monte-Carlo, ...). This is the surface the trading engine's feature generator
// will consume, so it must stay free of any backtester-specific dependencies.
class PricingModel
{
  public:
    virtual ~PricingModel() = default;

    // Returns the theoretical value and greeks for `spec` given market `inputs`.
    [[nodiscard]] virtual Greeks evaluate(const OptionSpec& spec,
                                          const PricingInputs& inputs) const = 0;
};

} // namespace cmf::pricing
