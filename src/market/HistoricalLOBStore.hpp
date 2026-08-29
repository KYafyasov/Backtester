#pragma once

#include "market/HistoricalL2Book.hpp"
#include "market/LimitOrderBook.hpp"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cmf::market {

class HistoricalLOBStore {
public:
  LimitOrderBook &apply(const MarketDataEvent &event);
  HistoricalL2Book &replace_snapshot(InstrumentId instrument_id,
                                     std::span<const BookLevel> bids,
                                     std::span<const BookLevel> asks,
                                     Sequence source_sequence);

  [[nodiscard]] LimitOrderBook *find(InstrumentId instrument_id) noexcept;
  [[nodiscard]] const LimitOrderBook *
  find(InstrumentId instrument_id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept {
    return books_.size() + l2_books_.size();
  }
  [[nodiscard]] std::optional<HistoricalBookLevel>
  best_bid(InstrumentId instrument_id) const;
  [[nodiscard]] std::optional<HistoricalBookLevel>
  best_ask(InstrumentId instrument_id) const;
  [[nodiscard]] std::optional<HistoricalBookLevel>
  level(InstrumentId instrument_id, Side side, PriceTicks price) const;
  void write_top_bids(InstrumentId instrument_id, std::size_t depth,
                      std::vector<BookLevel> &output) const;
  void write_top_asks(InstrumentId instrument_id, std::size_t depth,
                      std::vector<BookLevel> &output) const;
  [[nodiscard]] Sequence
  last_book_source_sequence(InstrumentId instrument_id) const;
  [[nodiscard]] std::vector<InstrumentId> instrument_ids() const;

private:
  std::unordered_map<InstrumentId, std::unique_ptr<LimitOrderBook>> books_;
  std::unordered_map<InstrumentId, std::unique_ptr<HistoricalL2Book>> l2_books_;
};

} // namespace cmf::market
