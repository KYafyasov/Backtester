import json
import math
from collections import deque
import back_tester as bt
from strategies.common.base_mixin import TrackingMixin


class MLLogisticStrategy(TrackingMixin, bt.Strategy):
    """Торгует по предсказанию LogReg на 10 фичах:
    imbalance, spread, imbalance_ma_5, imbalance_ma_20,
    momentum_5, momentum_20, volatility_20, bid_qty, ask_qty, trade_freq_20.
    trade_freq считается через on_trade — так же, как в offline feature_extraction v3."""

    NAME = "ml_logistic"
    PRICE_SCALE = 1_000_000_000

    def __init__(self, instrument_ids, model_path="my_scripts/ml/logistic_model.json",
                 prob_threshold=0.52, confirmation_steps=3, min_hold_updates=15, order_size=1):
        super().__init__()
        self._init_tracking(instrument_ids)

        with open(model_path) as f:
            model = json.load(f)
        self.feature_names = model["features"]
        self.coef = model["coef"]
        self.intercept = model["intercept"]

        self.prob_threshold = prob_threshold
        self.confirmation_steps = confirmation_steps
        self.min_hold_updates = min_hold_updates
        self.order_size = order_size

        self.imbalance_history = {i: [] for i in self.instrument_ids}
        self.mid_price_history = {i: deque(maxlen=21) for i in self.instrument_ids}
        self.trade_flag_history = {i: deque(maxlen=20) for i in self.instrument_ids}
        self.trades_since_last_row = {i: 0 for i in self.instrument_ids}
        self.signal_streak = {i: 0 for i in self.instrument_ids}
        self.streak_direction = {i: 0 for i in self.instrument_ids}
        self.updates_since_entry = {i: 0 for i in self.instrument_ids}

    def _predict_proba(self, feats):
        z = self.intercept
        for name, w in zip(self.feature_names, self.coef):
            z += w * feats[name]
        return 1.0 / (1.0 + math.exp(-z))

    def on_trade(self, trade):
        self.trades_seen += 1
        instrument_id = trade.instrument_id
        self.trades_since_last_row[instrument_id] = self.trades_since_last_row.get(instrument_id, 0) + 1

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

        best_bid_raw = update.bids[0].price / self.PRICE_SCALE
        best_ask_raw = update.asks[0].price / self.PRICE_SCALE
        best_bid_price = update.bids[0].price
        best_ask_price = update.asks[0].price

        imbalance = (bid_qty - ask_qty) / total
        spread = best_ask_raw - best_bid_raw
        mid_price = (best_bid_raw + best_ask_raw) / 2

        hist = self.imbalance_history[instrument_id]
        hist.append(imbalance)
        if len(hist) > 21:
            hist.pop(0)
        imbalance_ma_5 = sum(hist[-5:]) / len(hist[-5:]) if hist else 0.0
        imbalance_ma_20 = sum(hist[-20:]) / len(hist[-20:]) if hist else 0.0

        mp_hist = self.mid_price_history[instrument_id]
        mp_hist.append(mid_price)
        mp_list = list(mp_hist)

        if len(mp_list) > 5:
            momentum_5 = mp_list[-1] - mp_list[-6]
        else:
            momentum_5 = 0.0

        if len(mp_list) > 20:
            momentum_20 = mp_list[-1] - mp_list[-21]
        else:
            momentum_20 = 0.0

        if len(mp_list) >= 2:
            window = mp_list[-20:] if len(mp_list) >= 20 else mp_list
            mean_v = sum(window) / len(window)
            var_v = sum((v - mean_v) ** 2 for v in window) / len(window)
            volatility_20 = var_v ** 0.5
        else:
            volatility_20 = 0.0

        tf_hist = self.trade_flag_history[instrument_id]
        tf_hist.append(1 if self.trades_since_last_row.get(instrument_id, 0) > 0 else 0)
        self.trades_since_last_row[instrument_id] = 0
        trade_freq_20 = sum(tf_hist) / len(tf_hist) if tf_hist else 0.0

        feats = {
            "imbalance": imbalance,
            "spread": spread,
            "imbalance_ma_5": imbalance_ma_5,
            "imbalance_ma_20": imbalance_ma_20,
            "momentum_5": momentum_5,
            "momentum_20": momentum_20,
            "volatility_20": volatility_20,
            "bid_qty": bid_qty,
            "ask_qty": ask_qty,
            "trade_freq_20": trade_freq_20,
        }
        prob_up = self._predict_proba(feats)

        pos = self.position(instrument_id)
        current_position = pos.net_quantity
        self.last_known_position[instrument_id] = pos
        self.updates_since_entry[instrument_id] += 1

        if prob_up > self.prob_threshold:
            raw_direction = 1
        elif prob_up < (1 - self.prob_threshold):
            raw_direction = -1
        else:
            raw_direction = 0

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
