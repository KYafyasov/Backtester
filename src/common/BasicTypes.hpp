#pragma once

#include "core/Types.hpp"

#include <functional>

namespace cmf {

using NanoTime = TimestampNs;
using OrderId = ExchangeOrderId;
using StrategyId = std::uint16_t; // identifies a strategy
using MarketId = std::uint16_t;   // identifies a market/exchange
using SecurityId = InstrumentId;
using Price = PriceTicks;

enum class OrderType { None = 0, Limit, Market };

enum class TimeInForce { None = 0, GoodTillCancel, FillAndKill, FillOrKill };

enum class SecurityType { None = 0, FX, Stock, Bond, Future, Option };

// id for an object identifying a specific security traded on a specific market
struct MarketSecurityId {
  MarketId mktId;
  SecurityId secId;

  bool operator==(const MarketSecurityId &other) const = default;
};

// hash function for MarketSecurityId
struct MarketSecurityIdHash {
  std::size_t operator()(const MarketSecurityId &key) const noexcept {
    std::size_t h1 = std::hash<SecurityId>{}(key.secId);
    std::size_t h2 = std::hash<MarketId>{}(key.mktId);
    return h1 ^ (h2 << 1);
  }
};

// Market identifiers
class MktId {
public:
  // sentinel
  static constexpr MarketId None = 0;

  // TBD
};

// sentinel for SecurityId
struct SecId {
  static constexpr SecurityId None = 0;
};

// sentinel for MarketSecurityId
struct MktSecId {
  static constexpr MarketSecurityId None = {0, 0};
};

} // namespace cmf
