// Smoke tests for the Options Hedging Engine scaffold: they verify the public
// API compiles, links and dispatches, including the dependency on the pricing
// engine's greeks. The sizing logic is the hedging team's to implement.

#include "hedging/DeltaHedger.hpp"
#include "hedging/HedgeOrder.hpp"
#include "hedging/HedgingStrategy.hpp"
#include "hedging/Position.hpp"

#include "catch2/catch_all.hpp"

#include <stdexcept>

using namespace cmf;
using namespace cmf::hedging;

TEST_CASE("hedging - HedgeOrder defaults", "[hedging]")
{
    const HedgeOrder order;
    REQUIRE(order.side == Side::None);
    REQUIRE(order.type == OrderType::Market);
    REQUIRE(order.quantity == Catch::Approx(0.0));
}

TEST_CASE("hedging - strategy is reachable through the interface", "[hedging]")
{
    const DeltaHedger hedger{0.5};
    REQUIRE(hedger.delta_tolerance() == Catch::Approx(0.5));

    const HedgingStrategy& iface = hedger;

    HedgingContext ctx;
    ctx.greeks.delta = 1.0; // consumes the pricing engine's Greeks type
    ctx.hedge_spot = 100.0;

    // Scaffold: sizing is not implemented yet, so the polymorphic call is
    // expected to throw. This still exercises construction and virtual dispatch.
    REQUIRE_THROWS_AS(iface.rebalance(ctx), std::logic_error);
}
