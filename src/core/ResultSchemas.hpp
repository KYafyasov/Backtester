#pragma once

#include "core/Types.hpp"

namespace cmf {

struct FillResultRow {
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  InstrumentId instrument_id{};
  ClOrdId client_order_id{};
  Side side{Side::None};
  PriceTicks price_ticks{};
  Quantity quantity{};
  Quantity remaining_quantity{};
  LiquiditySource liquidity_source{LiquiditySource::HistoricalDisplayed};
  Sequence trigger_source_sequence{};
  SourceId trigger_source_id{};
  Sequence trigger_global_market_sequence{};
  PriceTicks reference_price_ticks{};
  LiquidityRole liquidity_role{LiquidityRole::Maker};
  std::uint32_t slippage_ticks{};
  std::int64_t fee_micros{};
  TimestampNs order_submit_ts_ns{};
  TimestampNs order_arrival_ts_ns{};
  TimestampNs time_to_fill_ns{};
};

struct OrderLogResultRow {
  TimestampNs engine_ts_ns{};
  Sequence transition_sequence{};
  InstrumentId instrument_id{};
  ClOrdId client_order_id{};
  OrderLogEventType event_type{OrderLogEventType::Submit};
  OrderState previous_state{OrderState::PendingNew};
  OrderState state{OrderState::PendingNew};
  Side side{Side::None};
  PriceTicks limit_price_ticks{};
  Quantity order_quantity{};
  Quantity filled_quantity{};
  Quantity remaining_quantity{};
  Quantity queue_ahead_quantity{};
  RejectReason reject_reason{RejectReason::None};
};

struct PnlPoint {
  TimestampNs engine_ts_ns{};
  double total_pnl{};
};

struct FinalPositionRow {
  InstrumentId instrument_id{};
  Quantity net_quantity{};
  double realized_pnl{};
  double unrealized_pnl{};
  double total_pnl{};
};

} // namespace cmf
