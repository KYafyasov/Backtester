#include "market/HistoricalLOBStore.hpp"

#include <algorithm>

namespace cmf::market {

LimitOrderBook &HistoricalLOBStore::apply(const MarketDataEvent &event) {
  if (l2_books_.contains(event.instrument_id)) {
    throw BookError("cannot mix MBO and L2 state for one instrument");
  }
  auto &book = books_[event.instrument_id];
  if (!book) {
    book = std::make_unique<LimitOrderBook>();
  }
  book->apply(event);
  return *book;
}

HistoricalL2Book &HistoricalLOBStore::replace_snapshot(
    InstrumentId instrument_id, std::span<const BookLevel> bids,
    std::span<const BookLevel> asks, Sequence source_sequence) {
  if (books_.contains(instrument_id)) {
    throw BookError("cannot mix L2 and MBO state for one instrument");
  }
  auto &book = l2_books_[instrument_id];
  if (!book) {
    book = std::make_unique<HistoricalL2Book>();
  }
  book->replace_snapshot(bids, asks, source_sequence);
  return *book;
}

LimitOrderBook *HistoricalLOBStore::find(InstrumentId instrument_id) noexcept {
  const auto iterator = books_.find(instrument_id);
  return iterator == books_.end() ? nullptr : iterator->second.get();
}

const LimitOrderBook *
HistoricalLOBStore::find(InstrumentId instrument_id) const noexcept {
  const auto iterator = books_.find(instrument_id);
  return iterator == books_.end() ? nullptr : iterator->second.get();
}

std::vector<InstrumentId> HistoricalLOBStore::instrument_ids() const {
  std::vector<InstrumentId> result;
  result.reserve(books_.size() + l2_books_.size());
  for (const auto &[instrument_id, book] : books_) {
    (void)book;
    result.push_back(instrument_id);
  }
  for (const auto &[instrument_id, book] : l2_books_) {
    (void)book;
    result.push_back(instrument_id);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::optional<HistoricalBookLevel>
HistoricalLOBStore::best_bid(InstrumentId instrument_id) const {
  if (const auto *book = find(instrument_id); book != nullptr) {
    return book->best_bid();
  }
  const auto iterator = l2_books_.find(instrument_id);
  return iterator == l2_books_.end() ? std::nullopt
                                     : iterator->second->best_bid();
}

std::optional<HistoricalBookLevel>
HistoricalLOBStore::best_ask(InstrumentId instrument_id) const {
  if (const auto *book = find(instrument_id); book != nullptr) {
    return book->best_ask();
  }
  const auto iterator = l2_books_.find(instrument_id);
  return iterator == l2_books_.end() ? std::nullopt
                                     : iterator->second->best_ask();
}

std::optional<HistoricalBookLevel>
HistoricalLOBStore::level(InstrumentId instrument_id, Side side,
                          PriceTicks price) const {
  if (const auto *book = find(instrument_id); book != nullptr) {
    return book->level(side, price);
  }
  const auto iterator = l2_books_.find(instrument_id);
  return iterator == l2_books_.end() ? std::nullopt
                                     : iterator->second->level(side, price);
}

void HistoricalLOBStore::write_top_bids(InstrumentId instrument_id,
                                        std::size_t depth,
                                        std::vector<BookLevel> &output) const {
  if (const auto *book = find(instrument_id); book != nullptr) {
    book->write_top_bids(depth, output);
    return;
  }
  const auto iterator = l2_books_.find(instrument_id);
  if (iterator == l2_books_.end()) {
    throw BookError("unknown historical instrument");
  }
  iterator->second->write_top_bids(depth, output);
}

void HistoricalLOBStore::write_top_asks(InstrumentId instrument_id,
                                        std::size_t depth,
                                        std::vector<BookLevel> &output) const {
  if (const auto *book = find(instrument_id); book != nullptr) {
    book->write_top_asks(depth, output);
    return;
  }
  const auto iterator = l2_books_.find(instrument_id);
  if (iterator == l2_books_.end()) {
    throw BookError("unknown historical instrument");
  }
  iterator->second->write_top_asks(depth, output);
}

Sequence HistoricalLOBStore::last_book_source_sequence(
    InstrumentId instrument_id) const {
  if (const auto *book = find(instrument_id); book != nullptr) {
    return book->last_book_source_sequence();
  }
  const auto iterator = l2_books_.find(instrument_id);
  if (iterator == l2_books_.end()) {
    return 0;
  }
  return iterator->second->last_book_source_sequence();
}

} // namespace cmf::market
