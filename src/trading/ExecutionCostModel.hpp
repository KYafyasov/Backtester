#pragma once

#include "core/BacktestConfig.hpp"

#include <span>
#include <stdexcept>
#include <unordered_map>

namespace cmf::trading {

class ExecutionCostError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct ExecutionDecision {
  PriceTicks reference_price_ticks{};
  PriceTicks fill_price_ticks{};
  LiquidityRole liquidity_role{LiquidityRole::Maker};
  std::uint32_t slippage_ticks{};
  std::int64_t fee_micros{};
};

class ExecutionCostModel {
public:
  ExecutionCostModel(std::span<const InstrumentMeta> instruments,
                     const BacktestConfig &config);

  [[nodiscard]] ExecutionDecision apply(InstrumentId instrument_id, Side side,
                                        PriceTicks limit_price_ticks,
                                        PriceTicks reference_price_ticks,
                                        Quantity quantity,
                                        LiquiditySource source) const;

private:
  SlippageModel slippage_model_{};
  std::uint32_t taker_slippage_tick_count_{};
  std::int64_t maker_fee_micros_per_contract_{};
  std::int64_t taker_fee_micros_per_contract_{};
  std::unordered_map<InstrumentId, InstrumentMeta> instruments_;
};

} // namespace cmf::trading
