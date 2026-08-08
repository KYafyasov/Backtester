// Smoke tests for the Options Pricing Engine scaffold: they verify the public
// API compiles, links and dispatches. The model math itself is the pricing
// team's to implement.

#include "pricing/BlackScholesModel.hpp"
#include "pricing/Greeks.hpp"
#include "pricing/OptionSpec.hpp"
#include "pricing/PricingInputs.hpp"
#include "pricing/PricingModel.hpp"

#include "catch2/catch_all.hpp"

#include <stdexcept>

using namespace cmf;
using namespace cmf::pricing;

TEST_CASE("pricing - OptionRight encoding", "[pricing]")
{
    REQUIRE(int(OptionRight::Call) == 1);
    REQUIRE(int(OptionRight::Put) == -1);
    REQUIRE(int(OptionRight::None) == 0);
}

TEST_CASE("pricing - value types carry their fields", "[pricing]")
{
    const OptionSpec spec{OptionRight::Call, 100.0, 1'000};
    REQUIRE(spec.right == OptionRight::Call);
    REQUIRE(spec.strike == Catch::Approx(100.0));
    REQUIRE(spec.expiry == 1'000);

    const PricingInputs in{101.0, 0.02, 0.0, 0.2, 500};
    REQUIRE(in.spot == Catch::Approx(101.0));
    REQUIRE(in.volatility == Catch::Approx(0.2));

    constexpr Greeks g{};
    REQUIRE(g.price == Catch::Approx(0.0));
    REQUIRE(g.delta == Catch::Approx(0.0));
}

TEST_CASE("pricing - model is reachable through the interface", "[pricing]")
{
    const BlackScholesModel model;
    const PricingModel& iface = model;

    const OptionSpec spec{OptionRight::Call, 100.0, 1'000};
    const PricingInputs in{101.0, 0.02, 0.0, 0.2, 500};

    // Scaffold: the math is not implemented yet, so the polymorphic call is
    // expected to throw. This still exercises construction and virtual dispatch.
    REQUIRE_THROWS_AS(iface.evaluate(spec, in), std::logic_error);
}
