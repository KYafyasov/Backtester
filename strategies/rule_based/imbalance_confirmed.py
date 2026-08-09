import back_tester as bt
from strategies.common.base_mixin import TrackingMixin


class ImbalanceConfirmedStrategy(TrackingMixin, bt.Strategy):
    """Imbalance с confirmation (N шагов подряд в одну сторону перед входом)
    и min_hold (нельзя развернуть позицию раньше, чем через M обновлений).
    Та же идея, что cost-aware imbalance из прошлой валидации (Task 3/4)."""

    NAME = "imbalance_confirmed"

    def __init__(self, instrument_ids, threshold=0.3, confirmation_steps=3,
                 min_hold_updates=15, order_size=1):
        super().__init__()
        self._init_tracking(instrument_ids)
        self.threshold = threshold
        self.confirmation_steps = confirmation_steps
        self.min_hold_updates = min_hold_updates
        self.order_size = order_size
        self.signal_streak = {i: 0 for i in self.instrument_ids}  # подряд идущие сигналы в одну сторону
        self.streak_direction = {i: 0 for i in self.instrument_ids}
        self.updates_since_entry = {i: 0 for i in self.instrument_ids}

    def on_book_update(self, update):
        self.book_updates += 1
        if not update.bids or not update.asks:
            return

        instrument_id = update.instrument_id
        bid_qty = sum(level.quantity for level in update.bids)
        ask_qty = sum(level.quantity for level in update.asks)
        total = bid_qty + ask_qty
        if total == 0:
            return

        imbalance = (bid_qty - ask_qty) / total
        best_bid_price = update.bids[0].price
        best_ask_price = update.asks[0].price

        pos = self.position(instrument_id)
        current_position = pos.net_quantity
        self.last_known_position[instrument_id] = pos
        self.updates_since_entry[instrument_id] += 1

        # определяем текущее "сырое" направление сигнала
        if imbalance > self.threshold:
            raw_direction = 1
        elif imbalance < -self.threshold:
            raw_direction = -1
        else:
            raw_direction = 0

        # обновляем счетчикоподряд идущих сигналов в одну сторону
        if raw_direction != 0 and raw_direction == self.streak_direction[instrument_id]:
            self.signal_streak[instrument_id] += 1
        elif raw_direction != 0:
            self.streak_direction[instrument_id] = raw_direction
            self.signal_streak[instrument_id] = 1
        else:
            self.signal_streak[instrument_id] = 0
            self.streak_direction[instrument_id] = 0

        confirmed = self.signal_streak[instrument_id] >= self.confirmation_steps
        can_flip = self.updates_since_entry[instrument_id] >= self.min_hold_updates

        if not confirmed:
            return

        direction = self.streak_direction[instrument_id]

        if direction == 1 and current_position <= 0 and can_flip:
            self.submit_limit(instrument_id, bt.Side.BUY, best_ask_price, self.order_size)
            self.orders_sent += 1
            self.updates_since_entry[instrument_id] = 0
        elif direction == -1 and current_position >= 0 and can_flip:
            self.submit_limit(instrument_id, bt.Side.SELL, best_bid_price, self.order_size)
            self.orders_sent += 1
            self.updates_since_entry[instrument_id] = 0
