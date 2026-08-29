#pragma once

#include "core/Types.hpp"

#include <compare>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

namespace cmf {

struct BookLevel {
  PriceTicks price{};
  Quantity quantity{};
};

struct BookUpdateView {
  InstrumentId instrument_id{};
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  Sequence sequence{};
  bool is_snapshot{};
  std::span<const BookLevel> bids;
  std::span<const BookLevel> asks;
  SourceId source_id{};
  Sequence global_market_sequence{};
};

struct TradeView {
  InstrumentId instrument_id{};
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  Sequence sequence{};
  Side aggressor_side{Side::None};
  PriceTicks price{};
  Quantity quantity{};
  SourceId source_id{};
  Sequence global_market_sequence{};
};

struct PriceCrossSignal {
  InstrumentId instrument_id{};
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  Sequence source_sequence{};
  PriceCrossSource source{PriceCrossSource::BestQuote};
  std::optional<PriceTicks> best_bid;
  std::optional<PriceTicks> best_ask;
  std::optional<PriceTicks> trade_price;
  SourceId source_id{};
  Sequence global_market_sequence{};
  Side trade_aggressor_side{Side::None};
  Quantity trade_quantity{};
};

struct FillView {
  InstrumentId instrument_id{};
  ClOrdId client_order_id{};
  Side side{Side::None};
  PriceTicks price{};
  Quantity quantity{};
  Quantity remaining_quantity{};
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  Sequence fill_sequence{};
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

struct RejectView {
  InstrumentId instrument_id{};
  ClOrdId client_order_id{};
  RejectReason reason{RejectReason::None};
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  Sequence sequence{};
};

struct NewOrderCommand {
  ClOrdId client_order_id{};
  InstrumentId instrument_id{};
  Side side{Side::None};
  PriceTicks limit_price_ticks{};
  Quantity quantity{};
  TimestampNs submit_engine_ts_ns{};
  TimestampNs scheduled_arrival_ts_ns{};
  Sequence command_sequence{};
};

struct CancelCommand {
  ClOrdId client_order_id{};
  InstrumentId instrument_id{};
  TimestampNs submit_engine_ts_ns{};
  TimestampNs scheduled_arrival_ts_ns{};
  Sequence command_sequence{};
};

struct MarketDelivery {
  InstrumentId instrument_id{};
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  Sequence source_sequence{};
  std::optional<BookUpdateView> book_update;
  std::span<const TradeView> trades;
  std::span<const PriceCrossSignal> price_cross_signals;
  SourceId source_id{};
  Sequence global_input_sequence{};
  Sequence global_market_sequence{};
};

struct ScheduledKey {
  TimestampNs scheduled_ts_ns{};
  EventPriority priority{EventPriority::MarketData};
  Sequence source_or_command_sequence{};

  auto operator<=>(const ScheduledKey &) const = default;
};

class ScheduledEvent {
public:
  using Payload = std::variant<MarketDelivery, NewOrderCommand, CancelCommand>;

  explicit ScheduledEvent(MarketDelivery delivery,
                          Sequence dispatch_sequence = 0)
      : payload_(delivery), dispatch_sequence_(dispatch_sequence) {}

  explicit ScheduledEvent(NewOrderCommand command,
                          Sequence dispatch_sequence = 0)
      : payload_(command), dispatch_sequence_(dispatch_sequence) {}

  explicit ScheduledEvent(CancelCommand command, Sequence dispatch_sequence = 0)
      : payload_(command), dispatch_sequence_(dispatch_sequence) {}

  [[nodiscard]] EventPriority priority() const {
    return std::visit(
        []<typename PayloadType>(const PayloadType &) {
          if constexpr (std::is_same_v<PayloadType, MarketDelivery>) {
            return EventPriority::MarketData;
          } else if constexpr (std::is_same_v<PayloadType, NewOrderCommand>) {
            return EventPriority::NewOrder;
          } else {
            return EventPriority::Cancel;
          }
        },
        payload_);
  }

  [[nodiscard]] ScheduledKey key() const {
    return std::visit(
        [this]<typename PayloadType>(const PayloadType &payload) {
          if constexpr (std::is_same_v<PayloadType, MarketDelivery>) {
            return ScheduledKey{payload.engine_ts_ns, priority(),
                                payload.global_market_sequence == 0
                                    ? payload.source_sequence
                                    : payload.global_market_sequence};
          } else {
            return ScheduledKey{payload.scheduled_arrival_ts_ns, priority(),
                                payload.command_sequence};
          }
        },
        payload_);
  }

  [[nodiscard]] const Payload &payload() const noexcept { return payload_; }

  [[nodiscard]] Sequence dispatch_sequence() const noexcept {
    return dispatch_sequence_;
  }

private:
  Payload payload_;
  Sequence dispatch_sequence_{};
};

struct OrderQueryRow {
  InstrumentId instrument_id{};
  ClOrdId client_order_id{};
  OrderState state{OrderState::PendingNew};
  Side side{Side::None};
  PriceTicks limit_price_ticks{};
  Quantity order_quantity{};
  Quantity filled_quantity{};
  Quantity remaining_quantity{};
  Sequence exchange_arrival_sequence{};
  Quantity queue_ahead_quantity{};
  TimestampNs submit_engine_ts_ns{};
  TimestampNs exchange_arrival_ts_ns{};
};

struct RiskSnapshot {
  InstrumentId instrument_id{};
  Quantity net_position{};
  Quantity reserved_buy_quantity{};
  Quantity reserved_sell_quantity{};
  std::uint32_t active_orders{};
  Quantity worst_case_long{};
  Quantity worst_case_short{};
};

struct PositionSnapshot {
  InstrumentId instrument_id{};
  Quantity net_quantity{};
  double average_open_price_ticks{};
  double realized_pnl{};
  double unrealized_pnl{};
};

} // namespace cmf
