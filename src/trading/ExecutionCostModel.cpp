#include "trading/ExecutionCostModel.hpp"

#include <algorithm>
#include <limits>

namespace cmf::trading {

ExecutionCostModel::ExecutionCostModel(
    std::span<const InstrumentMeta> instruments, const BacktestConfig &config)
    : slippage_model_(config.slippage_model),
      taker_slippage_tick_count_(config.taker_slippage_tick_count),
      maker_fee_micros_per_contract_(config.maker_fee_micros_per_contract),
      taker_fee_micros_per_contract_(config.taker_fee_micros_per_contract) {
  if (slippage_model_ != SlippageModel::None &&
      slippage_model_ != SlippageModel::FixedTicks) {
    throw std::invalid_argument("unknown slippage model");
  }
  if (taker_fee_micros_per_contract_ < 0) {
    throw std::invalid_argument("taker fee must not be negative");
  }
  instruments_.reserve(instruments.size());
  for (const auto &meta : instruments) {
    if (!instruments_.emplace(meta.instrument_id, meta).second) {
      throw std::invalid_argument("duplicate execution-cost instrument");
    }
  }
}

ExecutionDecision ExecutionCostModel::apply(InstrumentId instrument_id,
                                            Side side,
                                            PriceTicks limit_price_ticks,
                                            PriceTicks reference_price_ticks,
                                            Quantity quantity,
                                            LiquiditySource source) const {
  const auto found = instruments_.find(instrument_id);
  if (found == instruments_.end()) {
    throw ExecutionCostError("execution references unknown instrument");
  }
  if ((side != Side::Buy && side != Side::Sell) || limit_price_ticks <= 0 ||
      reference_price_ticks <= 0 || quantity <= 0) {
    throw ExecutionCostError("invalid execution-cost input");
  }

  const LiquidityRole role = source == LiquiditySource::QuoteCross
                                 ? LiquidityRole::Taker
                                 : LiquidityRole::Maker;
  PriceTicks fill_price = reference_price_ticks;
  std::uint32_t applied_ticks{};
  if (role == LiquidityRole::Taker &&
      slippage_model_ == SlippageModel::FixedTicks &&
      taker_slippage_tick_count_ != 0) {
    const __int128 requested_delta =
        static_cast<__int128>(found->second.tick_size_ticks) *
        taker_slippage_tick_count_;
    const __int128 adverse =
        side == Side::Buy
            ? static_cast<__int128>(reference_price_ticks) + requested_delta
            : static_cast<__int128>(reference_price_ticks) - requested_delta;
    const __int128 bounded =
        side == Side::Buy
            ? std::min(adverse, static_cast<__int128>(limit_price_ticks))
            : std::max(adverse, static_cast<__int128>(limit_price_ticks));
    if (bounded <= 0 || bounded > std::numeric_limits<PriceTicks>::max()) {
      throw ExecutionCostError("slippage-adjusted price overflow");
    }
    fill_price = static_cast<PriceTicks>(bounded);
    const PriceTicks distance = side == Side::Buy
                                    ? fill_price - reference_price_ticks
                                    : reference_price_ticks - fill_price;
    applied_ticks =
        static_cast<std::uint32_t>(distance / found->second.tick_size_ticks);
  }

  const std::int64_t rate = role == LiquidityRole::Maker
                                ? maker_fee_micros_per_contract_
                                : taker_fee_micros_per_contract_;
  const __int128 fee = static_cast<__int128>(rate) * quantity;
  if (fee < std::numeric_limits<std::int64_t>::min() ||
      fee > std::numeric_limits<std::int64_t>::max()) {
    throw ExecutionCostError("execution fee overflow");
  }
  return {reference_price_ticks, fill_price, role, applied_ticks,
          static_cast<std::int64_t>(fee)};
}

} // namespace cmf::trading
