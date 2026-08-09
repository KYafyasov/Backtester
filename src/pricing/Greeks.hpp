#pragma once

#include "types/BasicTypes.hpp"

namespace cmf::pricing
{

// Theoretical value plus first/second-order risk sensitivities returned by a
// pricing model.
struct Greeks
{
    Price price = 0.0;
    double delta = 0.0; // dV / dSpot
    double gamma = 0.0; // d2V / dSpot2
    double vega = 0.0;  // dV / dVol
    double theta = 0.0; // dV / dTime
    double rho = 0.0;   // dV / dRate
};

} // namespace cmf::pricing
