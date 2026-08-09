import back_tester as bt
from strategies.common.base_mixin import TrackingMixin


class MeanReversionStrategy(TrackingMixin, bt.Strategy):
    """Mean-reversion: если mid-price сильно отклонился от скользящей средней — ставка на возврат."""

    NAME = "mean_reversion"

    def __init__(self, instrument_ids, lookback=20, threshold_ticks=3, order_size=1):
        super().__init__()
        self._init_tracking(instrument_ids)
        self.lookback = lookback
        self.threshold_ticks = threshold_ticks
        self.order_size = order_size
        self.price_history = {i: [] for i in self.instrument_ids}

    def on_book_update(self, update):
        self.book_updates += 1
        if not update.bids or not update.asks:
            return

        instrument_id = update.instrument_id
        best_bid_price = update.bids[0].price
        best_ask_price = update.asks[0].price
        mid_price = (best_bid_price + best_ask_price) / 2 / 1_000_000_000

        hist = self.price_history[instrument_id]
        hist.append(mid_price)
        if len(hist) > self.lookback:
            hist.pop(0)

        pos = self.position(instrument_id)
        current_position = pos.net_quantity
        self.last_known_position[instrument_id] = pos

        if len(hist) < self.lookback:
            return

        avg_price = sum(hist) / len(hist)
        deviation = mid_price - avg_price

        # цена сильно выше среднего -> ждём возврата вниз -> продаём
        if deviation > self.threshold_ticks and current_position >= 0:
            self.submit_limit(instrument_id, bt.Side.SELL, best_bid_price, self.order_size)
            self.orders_sent += 1
        # цена сильно ниже среднего -> ждём возврата вверх -> покупаем
        elif deviation < -self.threshold_ticks and current_position <= 0:
            self.submit_limit(instrument_id, bt.Side.BUY, best_ask_price, self.order_size)
            self.orders_sent += 1
