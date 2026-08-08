#include "pricing/BlackScholesModel.hpp"

#include <stdexcept>

namespace cmf::pricing
{

Greeks BlackScholesModel::evaluate(const OptionSpec&, const PricingInputs&) const
{
    // TODO(pricing): implement the Black-Scholes-Merton closed form and its
    // greeks. Until then the throw documents that the model is a scaffold.
    throw std::logic_error("BlackScholesModel::evaluate not implemented");
}

} // namespace cmf::pricing
