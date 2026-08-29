#pragma once
#include "BookTypes.hpp"
#include "MarketDataEvent.hpp"
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <unordered_map>
#include <vector>

// Limit order book (LOB) for one instrument.
// Supports Add/Cancel/Modify/Clear operations in O(log N).
// Trade and Fill records are handled for book maintenance and statistics;
// passive liquidity changes arrive as separate exchange records.
class LimitOrderBook {
public:
  // Apply one MBO event by dispatching event.action to its private handler.
  void applyEvent(const MarketDataEvent &event);

  // Return the best bid price, or std::nullopt when there are no bids.
  std::optional<double> bestBid() const;
  // Return the best ask price, or std::nullopt when there are no asks.
  std::optional<double> bestAsk() const;

  // Return aggregate quantity at the best bid or ask.
  long long bestBidSize() const;
  long long bestAskSize() const;

  // Return an aggregated snapshot. depth=0 includes all visible levels.
  BookSnapshot snapshot(std::size_t depth = 0) const;

  // Print the top N levels on each side.
  void printSnapshot(std::ostream &os, int depth = 5) const;

  // Event counters used for statistics.
  long long totalAdds = 0;
  long long totalCancels = 0;
  long long totalTrades = 0;
  long long totalClears = 0;

private:
  // action='A': insert an order. An existing order_id is overwritten.
  void onAdd(const MarketDataEvent &e);

  // action='C': remove by order_id using the O(1) price-level index.
  void onCancel(const MarketDataEvent &e);

  // action='M': update price/quantity as cancel followed by add.
  void onModify(const MarketDataEvent &e);

  // action='R': clear the complete book before snapshot reconstruction.
  void onClear();

  // action='T': aggressor-side trade; does not mutate the resting book.
  void onTrade(const MarketDataEvent &e);

  // action='F': execution of a passive order.
  void onFill(const MarketDataEvent &e);

  // Bids: price -> (order_id -> size); begin() is the best bid.
  std::map<double, std::map<std::string, long long>, std::greater<double>>
      bids_;
  // Asks: price -> (order_id -> size); begin() is the best (lowest) ask.
  std::map<double, std::map<std::string, long long>> asks_;
  // Index: order_id -> {side, price} for O(1) Cancel/Fill lookup.
  std::unordered_map<std::string, std::pair<char, double>> orderIndex_;
};
