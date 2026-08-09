#pragma once

#include "hedging/HedgingStrategy.hpp"

namespace cmf::hedging
{

// Keeps net delta within a fixed tolerance by trading the underlying.
//
// Scaffold stub: the public interface and build wiring are final, but the
// sizing logic in rebalance() is not implemented yet. The hedging team fills
// it in.
class DeltaHedger final : public HedgingStrategy
{
  public:
    explicit DeltaHedger(double delta_tolerance) noexcept
        : delta_tolerance_(delta_tolerance)
    {
    }

    [[nodiscard]] std::vector<HedgeOrder>
    rebalance(const HedgingContext& ctx) const override;

    [[nodiscard]] double delta_tolerance() const noexcept
    {
        return delta_tolerance_;
    }

  private:
    double delta_tolerance_ = 0.0;
};

} // namespace cmf::hedging
