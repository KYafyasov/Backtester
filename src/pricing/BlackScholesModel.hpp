#pragma once

#include "pricing/PricingModel.hpp"

namespace cmf::pricing
{

// Black-Scholes-Merton closed-form model for European options.
//
// Scaffold stub: the public interface and build wiring are final, but the math
// in evaluate() is not implemented yet. The pricing team fills it in.
class BlackScholesModel final : public PricingModel
{
  public:
    [[nodiscard]] Greeks evaluate(const OptionSpec& spec,
                                  const PricingInputs& inputs) const override;
};

} // namespace cmf::pricing
