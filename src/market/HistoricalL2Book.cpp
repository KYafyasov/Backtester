#include "market/HistoricalL2Book.hpp"

#include <algorithm>
#include <limits>

namespace cmf::market {
namespace {

void validate_levels(std::span<const BookLevel> levels, Side side) {
  for (std::size_t index = 0; index < levels.size(); ++index) {
    const auto &level = levels[index];
    if (level.price <= 0 || level.quantity <= 0) {
      throw BookError("L2 snapshot contains a non-positive price or quantity");
    }
    if (index == 0) {
      continue;
    }
    const bool ordered = side == Side::Buy
                             ? levels[index - 1].price > level.price
                             : levels[index - 1].price < level.price;
    if (!ordered) {
      throw BookError("L2 snapshot levels are not strictly price ordered");
    }
  }
}

void write_top(std::span<const BookLevel> input, std::size_t depth,
               std::vector<BookLevel> &output) {
  const auto count = std::min(depth, input.size());
  output.assign(input.begin(),
                input.begin() + static_cast<std::ptrdiff_t>(count));
}

} // namespace

void HistoricalL2Book::replace_snapshot(std::span<const BookLevel> bids,
                                        std::span<const BookLevel> asks,
                                        Sequence source_sequence) {
  if (source_sequence <= last_book_source_sequence_ &&
      last_book_source_sequence_ != 0) {
    throw BookError("L2 snapshot source sequence must increase");
  }
  validate_levels(bids, Side::Buy);
  validate_levels(asks, Side::Sell);
  if (!bids.empty() && !asks.empty() &&
      bids.front().price >= asks.front().price) {
    throw BookError("L2 snapshot is locked or crossed");
  }
  if (revision_ == std::numeric_limits<Sequence>::max()) {
    throw BookError("L2 book revision overflow");
  }
  bids_.assign(bids.begin(), bids.end());
  asks_.assign(asks.begin(), asks.end());
  ++revision_;
  last_book_source_sequence_ = source_sequence;
}

std::optional<HistoricalBookLevel> HistoricalL2Book::best_bid() const {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return HistoricalBookLevel{bids_.front().price, bids_.front().quantity,
                             revision_};
}

std::optional<HistoricalBookLevel> HistoricalL2Book::best_ask() const {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return HistoricalBookLevel{asks_.front().price, asks_.front().quantity,
                             revision_};
}

std::optional<HistoricalBookLevel>
HistoricalL2Book::level(Side side, PriceTicks price) const {
  const auto &levels = side == Side::Buy ? bids_ : asks_;
  if (side != Side::Buy && side != Side::Sell) {
    throw BookError("cannot query an L2 level for Side::None");
  }
  const auto iterator = std::find_if(
      levels.begin(), levels.end(),
      [price](const BookLevel &level) { return level.price == price; });
  if (iterator == levels.end()) {
    return std::nullopt;
  }
  return HistoricalBookLevel{iterator->price, iterator->quantity, revision_};
}

void HistoricalL2Book::write_top_bids(std::size_t depth,
                                      std::vector<BookLevel> &output) const {
  write_top(bids_, depth, output);
}

void HistoricalL2Book::write_top_asks(std::size_t depth,
                                      std::vector<BookLevel> &output) const {
  write_top(asks_, depth, output);
}

} // namespace cmf::market
