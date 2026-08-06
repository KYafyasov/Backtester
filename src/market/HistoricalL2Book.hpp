#pragma once

#include "core/Events.hpp"
#include "market/LimitOrderBook.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cmf::market {

// Aggregated snapshot state. It deliberately has no exchange-order identity
// and cannot be queried as L3 liquidity.
class HistoricalL2Book {
public:
  void replace_snapshot(std::span<const BookLevel> bids,
                        std::span<const BookLevel> asks,
                        Sequence source_sequence);

  [[nodiscard]] std::optional<HistoricalBookLevel> best_bid() const;
  [[nodiscard]] std::optional<HistoricalBookLevel> best_ask() const;
  void write_top_bids(std::size_t depth, std::vector<BookLevel> &output) const;
  void write_top_asks(std::size_t depth, std::vector<BookLevel> &output) const;
  [[nodiscard]] Sequence last_book_source_sequence() const noexcept {
    return last_book_source_sequence_;
  }

private:
  std::vector<BookLevel> bids_;
  std::vector<BookLevel> asks_;
  Sequence revision_{};
  Sequence last_book_source_sequence_{};
};

} // namespace cmf::market
