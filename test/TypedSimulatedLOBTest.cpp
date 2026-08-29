#include "market/HistoricalLOBStore.hpp"
#include "trading/SimulatedLOB.hpp"

#include "MiniTest.hpp"

#include <array>

namespace {

using namespace cmf;

market::MarketDataEvent add(Sequence sequence, ExchangeOrderId order_id,
                            Side side, PriceTicks price, Quantity quantity) {
  return market::MarketDataEvent{
      100,  100,   1,        order_id, sequence, market::MarketAction::Add,
      side, price, quantity, 128};
}

constexpr std::array<InstrumentMeta, 1> instruments{
    InstrumentMeta{1, 1, 1'000'000'000, 1}};

} // namespace

TEST_CASE("Typed SimulatedLOB owns the M5 golden matching decision",
          "[SimulatedLOB]") {
  market::HistoricalLOBStore books;
  auto &book = books.apply(add(1, 11, Side::Sell, 101'000'000'000, 2));
  trading::SimulatedLOB simulated(instruments);

  const auto fills =
      simulated.accept(1, 1, Side::Buy, 101'000'000'000, 20, 1, &book);

  REQUIRE(fills.size() == 1);
  REQUIRE(fills[0].client_order_id == 1);
  REQUIRE(fills[0].price == 101'000'000'000);
  REQUIRE(fills[0].quantity == 20);
  REQUIRE(fills[0].liquidity_source == LiquiditySource::QuoteCross);
  REQUIRE(fills[0].trigger_source_sequence == 1);
  REQUIRE(book.best_ask()->quantity == 2);
}

TEST_CASE("Typed EngineViews independently use infinite quote liquidity",
          "[SimulatedLOB]") {
  market::HistoricalLOBStore books;
  auto &book = books.apply(add(1, 11, Side::Sell, 101, 3));
  trading::SimulatedLOB first(instruments);
  trading::SimulatedLOB second(instruments);

  const auto first_fills = first.accept(1, 1, Side::Buy, 101, 20, 1, &book);
  const auto second_fills = second.accept(1, 1, Side::Buy, 101, 30, 1, &book);

  REQUIRE(first_fills.size() == 1);
  REQUIRE(first_fills[0].quantity == 20);
  REQUIRE(second_fills.size() == 1);
  REQUIRE(second_fills[0].quantity == 30);
  REQUIRE(book.best_ask()->quantity == 3);
}

TEST_CASE("One price-only trade fills all eligible buys and sells",
          "[SimulatedLOB]") {
  trading::SimulatedLOB simulated(instruments);
  REQUIRE(simulated.accept(1, 1, Side::Buy, 100, 40, 1, nullptr).empty());
  REQUIRE(simulated.accept(2, 1, Side::Sell, 100, 50, 2, nullptr).empty());

  const PriceCrossSignal trade{
      1, 200, 205, 7, PriceCrossSource::Trade, std::nullopt, std::nullopt, 100};
  const auto fills = simulated.on_signal(trade);

  REQUIRE(fills.size() == 2);
  REQUIRE(fills[0].client_order_id == 1);
  REQUIRE(fills[0].quantity == 40);
  REQUIRE(fills[0].price == 100);
  REQUIRE(fills[0].liquidity_source == LiquiditySource::TradeCross);
  REQUIRE(fills[0].trigger_source_sequence == 7);
  REQUIRE(fills[1].client_order_id == 2);
  REQUIRE(fills[1].quantity == 50);
  REQUIRE(fills[1].price == 100);
  REQUIRE(fills[1].liquidity_source == LiquiditySource::TradeCross);
  REQUIRE(fills[1].trigger_source_sequence == 7);
}

TEST_CASE(
    "Queue-aware matching waits for displayed FIFO and emits partial fills",
    "[SimulatedLOB][QueueAware]") {
  market::HistoricalLOBStore books;
  auto &book = books.apply(add(1, 11, Side::Buy, 100, 5));
  trading::SimulatedLOB simulated(instruments, FillModel::QueueAware);

  REQUIRE(simulated.accept(1, 1, Side::Buy, 100, 4, 1, &book).empty());
  REQUIRE(simulated.queue_ahead(1) == 5);

  const PriceCrossSignal first_trade{1,
                                     200,
                                     205,
                                     2,
                                     PriceCrossSource::Trade,
                                     std::nullopt,
                                     std::nullopt,
                                     100,
                                     0,
                                     0,
                                     Side::Sell,
                                     3};
  REQUIRE(simulated.on_signal(first_trade).empty());
  REQUIRE(simulated.queue_ahead(1) == 2);

  const PriceCrossSignal second_trade{1,
                                      210,
                                      215,
                                      3,
                                      PriceCrossSource::Trade,
                                      std::nullopt,
                                      std::nullopt,
                                      100,
                                      0,
                                      0,
                                      Side::Sell,
                                      4};
  const auto partial = simulated.on_signal(second_trade);
  REQUIRE(partial.size() == 1);
  REQUIRE(partial[0].client_order_id == 1);
  REQUIRE(partial[0].quantity == 2);
  REQUIRE(simulated.queue_ahead(1) == 0);

  const PriceCrossSignal final_trade{1,
                                     220,
                                     225,
                                     4,
                                     PriceCrossSource::Trade,
                                     std::nullopt,
                                     std::nullopt,
                                     100,
                                     0,
                                     0,
                                     Side::Sell,
                                     2};
  const auto completed = simulated.on_signal(final_trade);
  REQUIRE(completed.size() == 1);
  REQUIRE(completed[0].quantity == 2);
  REQUIRE_FALSE(simulated.queue_ahead(1).has_value());
}

TEST_CASE("Queue-aware own orders retain FIFO and cancellation releases space",
          "[SimulatedLOB][QueueAware]") {
  market::HistoricalLOBStore books;
  auto &book = books.apply(add(1, 11, Side::Buy, 100, 5));
  trading::SimulatedLOB simulated(instruments, FillModel::QueueAware);

  REQUIRE(simulated.accept(1, 1, Side::Buy, 100, 2, 1, &book).empty());
  books.apply(add(2, 12, Side::Buy, 100, 5));
  REQUIRE(simulated.accept(2, 1, Side::Buy, 100, 3, 2, &book).empty());
  REQUIRE(simulated.queue_ahead(1) == 5);
  REQUIRE(simulated.queue_ahead(2) == 12);

  simulated.cancel(1);
  REQUIRE(simulated.queue_ahead(2) == 10);

  const PriceCrossSignal trade{1,
                               200,
                               205,
                               2,
                               PriceCrossSource::Trade,
                               std::nullopt,
                               std::nullopt,
                               100,
                               0,
                               0,
                               Side::Sell,
                               11};
  const auto fills = simulated.on_signal(trade);
  REQUIRE(fills.size() == 1);
  REQUIRE(fills[0].client_order_id == 2);
  REQUIRE(fills[0].quantity == 1);
  REQUIRE(simulated.queue_ahead(2) == 0);
}

TEST_CASE("Queue-aware matching is conservative without aggressor semantics",
          "[SimulatedLOB][QueueAware]") {
  trading::SimulatedLOB simulated(instruments, FillModel::QueueAware);
  REQUIRE(simulated.accept(1, 1, Side::Buy, 100, 2, 1, nullptr).empty());

  const PriceCrossSignal unknown_side_trade{1,
                                            200,
                                            205,
                                            2,
                                            PriceCrossSource::Trade,
                                            std::nullopt,
                                            std::nullopt,
                                            100,
                                            0,
                                            0,
                                            Side::None,
                                            10};
  REQUIRE(simulated.on_signal(unknown_side_trade).empty());
  REQUIRE(simulated.queue_ahead(1) == 0);
}

TEST_CASE("Queue-aware arrival reads displayed quantity from an L2 snapshot",
          "[SimulatedLOB][QueueAware][L2]") {
  market::HistoricalLOBStore books;
  const std::array bids{BookLevel{100, 6}};
  const std::array asks{BookLevel{101, 4}};
  books.replace_snapshot(1, bids, asks, 1);
  trading::SimulatedLOB simulated(instruments, FillModel::QueueAware);

  REQUIRE(
      simulated.accept_from_store(1, 1, Side::Buy, 100, 2, 1, &books).empty());
  REQUIRE(simulated.queue_ahead(1) == 6);
}
