#pragma once

#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"
#include "market/HistoricalLOBStore.hpp"
#include "market/LimitOrderBook.hpp"
#include "trading/ExecutionCostModel.hpp"

#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace cmf::trading {

class SimulatedLOBError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct SyntheticFill {
  ClOrdId client_order_id{};
  PriceTicks price{};
  Quantity quantity{};
  LiquiditySource liquidity_source{LiquiditySource::HistoricalDisplayed};
  Sequence trigger_source_sequence{};
  SourceId trigger_source_id{};
  Sequence trigger_global_market_sequence{};
  PriceTicks reference_price_ticks{};
  LiquidityRole liquidity_role{LiquidityRole::Maker};
  std::uint32_t slippage_ticks{};
  std::int64_t fee_micros{};
};

// The typed private overlay. It owns only this engine's resting orders; the
// shared HistoricalLOB remains immutable here.
class EngineView {
public:
  explicit EngineView(std::span<const InstrumentMeta> instruments);

private:
  friend class SimulatedLOB;

  struct RestingKey {
    PriceTicks price{};
    Sequence arrival_sequence{};
    ClOrdId client_order_id{};
  };
  struct BuyFirst {
    bool operator()(const RestingKey &left,
                    const RestingKey &right) const noexcept;
  };
  struct SellFirst {
    bool operator()(const RestingKey &left,
                    const RestingKey &right) const noexcept;
  };
  struct PrivateOrder {
    InstrumentId instrument_id{};
    ClOrdId client_order_id{};
    Side side{Side::None};
    PriceTicks limit_price{};
    Quantity remaining_quantity{};
    Quantity initial_quantity{};
    Sequence arrival_sequence{};
    Quantity queue_threshold{};
  };
  struct InstrumentOrders {
    std::map<RestingKey, ClOrdId, BuyFirst> buys;
    std::map<RestingKey, ClOrdId, SellFirst> sells;
  };

  std::unordered_map<ClOrdId, PrivateOrder> orders_;
  std::unordered_map<InstrumentId, InstrumentOrders> resting_;
};

// The sole synthetic-fill authority. TradingEngine supplies accepted/cancelled
// lifecycle events and applies the returned decisions to state and callbacks.
class SimulatedLOB {
public:
  explicit SimulatedLOB(std::span<const InstrumentMeta> instruments,
                        FillModel fill_model = FillModel::FillAtTouch);
  SimulatedLOB(std::span<const InstrumentMeta> instruments,
               const BacktestConfig &config);

  // Returned spans alias this SimulatedLOB's internal fill buffer. Their
  // elements remain valid only until the next accept() or on_signal() call on
  // this instance, or until this instance is moved from or destroyed,
  // whichever comes first. cancel() does not invalidate them. Callers must
  // consume the elements synchronously and must not retain the span.
  [[nodiscard]] std::span<const SyntheticFill>
  accept(ClOrdId client_order_id, InstrumentId instrument_id, Side side,
         PriceTicks limit_price, Quantity remaining_quantity,
         Sequence arrival_sequence, const market::LimitOrderBook *book);

  [[nodiscard]] std::span<const SyntheticFill>
  accept_from_store(ClOrdId client_order_id, InstrumentId instrument_id,
                    Side side, PriceTicks limit_price,
                    Quantity remaining_quantity, Sequence arrival_sequence,
                    const market::HistoricalLOBStore *books);

  [[nodiscard]] std::span<const SyntheticFill>
  on_signal(const PriceCrossSignal &signal);

  void cancel(ClOrdId client_order_id);

  [[nodiscard]] std::optional<Quantity>
  queue_ahead(ClOrdId client_order_id) const;

  [[nodiscard]] const EngineView &engine_view() const noexcept { return view_; }

private:
  void match_prices(InstrumentId instrument_id,
                    std::optional<PriceTicks> buy_trigger,
                    std::optional<PriceTicks> sell_trigger,
                    LiquiditySource liquidity_source,
                    Sequence trigger_source_sequence,
                    SourceId trigger_source_id = 0,
                    Sequence trigger_global_market_sequence = 0);
  void match_queue_trade(const PriceCrossSignal &signal);
  void insert_resting(const EngineView::PrivateOrder &order);
  void erase_resting(const EngineView::PrivateOrder &order);
  void release_cancelled_queue(const EngineView::PrivateOrder &order);
  [[nodiscard]] std::span<const SyntheticFill> accept_with_touch(
      ClOrdId client_order_id, InstrumentId instrument_id, Side side,
      PriceTicks limit_price, Quantity remaining_quantity,
      Sequence arrival_sequence, std::optional<PriceTicks> best_bid,
      std::optional<PriceTicks> best_ask,
      std::optional<Quantity> same_side_quantity, Sequence source_sequence);

  struct QueueKey {
    InstrumentId instrument_id{};
    Side side{Side::None};
    PriceTicks price{};

    [[nodiscard]] bool operator<(const QueueKey &other) const noexcept {
      return std::tuple{instrument_id, side, price} <
             std::tuple{other.instrument_id, other.side, other.price};
    }
  };

  [[nodiscard]] Quantity executed_volume(const QueueKey &key) const noexcept;
  [[nodiscard]] Quantity
  queue_ahead_quantity(const EngineView::PrivateOrder &order) const noexcept;

  EngineView view_;
  FillModel fill_model_;
  ExecutionCostModel execution_costs_;
  std::map<QueueKey, Quantity> executed_volume_;
  std::vector<SyntheticFill> fills_;
};

} // namespace cmf::trading
