#pragma once

#include <cstdint>

namespace cmf {

using TimestampNs = std::int64_t;
using PriceTicks = std::int64_t;
using Quantity = std::int64_t;
using InstrumentId = std::int64_t;
using ClOrdId = std::uint64_t;
using ExchangeOrderId = std::uint64_t;
using Sequence = std::uint64_t;
using SourceId = std::uint32_t;
using SourcePriority = std::uint32_t;

enum class TimestampSemantics : std::uint8_t {
  Exchange = 1,
  Receive = 2,
  LocalReceive = 3,
  Unknown = 4,
};

[[nodiscard]] inline constexpr const char *
timestamp_semantics_name(TimestampSemantics semantics) noexcept {
  switch (semantics) {
  case TimestampSemantics::Exchange:
    return "exchange";
  case TimestampSemantics::Receive:
    return "receive";
  case TimestampSemantics::LocalReceive:
    return "local_receive";
  case TimestampSemantics::Unknown:
    return "unknown";
  }
  return "unknown";
}

struct AccountCurrencyAmount {
  std::int64_t numerator{};
  std::int64_t denominator{1};
};

enum class Side : std::int8_t {
  Sell = -1,
  None = 0,
  Buy = 1,
};

enum class OrderState : std::uint8_t {
  PendingNew = 0,
  Open = 1,
  PartiallyFilled = 2,
  Filled = 3,
  PendingCancel = 4,
  Cancelled = 5,
  Rejected = 6,
};

enum class RejectReason : std::uint8_t {
  None = 0,
  UnknownInstrument = 1,
  InvalidSide = 2,
  NonPositiveQuantity = 3,
  InvalidPrice = 4,
  TickMisalignment = 5,
  DuplicateClientOrderId = 6,
  UnsupportedOrderType = 7,
  UnsupportedTimeInForce = 8,
  UnknownOrder = 9,
  AlreadyTerminal = 10,
};

enum class EventPriority : std::uint8_t {
  MarketData = 0,
  NewOrder = 1,
  Cancel = 2,
};

enum class CommandType : std::uint8_t {
  NewOrder = 0,
  Cancel = 1,
};

enum class OrderLogEventType : std::uint8_t {
  Submit = 0,
  Accepted = 1,
  Fill = 2,
  CancelRequest = 3,
  Cancelled = 4,
  Reject = 5,
};

enum class LiquiditySource : std::uint8_t {
  HistoricalDisplayed = 0,
  QuoteCross = 1,
  TradeCross = 2,
};

enum class PriceCrossSource : std::uint8_t {
  BestQuote = 0,
  Trade = 1,
};

} // namespace cmf
