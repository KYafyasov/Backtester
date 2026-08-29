#include "market/HistoricalLOBStore.hpp"
#include "scheduler/SchedulerRuntime.hpp"
#include "trading/ExecutionCostModel.hpp"
#include "trading/PreTradeRiskEngine.hpp"
#include "trading/TradingEngine.hpp"

#include "MiniTest.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace cmf;
using namespace cmf::market;
using namespace cmf::scheduler;
using namespace cmf::trading;

MarketDataEvent market_event(Sequence seq, TimestampNs time,
                             ExchangeOrderId order_id, MarketAction action,
                             Side side, PriceTicks price, Quantity quantity) {
  return MarketDataEvent{time,   time, 1,     order_id, seq,
                         action, side, price, quantity, 128};
}

struct RecordingStrategy : Strategy {
  std::vector<FillView> fills;
  std::vector<RejectView> rejects;
  std::vector<PositionSnapshot> positions_in_fill;
  std::vector<std::size_t> open_counts_in_fill;

  void on_fill(const FillView &fill, StrategyContext &context) override {
    fills.push_back(fill);
    positions_in_fill.push_back(context.position(fill.instrument_id));
    open_counts_in_fill.push_back(
        context.open_orders(fill.instrument_id).size());
  }

  void on_reject(const RejectView &reject, StrategyContext &) override {
    rejects.push_back(reject);
  }
};

struct RecordingRecorder final : Recorder {
  std::vector<OrderLogResultRow> orders;
  std::vector<FillResultRow> fills;
  std::vector<RejectView> rejects;

  void on_order_event(const OrderLogResultRow &row) override {
    orders.push_back(row);
  }
  void on_fill(const FillResultRow &row) override { fills.push_back(row); }
  void on_reject(const RejectView &row) override { rejects.push_back(row); }
};

struct SubmitOnFirstMarket : RecordingStrategy {
  ClOrdId order_id{};
  Side side{Side::Buy};
  PriceTicks price{102};
  Quantity quantity{10};
  bool submitted{};

  void on_book_update(const BookUpdateView &,
                      StrategyContext &context) override {
    if (!submitted) {
      submitted = true;
      order_id = context.submit_limit(1, side, price, quantity);
    }
  }
};

BookUpdateView empty_book_view(TimestampNs time, Sequence seq) {
  return BookUpdateView{1, time, time, seq, false, {}, {}};
}

PriceCrossSignal quote_signal(TimestampNs time, Sequence seq,
                              std::optional<PriceTicks> best_bid,
                              std::optional<PriceTicks> best_ask,
                              InstrumentId instrument_id = 1) {
  return PriceCrossSignal{
      instrument_id, time,     time,        seq, PriceCrossSource::BestQuote,
      best_bid,      best_ask, std::nullopt};
}

PriceCrossSignal trade_signal(TimestampNs time, Sequence seq, PriceTicks price,
                              InstrumentId instrument_id = 1) {
  return PriceCrossSignal{
      instrument_id,           time,         time,         seq,
      PriceCrossSource::Trade, std::nullopt, std::nullopt, price};
}

const std::array<InstrumentMeta, 1> instruments{InstrumentMeta{1, 1, 100, 10}};

} // namespace

TEST_CASE("Trading engine validates instrument metadata", "[Trading]") {
  HistoricalLOBStore books;
  RecordingStrategy strategy;
  RecordingRecorder recorder;
  bool rejected = false;
  try {
    const std::array invalid{InstrumentMeta{1, 0, 100, 1}};
    TradingEngine engine(invalid, BacktestConfig{0, 1, 1}, books, strategy,
                         recorder);
    (void)engine;
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  REQUIRE(rejected);
}

TEST_CASE("Trading engine requires causal positive order latency",
          "[Trading]") {
  HistoricalLOBStore books;
  RecordingStrategy strategy;
  RecordingRecorder recorder;
  for (const TimestampNs invalid_latency : {TimestampNs{0}, TimestampNs{-1}}) {
    bool rejected = false;
    try {
      TradingEngine engine(instruments, BacktestConfig{0, invalid_latency, 1},
                           books, strategy, recorder);
      (void)engine;
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    REQUIRE(rejected);
  }
}

TEST_CASE("Queue-aware fills drive observable partial lifecycle transitions",
          "[Trading][QueueAware]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 50, MarketAction::Add, Side::Buy, 100, 5));

  SubmitOnFirstMarket strategy;
  strategy.price = 100;
  strategy.quantity = 4;
  RecordingRecorder recorder;
  TradingEngine engine(instruments,
                       BacktestConfig{0, 1, 1, false, FillModel::QueueAware},
                       books, strategy, recorder);

  const auto initial_book = empty_book_view(100, 10);
  const std::array first_trade{
      PriceCrossSignal{1, 200, 200, 20, PriceCrossSource::Trade, std::nullopt,
                       std::nullopt, 100, 0, 0, Side::Sell, 7}};
  const std::array final_trade{
      PriceCrossSignal{1, 300, 300, 30, PriceCrossSource::Trade, std::nullopt,
                       std::nullopt, 100, 0, 0, Side::Sell, 2}};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 10, initial_book, {}, {}}},
      ScheduledEvent{
          MarketDelivery{1, 200, 200, 20, std::nullopt, {}, first_trade}},
      ScheduledEvent{
          MarketDelivery{1, 300, 300, 30, std::nullopt, {}, final_trade}},
  };
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 2);
  REQUIRE(strategy.fills[0].quantity == 2);
  REQUIRE(strategy.fills[0].remaining_quantity == 2);
  REQUIRE(strategy.fills[1].quantity == 2);
  REQUIRE(strategy.fills[1].remaining_quantity == 0);
  REQUIRE(strategy.open_counts_in_fill[0] == 1);
  REQUIRE(strategy.open_counts_in_fill[1] == 0);

  REQUIRE(recorder.orders.size() == 4);
  REQUIRE(recorder.orders[0].transition_sequence == 1);
  REQUIRE(recorder.orders[0].state == OrderState::PendingNew);
  REQUIRE(recorder.orders[1].previous_state == OrderState::PendingNew);
  REQUIRE(recorder.orders[1].state == OrderState::Open);
  REQUIRE(recorder.orders[1].queue_ahead_quantity == 5);
  REQUIRE(recorder.orders[2].previous_state == OrderState::Open);
  REQUIRE(recorder.orders[2].state == OrderState::PartiallyFilled);
  REQUIRE(recorder.orders[2].queue_ahead_quantity == 0);
  REQUIRE(recorder.orders[3].previous_state == OrderState::PartiallyFilled);
  REQUIRE(recorder.orders[3].state == OrderState::Filled);
  REQUIRE(recorder.orders[3].transition_sequence == 4);
}

TEST_CASE("Delayed order fully fills at best quote without volume cap",
          "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 101, MarketAction::Add, Side::Sell, 101, 4));
  books.apply(market_event(2, 2, 102, MarketAction::Add, Side::Sell, 102, 6));

  SubmitOnFirstMarket strategy;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 25, 1}, books, strategy,
                       recorder);
  const std::array<BookLevel, 1> asks{{BookLevel{101, 10}}};
  const BookUpdateView view{1, 100, 100, 3, false, {}, asks};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 3, view, {}, {}}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.order_id == 1);
  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].engine_ts_ns == 125);
  REQUIRE(strategy.fills[0].price == 101);
  REQUIRE(strategy.fills[0].quantity == 10);
  REQUIRE(strategy.fills[0].liquidity_source == LiquiditySource::QuoteCross);
  REQUIRE(strategy.fills[0].trigger_source_sequence == 2);
  REQUIRE(recorder.fills[0].liquidity_source == LiquiditySource::QuoteCross);
  REQUIRE(recorder.fills[0].trigger_source_sequence == 2);
  REQUIRE(strategy.positions_in_fill.back().net_quantity == 10);
  REQUIRE(strategy.open_counts_in_fill.back() == 0);
  REQUIRE(runtime.processed_sequence() == 2);
}

TEST_CASE("Resting oversized order fully fills on the first quote cross",
          "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 50, MarketAction::Add, Side::Sell, 101, 5));
  SubmitOnFirstMarket strategy;
  strategy.price = 100;
  strategy.quantity = 8;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 1, 1}, books, strategy,
                       recorder);
  const auto view100 = empty_book_view(100, 10);
  const auto view200 = empty_book_view(200, 20);
  const std::array cross{quote_signal(200, 20, std::nullopt, 100)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 10, view100, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 200, 200, 20, view200, {}, cross}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].quantity == 8);
  REQUIRE(strategy.fills[0].engine_ts_ns == 200);
  REQUIRE(strategy.fills[0].price == 100);
  REQUIRE(strategy.fills[0].trigger_source_sequence == 20);
  REQUIRE(engine.position(1).net_quantity == 8);
}

TEST_CASE("Only post-arrival same-instrument trade cross fully fills",
          "[Trading]") {
  HistoricalLOBStore books;
  struct TradeStrategy final : SubmitOnFirstMarket {
    std::vector<std::size_t> fills_before_trade;

    TradeStrategy() {
      price = 100;
      quantity = 50;
    }

    void on_trade(const TradeView &, StrategyContext &) override {
      fills_before_trade.push_back(fills.size());
    }
  } strategy;
  RecordingRecorder recorder;
  const std::array two_instruments{InstrumentMeta{1, 1, 100, 10},
                                   InstrumentMeta{2, 1, 100, 10}};
  TradingEngine engine(two_instruments, BacktestConfig{0, 5, 1}, books,
                       strategy, recorder);

  const auto initial_view = empty_book_view(100, 1);
  const std::array early_trade{TradeView{1, 102, 102, 2, Side::Buy, 99, 1}};
  const std::array early_signal{trade_signal(102, 2, 99)};
  const std::array other_trade{TradeView{2, 106, 106, 3, Side::Sell, 99, 1}};
  const std::array other_signal{trade_signal(106, 3, 99, 2)};
  const std::array winning_trade{TradeView{1, 110, 110, 4, Side::None, 100, 1}};
  const std::array winning_signal{trade_signal(110, 4, 100)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, initial_view, {}, {}}},
      ScheduledEvent{
          MarketDelivery{1, 102, 102, 2, {}, early_trade, early_signal}},
      ScheduledEvent{
          MarketDelivery{2, 106, 106, 3, {}, other_trade, other_signal}},
      ScheduledEvent{
          MarketDelivery{1, 110, 110, 4, {}, winning_trade, winning_signal}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].quantity == 50);
  REQUIRE(strategy.fills[0].price == 100);
  REQUIRE(strategy.fills[0].engine_ts_ns == 110);
  REQUIRE(strategy.fills[0].liquidity_source == LiquiditySource::TradeCross);
  REQUIRE(strategy.fills[0].trigger_source_sequence == 4);
  REQUIRE(strategy.fills_before_trade == std::vector<std::size_t>({0, 0, 1}));
  REQUIRE(engine.position(1).net_quantity == 50);
  REQUIRE(engine.position(2).net_quantity == 0);
  REQUIRE(recorder.fills[0].liquidity_source == LiquiditySource::TradeCross);
  REQUIRE(recorder.fills[0].trigger_source_sequence == 4);
}

TEST_CASE(
    "Post-arrival same-instrument non-crossing quote and trade do not fill",
    "[Trading]") {
  HistoricalLOBStore books;
  SubmitOnFirstMarket strategy;
  strategy.price = 100;
  strategy.quantity = 9;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 5, 1}, books, strategy,
                       recorder);

  const auto initial = empty_book_view(100, 1);
  const auto later = empty_book_view(120, 3);
  const std::array quote{quote_signal(110, 2, std::nullopt, 101)};
  const std::array trade{trade_signal(120, 3, 101)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, initial, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 110, 110, 2, {}, {}, quote}},
      ScheduledEvent{MarketDelivery{1, 120, 120, 3, later, {}, trade}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.empty());
  REQUIRE(recorder.fills.empty());
  const auto open = engine.open_orders(1);
  REQUIRE(open.size() == 1);
  REQUIRE(open[0].state == OrderState::Open);
  REQUIRE(open[0].remaining_quantity == 9);
}

TEST_CASE("Own price priority precedes arrival FIFO across different prices",
          "[Trading]") {
  HistoricalLOBStore books;
  struct TwoPrices final : RecordingStrategy {
    std::array<ClOrdId, 2> ids{};
    bool submitted{};

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      if (!submitted) {
        submitted = true;
        ids[0] = context.submit_limit(1, Side::Buy, 99, 1);
        ids[1] = context.submit_limit(1, Side::Buy, 100, 1);
      }
    }
  } strategy;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 5, 1}, books, strategy,
                       recorder);

  const auto initial = empty_book_view(100, 1);
  const std::array cross{quote_signal(110, 2, std::nullopt, 99)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, initial, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 110, 110, 2, {}, {}, cross}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 2);
  REQUIRE(strategy.fills[0].client_order_id == strategy.ids[1]);
  REQUIRE(strategy.fills[1].client_order_id == strategy.ids[0]);
  REQUIRE(strategy.fills[0].trigger_source_sequence == 2);
  REQUIRE(strategy.fills[1].trigger_source_sequence == 2);
}

TEST_CASE("Infinite quote liquidity fills every own order and isolated view",
          "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 90, MarketAction::Add, Side::Sell, 101, 3));

  struct TwoOrders final : RecordingStrategy {
    std::vector<ClOrdId> ids;
    bool submitted{};
    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      if (!submitted) {
        submitted = true;
        ids.push_back(context.submit_limit(1, Side::Buy, 101, 2));
        ids.push_back(context.submit_limit(1, Side::Buy, 101, 2));
      }
    }
  } first, second;
  RecordingRecorder first_recorder;
  RecordingRecorder second_recorder;
  TradingEngine first_engine(instruments, BacktestConfig{0, 1, 1}, books, first,
                             first_recorder);
  TradingEngine second_engine(instruments, BacktestConfig{0, 1, 1}, books,
                              second, second_recorder);
  const auto view = empty_book_view(100, 2);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 2, view, {}, {}}}};
  SchedulerRuntime first_runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  SchedulerRuntime second_runtime(
      SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  first_runtime.run(events, first_engine);
  second_runtime.run(events, second_engine);

  REQUIRE(first.fills.size() == 2);
  REQUIRE(first.fills[0].client_order_id == first.ids[0]);
  REQUIRE(first.fills[0].quantity == 2);
  REQUIRE(first.fills[1].client_order_id == first.ids[1]);
  REQUIRE(first.fills[1].quantity == 2);
  REQUIRE(second.fills.size() == 2);
  REQUIRE(second_engine.position(1).net_quantity == 4);
}

TEST_CASE("Cancel is delayed and equal-time market fill wins", "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 70, MarketAction::Add, Side::Sell, 101, 2));
  struct CancelStrategy final : RecordingStrategy {
    ClOrdId id{};
    ClOrdId replacement_id{};
    int callbacks{};
    std::size_t fills_seen_in_reject{};

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      ++callbacks;
      if (callbacks == 1) {
        id = context.submit_limit(1, Side::Buy, 100, 2);
      } else if (callbacks == 2) {
        REQUIRE(context.cancel_order(id));
      }
    }

    void on_reject(const RejectView &reject,
                   StrategyContext &context) override {
      RecordingStrategy::on_reject(reject, context);
      fills_seen_in_reject = fills.size();
      replacement_id = context.submit_limit(1, Side::Buy, 100, 1);
    }
  } strategy;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 10, 1}, books, strategy,
                       recorder);
  const auto view100 = empty_book_view(100, 10);
  const auto view111 = empty_book_view(111, 11);
  const auto view121 = empty_book_view(121, 12);
  const auto view140 = empty_book_view(140, 13);
  const std::array cross121{quote_signal(121, 12, std::nullopt, 100)};
  const std::array cross140{quote_signal(140, 13, std::nullopt, 100)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 10, view100, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 111, 111, 11, view111, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 121, 121, 12, view121, {}, cross121}},
      ScheduledEvent{MarketDelivery{1, 140, 140, 13, view140, {}, cross140}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 2);
  REQUIRE(strategy.fills[0].engine_ts_ns == 121);
  REQUIRE(strategy.fills[0].remaining_quantity == 0);
  REQUIRE(strategy.fills_seen_in_reject == 1);
  REQUIRE(strategy.replacement_id == 2);
  REQUIRE(strategy.fills[1].client_order_id == strategy.replacement_id);
  REQUIRE(strategy.fills[1].engine_ts_ns == 140);
  REQUIRE(strategy.rejects.size() == 1);
  REQUIRE(strategy.rejects[0].reason == RejectReason::AlreadyTerminal);
}

TEST_CASE("Cancel arriving before a later price cross prevents fill",
          "[Trading]") {
  HistoricalLOBStore books;
  struct CancelBeforeCross final : RecordingStrategy {
    ClOrdId id{};
    int callbacks{};

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      ++callbacks;
      if (callbacks == 1) {
        id = context.submit_limit(1, Side::Buy, 100, 3);
      } else if (callbacks == 2) {
        REQUIRE(context.cancel_order(id));
      }
    }
  } strategy;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 5, 1}, books, strategy,
                       recorder);

  const auto view100 = empty_book_view(100, 1);
  const auto view110 = empty_book_view(110, 2);
  const std::array later_cross{quote_signal(120, 3, std::nullopt, 100)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, view100, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 110, 110, 2, view110, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 120, 120, 3, {}, {}, later_cross}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.empty());
  REQUIRE(recorder.fills.empty());
  REQUIRE(engine.open_orders(1).empty());
  REQUIRE(recorder.orders.back().event_type == OrderLogEventType::Cancelled);
  REQUIRE(recorder.orders.back().state == OrderState::Cancelled);
}

TEST_CASE("Invalid orders and unknown cancels reject deterministically",
          "[Trading]") {
  HistoricalLOBStore books;
  struct InvalidStrategy final : RecordingStrategy {
    bool done{};
    int callback_depth{};
    int maximum_callback_depth{};
    std::vector<std::string> callback_order;

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      if (!done) {
        done = true;
        ++callback_depth;
        maximum_callback_depth =
            std::max(maximum_callback_depth, callback_depth);
        callback_order.push_back("book");
        const ClOrdId terminal = context.submit_limit(2, Side::Buy, 100, 1);
        (void)context.submit_limit(1, Side::None, 100, 1);
        (void)context.submit_limit(1, static_cast<Side>(2), 100, 1);
        (void)context.submit_limit(1, Side::Buy, 101, 1);
        (void)context.submit_limit(1, Side::Buy, 100, 0);
        REQUIRE_FALSE(context.cancel_order(999));
        REQUIRE_FALSE(context.cancel_order(terminal));
        callback_order.push_back("book_done");
        --callback_depth;
      }
    }

    void on_reject(const RejectView &reject,
                   StrategyContext &context) override {
      ++callback_depth;
      maximum_callback_depth = std::max(maximum_callback_depth, callback_depth);
      callback_order.push_back("reject");
      RecordingStrategy::on_reject(reject, context);
      --callback_depth;
    }
  } strategy;
  RecordingRecorder recorder;
  const std::array ticked{InstrumentMeta{1, 2, 100, 1}};
  TradingEngine engine(ticked, BacktestConfig{0, 1, 1}, books, strategy,
                       recorder);
  const auto view = empty_book_view(100, 1);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, view, {}, {}}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.rejects.size() == 7);
  REQUIRE(strategy.rejects[0].reason == RejectReason::UnknownInstrument);
  REQUIRE(strategy.rejects[1].reason == RejectReason::InvalidSide);
  REQUIRE(strategy.rejects[2].reason == RejectReason::InvalidSide);
  REQUIRE(strategy.rejects[3].reason == RejectReason::TickMisalignment);
  REQUIRE(strategy.rejects[4].reason == RejectReason::NonPositiveQuantity);
  REQUIRE(strategy.rejects[5].reason == RejectReason::UnknownOrder);
  REQUIRE(strategy.rejects[6].reason == RejectReason::AlreadyTerminal);
  REQUIRE(strategy.maximum_callback_depth == 1);
  REQUIRE(strategy.callback_order ==
          std::vector<std::string>({"book", "book_done", "reject", "reject",
                                    "reject", "reject", "reject", "reject",
                                    "reject"}));
}

TEST_CASE("Sell fully fills at best bid without volume cap", "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 81, MarketAction::Add, Side::Buy, 100, 2));
  books.apply(market_event(2, 2, 82, MarketAction::Add, Side::Buy, 99, 4));
  books.apply(market_event(3, 3, 83, MarketAction::Add, Side::Buy, 98, 20));
  SubmitOnFirstMarket strategy;
  strategy.side = Side::Sell;
  strategy.price = 99;
  strategy.quantity = 8;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 1, 1}, books, strategy,
                       recorder);
  const auto view = empty_book_view(100, 4);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 4, view, {}, {}}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].price == 100);
  REQUIRE(strategy.fills[0].quantity == 8);
  REQUIRE(strategy.fills[0].liquidity_source == LiquiditySource::QuoteCross);
  REQUIRE(engine.open_orders(1).empty());
  REQUIRE(engine.position(1).net_quantity == -8);
}

TEST_CASE("Commands are globally monotonic and normal cancel is terminal",
          "[Trading]") {
  HistoricalLOBStore books;
  struct SequencingStrategy final : RecordingStrategy {
    std::array<ClOrdId, 2> ids{};
    int callbacks{};

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      ++callbacks;
      if (callbacks == 1) {
        ids[0] = context.submit_limit(1, Side::Buy, 90, 1);
        ids[1] = context.submit_limit(2, Side::Sell, 110, 1);
      } else if (callbacks == 2) {
        REQUIRE(context.cancel_order(ids[0]));
      }
    }
  } strategy;
  RecordingRecorder recorder;
  const std::array two_instruments{InstrumentMeta{1, 1, 100, 10},
                                   InstrumentMeta{2, 1, 100, 10}};
  TradingEngine engine(two_instruments, BacktestConfig{0, 5, 1}, books,
                       strategy, recorder);
  const auto view100 = empty_book_view(100, 1);
  const auto view110 = empty_book_view(110, 2);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, view100, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 110, 110, 2, view110, {}, {}}}};
  std::vector<Sequence> command_sequences;
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, [&](const ScheduledEvent &event, CommandSink &commands) {
    if (event.priority() != EventPriority::MarketData) {
      command_sequences.push_back(event.key().source_or_command_sequence);
    }
    engine(event, commands);
  });

  REQUIRE(command_sequences == std::vector<Sequence>({1, 2, 3}));
  REQUIRE(engine.open_orders(1).empty());
  const auto open_second = engine.open_orders(2);
  REQUIRE(open_second.size() == 1);
  REQUIRE(open_second[0].client_order_id == strategy.ids[1]);
  REQUIRE(recorder.orders.back().event_type == OrderLogEventType::Cancelled);
  REQUIRE(recorder.orders.back().state == OrderState::Cancelled);
}

TEST_CASE("PositionKeeper uses exact multiplier-scaled FIFO realized inputs",
          "[Trading]") {
  PositionKeeper positions;
  positions.register_instrument(InstrumentMeta{1, 1, 100, 10});
  positions.apply_fill(1, Side::Buy, 100, 2);
  positions.apply_fill(1, Side::Sell, 110, 1);
  auto snapshot = positions.position(1);
  REQUIRE(snapshot.net_quantity == 1);
  REQUIRE(snapshot.average_open_price_ticks == 100.0);
  REQUIRE(snapshot.realized_pnl == 1.0);
  positions.apply_fill(1, Side::Sell, 90, 2);
  snapshot = positions.position(1);
  REQUIRE(snapshot.net_quantity == -1);
  REQUIRE(snapshot.average_open_price_ticks == 90.0);
  REQUIRE(snapshot.realized_pnl == 0.0);
}

TEST_CASE("PositionKeeper overflow is defined and leaves state unchanged",
          "[Trading]") {
  PositionKeeper positions;
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  positions.register_instrument(InstrumentMeta{1, 1, 1, maximum});
  positions.apply_fill(1, Side::Buy, 1, maximum);
  const PositionSnapshot before = positions.position(1);

  bool overflowed = false;
  try {
    positions.apply_fill(1, Side::Sell, maximum, maximum);
  } catch (const PositionError &) {
    overflowed = true;
  }
  REQUIRE(overflowed);
  const PositionSnapshot after = positions.position(1);
  REQUIRE(after.net_quantity == before.net_quantity);
  REQUIRE(after.average_open_price_ticks == before.average_open_price_ticks);
  REQUIRE(after.realized_pnl == before.realized_pnl);

  bool invalid_side = false;
  try {
    positions.apply_fill(1, static_cast<Side>(2), 1, 1);
  } catch (const std::invalid_argument &) {
    invalid_side = true;
  }
  REQUIRE(invalid_side);
  REQUIRE(positions.position(1).net_quantity == before.net_quantity);
}

TEST_CASE("Twenty scripted trading runs are deterministic", "[Trading]") {
  std::vector<PriceTicks> baseline_prices;
  std::vector<Quantity> baseline_quantities;
  for (int iteration = 0; iteration < 20; ++iteration) {
    HistoricalLOBStore books;
    books.apply(market_event(1, 1, 101, MarketAction::Add, Side::Sell, 101, 2));
    books.apply(market_event(2, 2, 102, MarketAction::Add, Side::Sell, 102, 3));
    SubmitOnFirstMarket strategy;
    strategy.quantity = 5;
    RecordingRecorder recorder;
    TradingEngine engine(instruments, BacktestConfig{0, 7, 1}, books, strategy,
                         recorder);
    const auto view = empty_book_view(100, 3);
    const std::array events{
        ScheduledEvent{MarketDelivery{1, 100, 100, 3, view, {}, {}}}};
    SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
    runtime.run(events, engine);

    std::vector<PriceTicks> prices;
    std::vector<Quantity> quantities;
    for (const auto &fill : strategy.fills) {
      prices.push_back(fill.price);
      quantities.push_back(fill.quantity);
    }
    if (iteration == 0) {
      baseline_prices = prices;
      baseline_quantities = quantities;
    } else {
      REQUIRE(prices == baseline_prices);
      REQUIRE(quantities == baseline_quantities);
    }
  }
}

TEST_CASE("Execution costs apply bounded adverse slippage and maker rebates",
          "[Trading][ExecutionCosts]") {
  const std::array metadata{InstrumentMeta{1, 5, 100, 1}};
  BacktestConfig config;
  config.slippage_model = SlippageModel::FixedTicks;
  config.taker_slippage_tick_count = 3;
  config.maker_fee_micros_per_contract = -100;
  config.taker_fee_micros_per_contract = 250;
  const ExecutionCostModel costs(metadata, config);

  const auto buy =
      costs.apply(1, Side::Buy, 110, 100, 2, LiquiditySource::QuoteCross);
  REQUIRE(buy.reference_price_ticks == 100);
  REQUIRE(buy.fill_price_ticks == 110);
  REQUIRE(buy.slippage_ticks == 2);
  REQUIRE(buy.liquidity_role == LiquidityRole::Taker);
  REQUIRE(buy.fee_micros == 500);

  const auto maker =
      costs.apply(1, Side::Sell, 95, 100, 2, LiquiditySource::TradeCross);
  REQUIRE(maker.fill_price_ticks == 100);
  REQUIRE(maker.slippage_ticks == 0);
  REQUIRE(maker.liquidity_role == LiquidityRole::Maker);
  REQUIRE(maker.fee_micros == -200);
}

TEST_CASE("Pre-trade risk reserves pending exposure and releases it exactly",
          "[Trading][Risk]") {
  BacktestConfig config;
  config.max_order_quantity = 5;
  config.max_abs_position = 7;
  config.max_open_quantity = 6;
  config.max_active_orders = 2;
  PreTradeRiskEngine risk(instruments, config);

  risk.reserve(1, 1, Side::Buy, 4);
  auto snapshot = risk.snapshot(1);
  REQUIRE(snapshot.reserved_buy_quantity == 4);
  REQUIRE(snapshot.active_orders == 1);
  REQUIRE(risk.check(1, Side::Buy, 3) ==
          RejectReason::RiskOpenQuantityExceeded);
  risk.reserve(2, 1, Side::Buy, 2);
  REQUIRE(risk.check(1, Side::Sell, 1) ==
          RejectReason::RiskActiveOrderLimitExceeded);

  risk.apply_fill(1, 2);
  risk.release(2);
  snapshot = risk.snapshot(1);
  REQUIRE(snapshot.net_position == 2);
  REQUIRE(snapshot.reserved_buy_quantity == 2);
  REQUIRE(snapshot.active_orders == 1);
  risk.apply_fill(1, 2);
  REQUIRE(risk.check(1, Side::Buy, 4) ==
          RejectReason::RiskPositionLimitExceeded);
}

TEST_CASE("Trading engine reports risk rejects without scheduling them",
          "[Trading][Risk]") {
  struct RiskStrategy final : RecordingStrategy {
    std::vector<RiskSnapshot> snapshots;
    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      (void)context.submit_limit(1, Side::Buy, 100, 4);
      snapshots.push_back(context.risk(1));
      (void)context.submit_limit(1, Side::Buy, 100, 2);
      snapshots.push_back(context.risk(1));
    }
  } strategy;
  HistoricalLOBStore books;
  RecordingRecorder recorder;
  BacktestConfig config{0, 1, 1};
  config.max_abs_position = 5;
  TradingEngine engine(instruments, config, books, strategy, recorder);
  const auto view = empty_book_view(100, 1);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, view, {}, {}}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.snapshots.size() == 2);
  REQUIRE(strategy.snapshots[0].reserved_buy_quantity == 4);
  REQUIRE(strategy.snapshots[1].reserved_buy_quantity == 4);
  REQUIRE(strategy.rejects.size() == 1);
  REQUIRE(strategy.rejects[0].reason ==
          RejectReason::RiskPositionLimitExceeded);
  REQUIRE(recorder.orders.size() == 4);
  REQUIRE(recorder.orders[2].event_type == OrderLogEventType::Reject);
}
