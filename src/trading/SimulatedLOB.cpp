#include "trading/SimulatedLOB.hpp"

#include <tuple>

namespace cmf::trading {

bool EngineView::BuyFirst::operator()(const RestingKey &left,
                                      const RestingKey &right) const noexcept {
  return std::tuple{-left.price, left.arrival_sequence, left.client_order_id} <
         std::tuple{-right.price, right.arrival_sequence,
                    right.client_order_id};
}

bool EngineView::SellFirst::operator()(const RestingKey &left,
                                       const RestingKey &right) const noexcept {
  return std::tie(left.price, left.arrival_sequence, left.client_order_id) <
         std::tie(right.price, right.arrival_sequence, right.client_order_id);
}

EngineView::EngineView(std::span<const InstrumentMeta> instruments) {
  resting_.reserve(instruments.size());
  for (const auto &instrument : instruments) {
    if (!resting_.try_emplace(instrument.instrument_id).second) {
      throw std::invalid_argument("duplicate EngineView instrument");
    }
  }
}

SimulatedLOB::SimulatedLOB(std::span<const InstrumentMeta> instruments)
    : view_(instruments) {
  fills_.reserve(8);
}

std::span<const SyntheticFill>
SimulatedLOB::accept(ClOrdId client_order_id, InstrumentId instrument_id,
                     Side side, PriceTicks limit_price,
                     Quantity remaining_quantity, Sequence arrival_sequence,
                     const market::LimitOrderBook *book) {
  return accept_with_touch(
      client_order_id, instrument_id, side, limit_price, remaining_quantity,
      arrival_sequence,
      book == nullptr || !book->best_bid().has_value()
          ? std::nullopt
          : std::optional<PriceTicks>{book->best_bid()->price},
      book == nullptr || !book->best_ask().has_value()
          ? std::nullopt
          : std::optional<PriceTicks>{book->best_ask()->price},
      book == nullptr ? 0 : book->last_book_source_sequence());
}

std::span<const SyntheticFill> SimulatedLOB::accept_from_store(
    ClOrdId client_order_id, InstrumentId instrument_id, Side side,
    PriceTicks limit_price, Quantity remaining_quantity,
    Sequence arrival_sequence, const market::HistoricalLOBStore *books) {
  const auto bid =
      books == nullptr ? std::nullopt : books->best_bid(instrument_id);
  const auto ask =
      books == nullptr ? std::nullopt : books->best_ask(instrument_id);
  return accept_with_touch(
      client_order_id, instrument_id, side, limit_price, remaining_quantity,
      arrival_sequence,
      bid.has_value() ? std::optional<PriceTicks>{bid->price} : std::nullopt,
      ask.has_value() ? std::optional<PriceTicks>{ask->price} : std::nullopt,
      books == nullptr ? 0 : books->last_book_source_sequence(instrument_id));
}

std::span<const SyntheticFill> SimulatedLOB::accept_with_touch(
    ClOrdId client_order_id, InstrumentId instrument_id, Side side,
    PriceTicks limit_price, Quantity remaining_quantity,
    Sequence arrival_sequence, std::optional<PriceTicks> best_bid,
    std::optional<PriceTicks> best_ask, Sequence source_sequence) {
  fills_.clear();
  if (view_.resting_.find(instrument_id) == view_.resting_.end()) {
    throw SimulatedLOBError("accepted order references unknown instrument");
  }
  EngineView::PrivateOrder order{
      instrument_id, client_order_id,    side,
      limit_price,   remaining_quantity, arrival_sequence};
  const auto [iterator, inserted] =
      view_.orders_.emplace(client_order_id, order);
  if (!inserted) {
    throw SimulatedLOBError("duplicate private order");
  }
  insert_resting(iterator->second);
  if (best_bid.has_value() || best_ask.has_value()) {
    match_prices(instrument_id, best_ask, best_bid, LiquiditySource::QuoteCross,
                 source_sequence);
  }
  return fills_;
}

std::span<const SyntheticFill>
SimulatedLOB::on_signal(const PriceCrossSignal &signal) {
  fills_.clear();
  if (signal.source == PriceCrossSource::BestQuote) {
    if (signal.trade_price.has_value()) {
      throw SimulatedLOBError("best-quote signal carries a trade price");
    }
    match_prices(signal.instrument_id, signal.best_ask, signal.best_bid,
                 LiquiditySource::QuoteCross, signal.source_sequence,
                 signal.source_id, signal.global_market_sequence);
  } else if (signal.source == PriceCrossSource::Trade) {
    if (!signal.trade_price.has_value() || signal.best_bid.has_value() ||
        signal.best_ask.has_value()) {
      throw SimulatedLOBError("trade signal has invalid price fields");
    }
    match_prices(signal.instrument_id, signal.trade_price, signal.trade_price,
                 LiquiditySource::TradeCross, signal.source_sequence,
                 signal.source_id, signal.global_market_sequence);
  } else {
    throw SimulatedLOBError("price-cross signal has invalid source");
  }
  return fills_;
}

void SimulatedLOB::cancel(ClOrdId client_order_id) {
  const auto iterator = view_.orders_.find(client_order_id);
  if (iterator == view_.orders_.end()) {
    throw SimulatedLOBError("cancel references unknown private order");
  }
  erase_resting(iterator->second);
  view_.orders_.erase(iterator);
}

void SimulatedLOB::match_prices(InstrumentId instrument_id,
                                std::optional<PriceTicks> buy_trigger,
                                std::optional<PriceTicks> sell_trigger,
                                LiquiditySource liquidity_source,
                                Sequence trigger_source_sequence,
                                SourceId trigger_source_id,
                                Sequence trigger_global_market_sequence) {
  auto instrument = view_.resting_.find(instrument_id);
  if (instrument == view_.resting_.end()) {
    throw SimulatedLOBError("price-cross signal references unknown instrument");
  }

  auto process = [&](auto &index, std::optional<PriceTicks> trigger,
                     Side side) {
    while (trigger.has_value() && !index.empty()) {
      const ClOrdId id = index.begin()->second;
      auto own = view_.orders_.find(id);
      if (own == view_.orders_.end()) {
        throw SimulatedLOBError("resting-order index is inconsistent");
      }
      const bool crossed = side == Side::Buy
                               ? *trigger <= own->second.limit_price
                               : *trigger >= own->second.limit_price;
      if (!crossed) {
        break;
      }
      const Quantity fill_quantity = own->second.remaining_quantity;
      erase_resting(own->second);
      own->second.remaining_quantity = 0;
      fills_.push_back(SyntheticFill{own->second.client_order_id, *trigger,
                                     fill_quantity, liquidity_source,
                                     trigger_source_sequence, trigger_source_id,
                                     trigger_global_market_sequence});
      view_.orders_.erase(own);
    }
  };
  process(instrument->second.buys, buy_trigger, Side::Buy);
  process(instrument->second.sells, sell_trigger, Side::Sell);
}

void SimulatedLOB::insert_resting(const EngineView::PrivateOrder &order) {
  if (order.remaining_quantity == 0) {
    return;
  }
  const EngineView::RestingKey key{order.limit_price, order.arrival_sequence,
                                   order.client_order_id};
  auto &instrument = view_.resting_.at(order.instrument_id);
  if (order.side == Side::Buy) {
    instrument.buys.emplace(key, order.client_order_id);
  } else if (order.side == Side::Sell) {
    instrument.sells.emplace(key, order.client_order_id);
  } else {
    throw SimulatedLOBError("private order has invalid side");
  }
}

void SimulatedLOB::erase_resting(const EngineView::PrivateOrder &order) {
  const EngineView::RestingKey key{order.limit_price, order.arrival_sequence,
                                   order.client_order_id};
  auto &instrument = view_.resting_.at(order.instrument_id);
  if (order.side == Side::Buy) {
    instrument.buys.erase(key);
  } else if (order.side == Side::Sell) {
    instrument.sells.erase(key);
  }
}

} // namespace cmf::trading
