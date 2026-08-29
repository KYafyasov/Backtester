#pragma once

#include "core/Events.hpp"
#include "core/ResultSchemas.hpp"

#include <span>

namespace cmf::scheduler {
class CommandSink;
}

namespace cmf::trading {

class StrategyContext {
public:
  virtual ~StrategyContext() = default;

  [[nodiscard]] virtual ClOrdId submit_limit(InstrumentId instrument_id,
                                             Side side,
                                             PriceTicks limit_price_ticks,
                                             Quantity quantity) = 0;
  virtual bool cancel_order(ClOrdId client_order_id) = 0;
  [[nodiscard]] virtual TimestampNs now_ns() const noexcept = 0;
  [[nodiscard]] virtual PositionSnapshot
  position(InstrumentId instrument_id) const = 0;
  [[nodiscard]] virtual std::span<const OrderQueryRow>
  open_orders(InstrumentId instrument_id) = 0;
  [[nodiscard]] virtual RiskSnapshot risk(InstrumentId instrument_id) const = 0;
};

class Strategy {
public:
  virtual ~Strategy() = default;

  virtual void on_book_update(const BookUpdateView &, StrategyContext &) {}
  virtual void on_trade(const TradeView &, StrategyContext &) {}
  virtual void on_fill(const FillView &, StrategyContext &) {}
  virtual void on_reject(const RejectView &, StrategyContext &) {}
};

class Recorder {
public:
  virtual ~Recorder() = default;

  virtual void on_order_event(const OrderLogResultRow &) {}
  virtual void on_fill(const FillResultRow &) {}
  virtual void on_reject(const RejectView &) {}
};

} // namespace cmf::trading
