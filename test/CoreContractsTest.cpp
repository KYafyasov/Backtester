#include "common/BasicTypes.hpp"
#include "core/Contracts.hpp"

#include "MiniTest.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

using namespace cmf;

namespace {

template <typename Enum> constexpr auto encoded(Enum value) noexcept {
  return static_cast<std::underlying_type_t<Enum>>(value);
}

static_assert(sizeof(TimestampNs) == 8);
static_assert(sizeof(PriceTicks) == 8);
static_assert(sizeof(Quantity) == 8);
static_assert(sizeof(InstrumentId) == 8);
static_assert(sizeof(ClOrdId) == 8);
static_assert(sizeof(ExchangeOrderId) == 8);
static_assert(sizeof(Sequence) == 8);
static_assert(std::is_signed_v<TimestampNs>);
static_assert(std::is_signed_v<PriceTicks>);
static_assert(std::is_signed_v<Quantity>);
static_assert(std::is_signed_v<InstrumentId>);
static_assert(std::is_unsigned_v<ClOrdId>);
static_assert(std::is_unsigned_v<ExchangeOrderId>);
static_assert(std::is_unsigned_v<Sequence>);
static_assert(
    std::is_same_v<decltype(AccountCurrencyAmount::numerator), std::int64_t>);
static_assert(
    std::is_same_v<decltype(AccountCurrencyAmount::denominator), std::int64_t>);
static_assert(std::is_same_v<NanoTime, TimestampNs>);
static_assert(std::is_same_v<OrderId, ExchangeOrderId>);
static_assert(std::is_same_v<Price, PriceTicks>);
static_assert(std::is_same_v<SecurityId, InstrumentId>);
static_assert(
    std::is_same_v<decltype(InstrumentMeta::instrument_id), InstrumentId>);
static_assert(
    std::is_same_v<decltype(InstrumentMeta::tick_size_ticks), PriceTicks>);
static_assert(
    std::is_same_v<decltype(InstrumentMeta::price_scale), PriceTicks>);
static_assert(
    std::is_same_v<decltype(InstrumentMeta::contract_multiplier), Quantity>);

static_assert(std::is_same_v<std::underlying_type_t<Side>, std::int8_t>);
static_assert(std::is_same_v<std::underlying_type_t<OrderState>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<RejectReason>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<EventPriority>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<CommandType>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<OrderLogEventType>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<LiquiditySource>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<PriceCrossSource>, std::uint8_t>);

static_assert(encoded(Side::Sell) == -1);
static_assert(encoded(Side::None) == 0);
static_assert(encoded(Side::Buy) == 1);
static_assert(encoded(OrderState::PendingNew) == 0);
static_assert(encoded(OrderState::Open) == 1);
static_assert(encoded(OrderState::PartiallyFilled) == 2);
static_assert(encoded(OrderState::Filled) == 3);
static_assert(encoded(OrderState::PendingCancel) == 4);
static_assert(encoded(OrderState::Cancelled) == 5);
static_assert(encoded(OrderState::Rejected) == 6);
static_assert(encoded(RejectReason::None) == 0);
static_assert(encoded(RejectReason::UnknownInstrument) == 1);
static_assert(encoded(RejectReason::InvalidSide) == 2);
static_assert(encoded(RejectReason::NonPositiveQuantity) == 3);
static_assert(encoded(RejectReason::InvalidPrice) == 4);
static_assert(encoded(RejectReason::TickMisalignment) == 5);
static_assert(encoded(RejectReason::DuplicateClientOrderId) == 6);
static_assert(encoded(RejectReason::UnsupportedOrderType) == 7);
static_assert(encoded(RejectReason::UnsupportedTimeInForce) == 8);
static_assert(encoded(RejectReason::UnknownOrder) == 9);
static_assert(encoded(RejectReason::AlreadyTerminal) == 10);
static_assert(encoded(RejectReason::RiskOrderSizeExceeded) == 11);
static_assert(encoded(RejectReason::RiskPositionLimitExceeded) == 12);
static_assert(encoded(RejectReason::RiskOpenQuantityExceeded) == 13);
static_assert(encoded(RejectReason::RiskActiveOrderLimitExceeded) == 14);
static_assert(encoded(EventPriority::MarketData) == 0);
static_assert(encoded(EventPriority::NewOrder) == 1);
static_assert(encoded(EventPriority::Cancel) == 2);
static_assert(encoded(CommandType::NewOrder) == 0);
static_assert(encoded(CommandType::Cancel) == 1);
static_assert(encoded(OrderLogEventType::Submit) == 0);
static_assert(encoded(OrderLogEventType::Accepted) == 1);
static_assert(encoded(OrderLogEventType::Fill) == 2);
static_assert(encoded(OrderLogEventType::CancelRequest) == 3);
static_assert(encoded(OrderLogEventType::Cancelled) == 4);
static_assert(encoded(OrderLogEventType::Reject) == 5);
static_assert(encoded(LiquiditySource::HistoricalDisplayed) == 0);
static_assert(encoded(LiquiditySource::QuoteCross) == 1);
static_assert(encoded(LiquiditySource::TradeCross) == 2);
static_assert(encoded(PriceCrossSource::BestQuote) == 0);
static_assert(encoded(PriceCrossSource::Trade) == 1);
static_assert(encoded(FillModel::FillAtTouch) == 0);
static_assert(encoded(FillModel::QueueAware) == 1);
static_assert(encoded(SlippageModel::None) == 0);
static_assert(encoded(SlippageModel::FixedTicks) == 1);
static_assert(encoded(LiquidityRole::Maker) == 0);
static_assert(encoded(LiquidityRole::Taker) == 1);

static_assert(std::is_trivially_copyable_v<BookLevel>);
static_assert(std::is_trivially_copyable_v<AccountCurrencyAmount>);
static_assert(std::is_trivially_copyable_v<BookUpdateView>);
static_assert(std::is_trivially_copyable_v<TradeView>);
static_assert(std::is_trivially_copyable_v<PriceCrossSignal>);
static_assert(std::is_trivially_copyable_v<FillView>);
static_assert(std::is_trivially_copyable_v<RejectView>);
static_assert(std::is_trivially_copyable_v<NewOrderCommand>);
static_assert(std::is_trivially_copyable_v<CancelCommand>);
static_assert(std::is_trivially_copyable_v<MarketDelivery>);
static_assert(std::is_trivially_copyable_v<ScheduledKey>);
static_assert(std::is_trivially_copyable_v<ScheduledEvent>);
static_assert(std::is_trivially_copyable_v<OrderQueryRow>);
static_assert(std::is_trivially_copyable_v<RiskSnapshot>);
static_assert(std::is_trivially_copyable_v<PositionSnapshot>);
static_assert(std::is_trivially_copyable_v<FillResultRow>);
static_assert(std::is_trivially_copyable_v<OrderLogResultRow>);
static_assert(std::is_trivially_copyable_v<PnlPoint>);
static_assert(std::is_aggregate_v<InstrumentMeta>);
static_assert(std::is_aggregate_v<AccountCurrencyAmount>);
static_assert(std::is_aggregate_v<BacktestConfig>);
static_assert(std::is_aggregate_v<DateRange>);
static_assert(std::is_aggregate_v<BookUpdateView>);
static_assert(std::is_aggregate_v<TradeView>);
static_assert(std::is_aggregate_v<PriceCrossSignal>);
static_assert(std::is_aggregate_v<FillView>);
static_assert(std::is_aggregate_v<RejectView>);
static_assert(std::is_aggregate_v<NewOrderCommand>);
static_assert(std::is_aggregate_v<CancelCommand>);
static_assert(std::is_aggregate_v<MarketDelivery>);
static_assert(!std::is_constructible_v<ScheduledEvent, ScheduledEvent::Payload,
                                       Sequence>);
static_assert(std::is_aggregate_v<FillResultRow>);
static_assert(std::is_aggregate_v<OrderLogResultRow>);
static_assert(std::is_aggregate_v<PnlPoint>);
static_assert(std::is_aggregate_v<FinalPositionRow>);

static_assert(
    std::is_same_v<decltype(FillResultRow::exchange_ts_ns), TimestampNs>);
static_assert(
    std::is_same_v<decltype(FillResultRow::engine_ts_ns), TimestampNs>);
static_assert(
    std::is_same_v<decltype(FillResultRow::instrument_id), InstrumentId>);
static_assert(
    std::is_same_v<decltype(FillResultRow::client_order_id), ClOrdId>);
static_assert(std::is_same_v<decltype(FillResultRow::side), Side>);
static_assert(std::is_same_v<decltype(FillResultRow::price_ticks), PriceTicks>);
static_assert(std::is_same_v<decltype(FillResultRow::quantity), Quantity>);
static_assert(
    std::is_same_v<decltype(FillResultRow::remaining_quantity), Quantity>);
static_assert(
    std::is_same_v<decltype(FillResultRow::liquidity_source), LiquiditySource>);
static_assert(
    std::is_same_v<decltype(FillResultRow::trigger_source_sequence), Sequence>);

static_assert(
    std::is_same_v<decltype(OrderLogResultRow::engine_ts_ns), TimestampNs>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::transition_sequence), Sequence>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::instrument_id), InstrumentId>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::client_order_id), ClOrdId>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::event_type), OrderLogEventType>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::previous_state), OrderState>);
static_assert(std::is_same_v<decltype(OrderLogResultRow::state), OrderState>);
static_assert(std::is_same_v<decltype(OrderLogResultRow::side), Side>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::limit_price_ticks), PriceTicks>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::order_quantity), Quantity>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::filled_quantity), Quantity>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::remaining_quantity), Quantity>);
static_assert(std::is_same_v<decltype(OrderLogResultRow::queue_ahead_quantity),
                             Quantity>);
static_assert(
    std::is_same_v<decltype(OrderLogResultRow::reject_reason), RejectReason>);

static_assert(std::is_same_v<decltype(PnlPoint::engine_ts_ns), TimestampNs>);
static_assert(std::is_same_v<decltype(PnlPoint::total_pnl), double>);

} // namespace

TEST_CASE("Core contracts - config and date policies", "[CoreContracts]") {
  const BacktestConfig config;
  REQUIRE(config.market_data_latency_ns == 0);
  REQUIRE(config.order_latency_ns == 0);
  REQUIRE(config.book_depth == 15);
  REQUIRE(config.fill_model == FillModel::FillAtTouch);

  const InstrumentMeta default_instrument;
  REQUIRE(default_instrument.tick_size_ticks == 1);
  REQUIRE(default_instrument.price_scale == 1);
  REQUIRE(default_instrument.contract_multiplier == 1);

  const BacktestConfig invalid_config{-1, -2, 0};
  REQUIRE(invalid_config.market_data_latency_ns == -1);
  REQUIRE(invalid_config.order_latency_ns == -2);
  REQUIRE(invalid_config.book_depth == 0);

  const DateRange range{100, 200};
  REQUIRE_FALSE(range.contains_historical(99));
  REQUIRE(range.contains_historical(100));
  REQUIRE(range.contains_historical(200));
  REQUIRE_FALSE(range.contains_historical(201));
  REQUIRE(range.allows_command_arrival(200));
  REQUIRE_FALSE(range.allows_command_arrival(201));

  const DateRange full_range;
  REQUIRE(full_range.contains_historical(
      std::numeric_limits<TimestampNs>::lowest()));
  REQUIRE(
      full_range.contains_historical(std::numeric_limits<TimestampNs>::max()));
}

TEST_CASE("Core contracts - scheduled key order", "[CoreContracts]") {
  std::vector<ScheduledKey> keys{
      {101, EventPriority::MarketData, 1}, {100, EventPriority::Cancel, 1},
      {100, EventPriority::NewOrder, 2},   {100, EventPriority::MarketData, 2},
      {100, EventPriority::NewOrder, 1},   {100, EventPriority::MarketData, 1},
  };

  std::sort(keys.begin(), keys.end());

  const std::vector<ScheduledKey> expected{
      {100, EventPriority::MarketData, 1}, {100, EventPriority::MarketData, 2},
      {100, EventPriority::NewOrder, 1},   {100, EventPriority::NewOrder, 2},
      {100, EventPriority::Cancel, 1},     {101, EventPriority::MarketData, 1},
  };
  REQUIRE(keys == expected);
}

TEST_CASE("Core contracts - scheduled payloads round trip", "[CoreContracts]") {
  const std::array<BookLevel, 1> bids{{{100, 5}}};
  const std::array<BookLevel, 1> asks{{{101, 6}}};
  const std::array<TradeView, 2> trades{{
      {7, 1'000, 1'075, 31, Side::Buy, 101, 2},
      {7, 1'001, 1'075, 32, Side::Sell, 100, 3},
  }};
  const std::array<PriceCrossSignal, 2> signals{{
      {7, 1'000, 1'075, 31, PriceCrossSource::BestQuote, 100, 101,
       std::nullopt},
      {7, 1'001, 1'075, 32, PriceCrossSource::Trade, std::nullopt, std::nullopt,
       100},
  }};
  const BookUpdateView book{7, 1'001, 1'075, 33, false, bids, asks};
  const MarketDelivery delivery{7, 1'001, 1'075, 33, book, trades, signals};
  const NewOrderCommand new_order{41, 7, Side::Buy, 101, 5, 1'050, 1'075, 40};
  const CancelCommand cancel{41, 7, 1'060, 1'075, 50};

  const ScheduledEvent market_event{delivery, 101};
  const ScheduledEvent new_event{new_order, 102};
  const ScheduledEvent cancel_event{cancel, 103};
  const ScheduledKey expected_market_key{1'075, EventPriority::MarketData, 33};
  const ScheduledKey expected_new_key{1'075, EventPriority::NewOrder, 40};
  const ScheduledKey expected_cancel_key{1'075, EventPriority::Cancel, 50};

  REQUIRE(market_event.priority() == EventPriority::MarketData);
  REQUIRE(new_event.priority() == EventPriority::NewOrder);
  REQUIRE(cancel_event.priority() == EventPriority::Cancel);
  REQUIRE(market_event.key() == expected_market_key);
  REQUIRE(new_event.key() == expected_new_key);
  REQUIRE(cancel_event.key() == expected_cancel_key);
  REQUIRE(market_event.dispatch_sequence() == 101);
  REQUIRE(new_event.dispatch_sequence() == 102);
  REQUIRE(cancel_event.dispatch_sequence() == 103);

  const auto &market_payload = std::get<MarketDelivery>(market_event.payload());
  REQUIRE(market_payload.instrument_id == delivery.instrument_id);
  REQUIRE(market_payload.exchange_ts_ns == delivery.exchange_ts_ns);
  REQUIRE(market_payload.engine_ts_ns == delivery.engine_ts_ns);
  REQUIRE(market_payload.source_sequence == delivery.source_sequence);
  REQUIRE(market_payload.book_update.has_value());
  REQUIRE(market_payload.book_update->instrument_id == book.instrument_id);
  REQUIRE(market_payload.book_update->exchange_ts_ns == book.exchange_ts_ns);
  REQUIRE(market_payload.book_update->engine_ts_ns == book.engine_ts_ns);
  REQUIRE(market_payload.book_update->sequence == book.sequence);
  REQUIRE(market_payload.book_update->is_snapshot == book.is_snapshot);
  REQUIRE(market_payload.book_update->bids.data() == bids.data());
  REQUIRE(market_payload.book_update->bids.size() == bids.size());
  REQUIRE(market_payload.book_update->asks.data() == asks.data());
  REQUIRE(market_payload.book_update->asks.size() == asks.size());
  REQUIRE(market_payload.trades.data() == trades.data());
  REQUIRE(market_payload.trades.size() == trades.size());
  REQUIRE(market_payload.trades[0].instrument_id == trades[0].instrument_id);
  REQUIRE(market_payload.trades[0].exchange_ts_ns == trades[0].exchange_ts_ns);
  REQUIRE(market_payload.trades[0].engine_ts_ns == trades[0].engine_ts_ns);
  REQUIRE(market_payload.trades[0].sequence == trades[0].sequence);
  REQUIRE(market_payload.trades[0].aggressor_side == trades[0].aggressor_side);
  REQUIRE(market_payload.trades[0].price == trades[0].price);
  REQUIRE(market_payload.trades[0].quantity == trades[0].quantity);
  REQUIRE(market_payload.trades[1].sequence == trades[1].sequence);
  REQUIRE(market_payload.price_cross_signals.data() == signals.data());
  REQUIRE(market_payload.price_cross_signals.size() == signals.size());
  REQUIRE(market_payload.price_cross_signals[0].source ==
          PriceCrossSource::BestQuote);
  REQUIRE(market_payload.price_cross_signals[0].best_bid == 100);
  REQUIRE(market_payload.price_cross_signals[0].best_ask == 101);
  REQUIRE_FALSE(market_payload.price_cross_signals[0].trade_price.has_value());
  REQUIRE(market_payload.price_cross_signals[1].source ==
          PriceCrossSource::Trade);
  REQUIRE(market_payload.price_cross_signals[1].trade_price == 100);

  const auto &new_payload = std::get<NewOrderCommand>(new_event.payload());
  REQUIRE(new_payload.client_order_id == new_order.client_order_id);
  REQUIRE(new_payload.instrument_id == new_order.instrument_id);
  REQUIRE(new_payload.side == new_order.side);
  REQUIRE(new_payload.limit_price_ticks == new_order.limit_price_ticks);
  REQUIRE(new_payload.quantity == new_order.quantity);
  REQUIRE(new_payload.submit_engine_ts_ns == new_order.submit_engine_ts_ns);
  REQUIRE(new_payload.scheduled_arrival_ts_ns ==
          new_order.scheduled_arrival_ts_ns);
  REQUIRE(new_payload.command_sequence == new_order.command_sequence);

  const auto &cancel_payload = std::get<CancelCommand>(cancel_event.payload());
  REQUIRE(cancel_payload.client_order_id == cancel.client_order_id);
  REQUIRE(cancel_payload.instrument_id == cancel.instrument_id);
  REQUIRE(cancel_payload.submit_engine_ts_ns == cancel.submit_engine_ts_ns);
  REQUIRE(cancel_payload.scheduled_arrival_ts_ns ==
          cancel.scheduled_arrival_ts_ns);
  REQUIRE(cancel_payload.command_sequence == cancel.command_sequence);

  std::vector<ScheduledKey> scheduled_keys{cancel_event.key(), new_event.key(),
                                           market_event.key()};
  std::sort(scheduled_keys.begin(), scheduled_keys.end());
  REQUIRE(scheduled_keys[0].priority == EventPriority::MarketData);
  REQUIRE(scheduled_keys[1].priority == EventPriority::NewOrder);
  REQUIRE(scheduled_keys[2].priority == EventPriority::Cancel);
}

TEST_CASE("Core contracts - callback and command aggregates",
          "[CoreContracts]") {
  const std::array<BookLevel, 1> bids{{{100, 5}}};
  const std::array<BookLevel, 1> asks{{{101, 6}}};
  const BookUpdateView book{7, 1'000, 1'050, 11, true, bids, asks};
  const TradeView trade{7, 1'001, 1'051, 12, Side::Buy, 101, 2};
  const FillView fill{7, 41,    Side::Buy, 101, 2,
                      3, 1'002, 1'052,     13,  LiquiditySource::TradeCross,
                      12};
  const RejectView reject{7, 42, RejectReason::InvalidPrice, 1'003, 1'053, 14};
  const NewOrderCommand new_order{41, 7, Side::Buy, 101, 5, 1'050, 1'075, 21};
  const CancelCommand cancel{41, 7, 1'060, 1'085, 22};

  REQUIRE(book.instrument_id == 7);
  REQUIRE(book.exchange_ts_ns == 1'000);
  REQUIRE(book.engine_ts_ns == 1'050);
  REQUIRE(book.sequence == 11);
  REQUIRE(book.bids.front().quantity == 5);
  REQUIRE(trade.instrument_id == 7);
  REQUIRE(trade.exchange_ts_ns == 1'001);
  REQUIRE(trade.engine_ts_ns == 1'051);
  REQUIRE(trade.sequence == 12);
  REQUIRE(fill.instrument_id == 7);
  REQUIRE(fill.exchange_ts_ns == 1'002);
  REQUIRE(fill.engine_ts_ns == 1'052);
  REQUIRE(fill.fill_sequence == 13);
  REQUIRE(fill.liquidity_source == LiquiditySource::TradeCross);
  REQUIRE(fill.trigger_source_sequence == 12);
  REQUIRE(reject.instrument_id == 7);
  REQUIRE(reject.exchange_ts_ns == 1'003);
  REQUIRE(reject.engine_ts_ns == 1'053);
  REQUIRE(reject.sequence == 14);
  REQUIRE(new_order.instrument_id == 7);
  REQUIRE(new_order.submit_engine_ts_ns == 1'050);
  REQUIRE(new_order.scheduled_arrival_ts_ns == 1'075);
  REQUIRE(new_order.command_sequence == 21);
  REQUIRE(cancel.instrument_id == 7);
  REQUIRE(cancel.submit_engine_ts_ns == 1'060);
  REQUIRE(cancel.scheduled_arrival_ts_ns == 1'085);
  REQUIRE(cancel.command_sequence == 22);
}

TEST_CASE("Core contracts - remaining value aggregates", "[CoreContracts]") {
  const InstrumentMeta instrument{7, 5, 10'000, 100};
  const PriceTicks delta_price_ticks = 25;
  const Quantity signed_quantity = 2;
  const AccountCurrencyAmount exact_pnl{5'000, 10'000};
  const AccountCurrencyAmount midpoint_ticks{201, 2};
  const OrderQueryRow order{
      7, 41, OrderState::PartiallyFilled, Side::Buy, 101, 5, 2, 3, 21};
  const PositionSnapshot position{7, 2, 101.0, 0.0, 4.0};
  const FillResultRow fill_result{
      1'075, 1'075,     7,
      41,    Side::Buy, 101,
      2,     3,         LiquiditySource::HistoricalDisplayed,
      72};
  const OrderLogResultRow order_log{1'075,
                                    1,
                                    7,
                                    41,
                                    OrderLogEventType::Fill,
                                    OrderState::Open,
                                    OrderState::PartiallyFilled,
                                    Side::Buy,
                                    101,
                                    5,
                                    2,
                                    3,
                                    0,
                                    RejectReason::None};
  const PnlPoint pnl{1'075, 4.0};

  REQUIRE(instrument.instrument_id == 7);
  REQUIRE(instrument.tick_size_ticks == 5);
  REQUIRE(instrument.price_scale == 10'000);
  REQUIRE(instrument.contract_multiplier == 100);
  REQUIRE(exact_pnl.numerator ==
          delta_price_ticks * signed_quantity * instrument.contract_multiplier);
  REQUIRE(exact_pnl.denominator == instrument.price_scale);
  REQUIRE(midpoint_ticks.numerator == 100 + 101);
  REQUIRE(midpoint_ticks.denominator == 2);
  REQUIRE(order.remaining_quantity == 3);
  REQUIRE(position.net_quantity == 2);
  REQUIRE(fill_result.liquidity_source == LiquiditySource::HistoricalDisplayed);
  REQUIRE(fill_result.trigger_source_sequence == 72);
  REQUIRE(order_log.event_type == OrderLogEventType::Fill);
  REQUIRE(pnl.total_pnl == 4.0);
}
