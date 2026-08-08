#pragma once

#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"
#include "market/HistoricalLOBStore.hpp"
#include "market/LimitOrderBook.hpp"

#include <map>
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
    Sequence arrival_sequence{};
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
  explicit SimulatedLOB(std::span<const InstrumentMeta> instruments);

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

  [[nodiscard]] const EngineView &engine_view() const noexcept { return view_; }

private:
  void match_prices(InstrumentId instrument_id,
                    std::optional<PriceTicks> buy_trigger,
                    std::optional<PriceTicks> sell_trigger,
                    LiquiditySource liquidity_source,
                    Sequence trigger_source_sequence,
                    SourceId trigger_source_id = 0,
                    Sequence trigger_global_market_sequence = 0);
  void insert_resting(const EngineView::PrivateOrder &order);
  void erase_resting(const EngineView::PrivateOrder &order);
  [[nodiscard]] std::span<const SyntheticFill> accept_with_touch(
      ClOrdId client_order_id, InstrumentId instrument_id, Side side,
      PriceTicks limit_price, Quantity remaining_quantity,
      Sequence arrival_sequence, std::optional<PriceTicks> best_bid,
      std::optional<PriceTicks> best_ask, Sequence source_sequence);

  EngineView view_;
  std::vector<SyntheticFill> fills_;
};

} // namespace cmf::trading
