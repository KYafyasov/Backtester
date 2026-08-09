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
};

struct OrderLogResultRow {
  TimestampNs engine_ts_ns{};
  InstrumentId instrument_id{};
  ClOrdId client_order_id{};
  OrderLogEventType event_type{OrderLogEventType::Submit};
  OrderState state{OrderState::PendingNew};
  Side side{Side::None};
  PriceTicks limit_price_ticks{};
  Quantity order_quantity{};
  Quantity filled_quantity{};
  Quantity remaining_quantity{};
  RejectReason reject_reason{RejectReason::None};
};

struct PnlPoint {
  TimestampNs engine_ts_ns{};
  double total_pnl{};
};

} // namespace cmf
