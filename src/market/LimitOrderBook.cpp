#include "market/LimitOrderBook.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace cmf::market {
namespace {

[[nodiscard]] const char *side_name(Side side) noexcept {
  return side == Side::Buy ? "bid" : "ask";
}

template <typename Levels>
[[nodiscard]] std::vector<HistoricalBookLevel> top_levels(const Levels &levels,
                                                          std::size_t depth) {
  std::vector<HistoricalBookLevel> result;
  result.reserve(std::min(depth, levels.size()));
  for (const auto &[price, level] : levels) {
    if (result.size() == depth) {
      break;
    }
    result.push_back({price, level.quantity, level.revision});
  }
  return result;
}

template <typename Levels>
void write_top_levels(const Levels &levels, std::size_t depth,
                      std::vector<BookLevel> &output) {
  output.clear();
  for (const auto &[price, level] : levels) {
    if (output.size() == depth) {
      break;
    }
    output.push_back({price, level.quantity});
  }
}

} // namespace

Sequence LimitOrderBook::next_revision() {
  if (revision_counter_ == std::numeric_limits<Sequence>::max()) {
    throw BookError("historical book revision overflow");
  }
  book_revision_ = ++revision_counter_;
  return book_revision_;
}

Sequence LimitOrderBook::next_liquidity_revision() {
  if (liquidity_revision_counter_ == std::numeric_limits<Sequence>::max()) {
    throw BookError("historical liquidity revision overflow");
  }
  return ++liquidity_revision_counter_;
}

void LimitOrderBook::change_level(Side side, PriceTicks price, Quantity delta) {
  if (side == Side::Buy) {
    auto iterator = bids_.find(price);
    const Quantity current =
        iterator == bids_.end() ? 0 : iterator->second.quantity;
    if ((delta > 0 && current > std::numeric_limits<Quantity>::max() - delta) ||
        (delta < 0 && current < -delta)) {
      throw BookError("invalid or overflowing bid level quantity");
    }
    const Quantity updated = current + delta;
    const Sequence revision = next_revision();
    if (updated == 0) {
      if (iterator != bids_.end()) {
        bids_.erase(iterator);
      }
    } else if (iterator == bids_.end()) {
      bids_.emplace(price, Level{updated, revision, {}});
    } else {
      iterator->second.quantity = updated;
      iterator->second.revision = revision;
    }
    return;
  }

  if (side != Side::Sell) {
    throw BookError("historical order side must be Buy or Sell");
  }
  auto iterator = asks_.find(price);
  const Quantity current =
      iterator == asks_.end() ? 0 : iterator->second.quantity;
  if ((delta > 0 && current > std::numeric_limits<Quantity>::max() - delta) ||
      (delta < 0 && current < -delta)) {
    throw BookError("invalid or overflowing ask level quantity");
  }
  const Quantity updated = current + delta;
  const Sequence revision = next_revision();
  if (updated == 0) {
    if (iterator != asks_.end()) {
      asks_.erase(iterator);
    }
  } else if (iterator == asks_.end()) {
    asks_.emplace(price, Level{updated, revision, {}});
  } else {
    iterator->second.quantity = updated;
    iterator->second.revision = revision;
  }
}

void LimitOrderBook::insert_order_reference(ExchangeOrderId order_id,
                                            const Order &order) {
  auto &level_value =
      order.side == Side::Buy ? bids_.at(order.price) : asks_.at(order.price);
  const auto [iterator, inserted] =
      level_value.orders.emplace(order.priority, order_id);
  (void)iterator;
  if (!inserted) {
    throw BookError("duplicate historical price-time priority key");
  }
}

void LimitOrderBook::erase_order_reference(ExchangeOrderId order_id,
                                           const Order &order) {
  auto &level_value =
      order.side == Side::Buy ? bids_.at(order.price) : asks_.at(order.price);
  const auto erased = level_value.orders.erase(order.priority);
  if (erased != 1) {
    throw BookError("historical order priority index is inconsistent for id " +
                    std::to_string(order_id));
  }
}

void LimitOrderBook::add(const MarketDataEvent &event) {
  if (event.side == Side::None || !event.price_ticks.has_value() ||
      event.quantity <= 0) {
    throw BookError("invalid historical add");
  }
  const auto existing = orders_.find(event.exchange_order_id);
  if (existing != orders_.end()) {
    if (existing->second.side == event.side &&
        existing->second.price == *event.price_ticks &&
        existing->second.quantity == event.quantity) {
      return;
    }
    throw BookError("conflicting duplicate historical order id " +
                    std::to_string(event.exchange_order_id));
  }

  const Order incoming{event.side, *event.price_ticks, event.quantity,
                       next_liquidity_revision(),
                       PriorityKey{event.exchange_ts_ns, event.source_sequence,
                                   event.exchange_order_id}};
  change_level(incoming.side, incoming.price, incoming.quantity);
  orders_.emplace(event.exchange_order_id, incoming);
  insert_order_reference(event.exchange_order_id, incoming);
  ++total_adds_;
}

void LimitOrderBook::remove_order(ExchangeOrderId order_id,
                                  const Order &order) {
  erase_order_reference(order_id, order);
  change_level(order.side, order.price, -order.quantity);
  orders_.erase(order_id);
}

void LimitOrderBook::cancel(const MarketDataEvent &event) {
  const auto iterator = orders_.find(event.exchange_order_id);
  if (iterator == orders_.end()) {
    return;
  }
  const Order order = iterator->second;
  remove_order(event.exchange_order_id, order);
  ++total_cancels_;
}

void LimitOrderBook::modify(const MarketDataEvent &event) {
  const auto iterator = orders_.find(event.exchange_order_id);
  if (iterator == orders_.end()) {
    throw BookError("modify references unknown historical order id " +
                    std::to_string(event.exchange_order_id));
  }
  if (event.side == Side::None || !event.price_ticks.has_value() ||
      event.quantity <= 0) {
    throw BookError("invalid historical modify");
  }

  const Order old_order = iterator->second;
  if (old_order.side == event.side && old_order.price == *event.price_ticks &&
      old_order.quantity == event.quantity) {
    return;
  }

  const Order replacement{
      event.side, *event.price_ticks, event.quantity, next_liquidity_revision(),
      PriorityKey{event.exchange_ts_ns, event.source_sequence,
                  event.exchange_order_id}};
  remove_order(event.exchange_order_id, old_order);
  try {
    change_level(replacement.side, replacement.price, replacement.quantity);
    orders_.emplace(event.exchange_order_id, replacement);
    insert_order_reference(event.exchange_order_id, replacement);
  } catch (...) {
    change_level(old_order.side, old_order.price, old_order.quantity);
    orders_.emplace(event.exchange_order_id, old_order);
    insert_order_reference(event.exchange_order_id, old_order);
    throw;
  }
  ++total_modifies_;
}

void LimitOrderBook::fill(const MarketDataEvent &event) {
  if (event.quantity <= 0) {
    throw BookError("historical fill quantity must be positive");
  }
  const auto iterator = orders_.find(event.exchange_order_id);
  if (iterator == orders_.end()) {
    throw BookError("fill references unknown historical order id " +
                    std::to_string(event.exchange_order_id));
  }
  const Order order = iterator->second;
  if (event.quantity > order.quantity) {
    throw BookError("historical fill exceeds resting quantity");
  }

  if (event.quantity == order.quantity) {
    erase_order_reference(event.exchange_order_id, order);
    change_level(order.side, order.price, -event.quantity);
    orders_.erase(iterator);
  } else {
    change_level(order.side, order.price, -event.quantity);
    iterator->second.quantity -= event.quantity;
  }
  ++total_fills_;
}

void LimitOrderBook::clear() {
  bids_.clear();
  asks_.clear();
  orders_.clear();
  (void)next_revision();
  ++total_clears_;
}

void LimitOrderBook::apply(const MarketDataEvent &event) {
  switch (event.action) {
  case MarketAction::Add:
    add(event);
    break;
  case MarketAction::Cancel:
    cancel(event);
    break;
  case MarketAction::Modify:
    modify(event);
    break;
  case MarketAction::Fill:
    fill(event);
    break;
  case MarketAction::Trade:
    ++total_trades_;
    return;
  case MarketAction::Clear:
    clear();
    break;
  }
  last_book_source_sequence_ = event.source_sequence;
}

std::optional<HistoricalBookLevel> LimitOrderBook::best_bid() const {
  if (bids_.empty()) {
    return std::nullopt;
  }
  const auto &[price, level_value] = *bids_.begin();
  return HistoricalBookLevel{price, level_value.quantity, level_value.revision};
}

std::optional<HistoricalBookLevel> LimitOrderBook::best_ask() const {
  if (asks_.empty()) {
    return std::nullopt;
  }
  const auto &[price, level_value] = *asks_.begin();
  return HistoricalBookLevel{price, level_value.quantity, level_value.revision};
}

std::vector<HistoricalBookLevel>
LimitOrderBook::top_bids(std::size_t depth) const {
  return top_levels(bids_, depth);
}

std::vector<HistoricalBookLevel>
LimitOrderBook::top_asks(std::size_t depth) const {
  return top_levels(asks_, depth);
}

void LimitOrderBook::write_top_bids(std::size_t depth,
                                    std::vector<BookLevel> &output) const {
  write_top_levels(bids_, depth, output);
}

void LimitOrderBook::write_top_asks(std::size_t depth,
                                    std::vector<BookLevel> &output) const {
  write_top_levels(asks_, depth, output);
}

std::optional<HistoricalBookLevel>
LimitOrderBook::level(Side side, PriceTicks price) const {
  if (side == Side::Buy) {
    const auto iterator = bids_.find(price);
    if (iterator == bids_.end()) {
      return std::nullopt;
    }
    return HistoricalBookLevel{price, iterator->second.quantity,
                               iterator->second.revision};
  }
  if (side == Side::Sell) {
    const auto iterator = asks_.find(price);
    if (iterator == asks_.end()) {
      return std::nullopt;
    }
    return HistoricalBookLevel{price, iterator->second.quantity,
                               iterator->second.revision};
  }
  throw BookError(std::string("cannot query ") + side_name(side) +
                  " level for Side::None");
}

HistoricalOrderSlice LimitOrderBook::make_slice(ExchangeOrderId order_id,
                                                const Order &order) noexcept {
  return HistoricalOrderSlice{order_id,
                              order.side,
                              order.price,
                              order.quantity,
                              order.liquidity_revision,
                              order.priority.exchange_ts_ns,
                              order.priority.source_sequence};
}

std::optional<HistoricalOrderSlice>
LimitOrderBook::order_slice(ExchangeOrderId exchange_order_id) const {
  const auto order = orders_.find(exchange_order_id);
  if (order == orders_.end()) {
    return std::nullopt;
  }
  return make_slice(exchange_order_id, order->second);
}

} // namespace cmf::market
