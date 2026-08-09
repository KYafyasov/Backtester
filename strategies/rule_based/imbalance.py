import back_tester as bt
from strategies.common.base_mixin import TrackingMixin


class ImbalanceStrategy(TrackingMixin, bt.Strategy):
    """Top-of-book order-flow imbalance: (bid_qty - ask_qty) / total."""

    NAME = "imbalance"

    def __init__(self, instrument_ids, threshold=0.3, order_size=1):
        super().__init__()
        self._init_tracking(instrument_ids)
        self.threshold = threshold
        self.order_size = order_size

    def on_book_update(self, update):
        self.book_updates += 1
        if not update.bids or not update.asks:
            return

        bid_qty = sum(level.quantity for level in update.bids)
        ask_qty = sum(level.quantity for level in update.asks)
        total = bid_qty + ask_qty
        if total == 0:
            return

        imbalance = (bid_qty - ask_qty) / total
        instrument_id = update.instrument_id
        best_bid_price = update.bids[0].price
        best_ask_price = update.asks[0].price

        pos = self.position(instrument_id)
        current_position = pos.net_quantity
        self.last_known_position[instrument_id] = pos

        if imbalance > self.threshold and current_position <= 0:
            self.submit_limit(instrument_id, bt.Side.BUY, best_ask_price, self.order_size)
            self.orders_sent += 1
        elif imbalance < -self.threshold and current_position >= 0:
            self.submit_limit(instrument_id, bt.Side.SELL, best_bid_price, self.order_size)
            self.orders_sent += 1
