#pragma once

#include "core/Events.hpp"
#include "market/MarketDataEvent.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace cmf::market {

class BookError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct HistoricalBookLevel {
  PriceTicks price{};
  Quantity quantity{};
  Sequence revision{};
};

// Stable identity of one visible historical order. Private consumption must be
// keyed by (exchange_order_id, liquidity_revision), not by aggregate level
// revision. Quantity may shrink while that identity remains stable.
struct HistoricalOrderSlice {
  ExchangeOrderId exchange_order_id{};
  Side side{Side::None};
  PriceTicks price{};
  Quantity remaining_quantity{};
  Sequence liquidity_revision{};
  TimestampNs exchange_ts_ns{};
  Sequence source_sequence{};
};

class LimitOrderBook {
public:
  // Unknown Cancel is a no-op so replay may start inside an order's lifetime.
  // Unknown Modify and Fill are corruption and throw BookError.
  void apply(const MarketDataEvent &event);

  [[nodiscard]] std::optional<HistoricalBookLevel> best_bid() const;
  [[nodiscard]] std::optional<HistoricalBookLevel> best_ask() const;
  [[nodiscard]] std::vector<HistoricalBookLevel>
  top_bids(std::size_t depth) const;
  [[nodiscard]] std::vector<HistoricalBookLevel>
  top_asks(std::size_t depth) const;
  void write_top_bids(std::size_t depth, std::vector<BookLevel> &output) const;
  void write_top_asks(std::size_t depth, std::vector<BookLevel> &output) const;
  [[nodiscard]] std::optional<HistoricalBookLevel>
  level(Side side, PriceTicks price) const;
  [[nodiscard]] std::optional<HistoricalOrderSlice>
  order_slice(ExchangeOrderId exchange_order_id) const;

  // Visits marketable historical orders without allocating a snapshot.
  // Ordering is deterministic: best price, then exchange timestamp, source
  // sequence, and exchange order ID. Returning false stops iteration.
  template <typename Visitor>
  void for_each_marketable_liquidity(Side taker_side, PriceTicks limit_price,
                                     Visitor &&visitor) const {
    if (taker_side == Side::Buy) {
      for (const auto &[price, level_value] : asks_) {
        if (price > limit_price) {
          break;
        }
        for (const auto &[priority, order_id] : level_value.orders) {
          (void)priority;
          const auto order = orders_.find(order_id);
          if (order == orders_.end()) {
            throw BookError("historical ask index is inconsistent");
          }
          if (!visitor(make_slice(order_id, order->second))) {
            return;
          }
        }
      }
      return;
    }
    if (taker_side == Side::Sell) {
      for (const auto &[price, level_value] : bids_) {
        if (price < limit_price) {
          break;
        }
        for (const auto &[priority, order_id] : level_value.orders) {
          (void)priority;
          const auto order = orders_.find(order_id);
          if (order == orders_.end()) {
            throw BookError("historical bid index is inconsistent");
          }
          if (!visitor(make_slice(order_id, order->second))) {
            return;
          }
        }
      }
      return;
    }
    throw BookError("marketable-liquidity taker side must be Buy or Sell");
  }
  [[nodiscard]] std::size_t order_count() const noexcept {
    return orders_.size();
  }
  [[nodiscard]] Sequence book_revision() const noexcept {
    return book_revision_;
  }
  [[nodiscard]] Sequence last_book_source_sequence() const noexcept {
    return last_book_source_sequence_;
  }

  [[nodiscard]] std::uint64_t total_adds() const noexcept {
    return total_adds_;
  }
  [[nodiscard]] std::uint64_t total_cancels() const noexcept {
    return total_cancels_;
  }
  [[nodiscard]] std::uint64_t total_modifies() const noexcept {
    return total_modifies_;
  }
  [[nodiscard]] std::uint64_t total_fills() const noexcept {
    return total_fills_;
  }
  [[nodiscard]] std::uint64_t total_trades() const noexcept {
    return total_trades_;
  }
  [[nodiscard]] std::uint64_t total_clears() const noexcept {
    return total_clears_;
  }

private:
  struct PriorityKey {
    TimestampNs exchange_ts_ns{};
    Sequence source_sequence{};
    ExchangeOrderId exchange_order_id{};

    [[nodiscard]] bool operator<(const PriorityKey &other) const noexcept {
      return std::tie(exchange_ts_ns, source_sequence, exchange_order_id) <
             std::tie(other.exchange_ts_ns, other.source_sequence,
                      other.exchange_order_id);
    }
  };

  struct Order {
    Side side{Side::None};
    PriceTicks price{};
    Quantity quantity{};
    Sequence liquidity_revision{};
    PriorityKey priority;
  };

  struct Level {
    Quantity quantity{};
    Sequence revision{};
    std::map<PriorityKey, ExchangeOrderId> orders;
  };

  using BidLevels = std::map<PriceTicks, Level, std::greater<PriceTicks>>;
  using AskLevels = std::map<PriceTicks, Level>;

  void add(const MarketDataEvent &event);
  void cancel(const MarketDataEvent &event);
  void modify(const MarketDataEvent &event);
  void fill(const MarketDataEvent &event);
  void clear();
  void remove_order(ExchangeOrderId order_id, const Order &order);
  void insert_order_reference(ExchangeOrderId order_id, const Order &order);
  void erase_order_reference(ExchangeOrderId order_id, const Order &order);
  void change_level(Side side, PriceTicks price, Quantity delta);
  [[nodiscard]] Sequence next_revision();
  [[nodiscard]] Sequence next_liquidity_revision();
  [[nodiscard]] static HistoricalOrderSlice
  make_slice(ExchangeOrderId order_id, const Order &order) noexcept;

  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<ExchangeOrderId, Order> orders_;
  Sequence revision_counter_{};
  Sequence liquidity_revision_counter_{};
  Sequence book_revision_{};
  Sequence last_book_source_sequence_{};
  std::uint64_t total_adds_{};
  std::uint64_t total_cancels_{};
  std::uint64_t total_modifies_{};
  std::uint64_t total_fills_{};
  std::uint64_t total_trades_{};
  std::uint64_t total_clears_{};
};

using HistoricalLOB = LimitOrderBook;

} // namespace cmf::market
