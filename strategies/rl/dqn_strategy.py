"""
DQNStrategy — встраивает уже обученный DQN (Q-network) в C++ backtesting
движок как обычную bt.Strategy. Никаких изменений в C++ коде не требуется.

ВАЖНО: state для Q-сети использует РЕАЛЬНУЮ позицию из движка
(self.position(instrument_id).net_quantity), а не параллельно отслеживаемую
переменную — параллельный трекер рассинхронизировался с реальным
исполнением ордеров (order_latency_ns) и приводил к ошибке
"modify references unknown historical order id".
"""
import json
from collections import deque
import torch
import torch.nn as nn
import back_tester as bt
from strategies.common.base_mixin import TrackingMixin


class QNet(nn.Module):
    def __init__(self, state_dim, n_actions=3):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(state_dim, 64), nn.ReLU(),
            nn.Linear(64, 64), nn.ReLU(),
            nn.Linear(64, n_actions),
        )

    def forward(self, x):
        return self.net(x)


class DQNStrategy(TrackingMixin, bt.Strategy):
    """Торгует по Q-значениям обученного DQN. Состояние на каждом book
    update: [imbalance, spread, imbalance_ma_5, imbalance_ma_20,
    momentum_5, текущая РЕАЛЬНАЯ позиция (знак net_quantity)].
    Действия: 0=держать, 1=LONG, 2=SHORT. epsilon=0."""

    NAME = "dqn"
    PRICE_SCALE = 1_000_000_000

    def __init__(self, instrument_ids, model_path="ml/dqn_model_traintest.pt",
                 meta_path="ml/dqn_meta_traintest.json", order_size=1,
                 min_hold_updates=15):
        super().__init__()
        self._init_tracking(instrument_ids)

        with open(meta_path) as f:
            meta = json.load(f)
        self.state_dim = meta["state_dim"]

        self.device = torch.device("cpu")
        self.model = QNet(self.state_dim)
        self.model.load_state_dict(torch.load(model_path, map_location=self.device))
        self.model.eval()

        self.order_size = order_size
        self.min_hold_updates = min_hold_updates

        self.imbalance_history = {i: [] for i in self.instrument_ids}
        self.mid_price_history = {i: deque(maxlen=6) for i in self.instrument_ids}
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

        best_bid_price = update.bids[0].price
        best_ask_price = update.asks[0].price
        best_bid_raw = update.bids[0].price / self.PRICE_SCALE
        best_ask_raw = update.asks[0].price / self.PRICE_SCALE

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
        momentum_5 = mp_list[-1] - mp_list[0] if len(mp_list) == 6 else 0.0

        # РЕАЛЬНАЯ позиция из движка — единственный источник истины
        pos = self.position(instrument_id)
        current_position = pos.net_quantity
        self.last_known_position[instrument_id] = pos
        self.updates_since_entry[instrument_id] += 1

        # знак реальной позиции для state (-1/0/1), т.к. DQN обучался
        # на упрощённой позиции, не на реальный net_quantity размер
        position_sign = 0 if current_position == 0 else (1 if current_position > 0 else -1)

        state_vec = torch.tensor(
            [imbalance, spread, imbalance_ma_5, imbalance_ma_20, momentum_5, position_sign],
            dtype=torch.float32,
        )

        with torch.no_grad():
            q = self.model(state_vec.unsqueeze(0))
            action = int(q.argmax(dim=1).item())

        target_position = {0: position_sign, 1: 1, 2: -1}[action]

        # min_hold_updates — не флипаем позицию слишком часто, как у остальных
        # ML-стратегий проекта (confirmation_steps/min_hold_updates паттерн)
        can_flip = self.updates_since_entry[instrument_id] >= self.min_hold_updates
        if not can_flip or target_position == position_sign:
            return

        if target_position == 1 and current_position <= 0:
            self.submit_limit(instrument_id, bt.Side.BUY, best_ask_price, self.order_size)
            self.orders_sent += 1
            self.updates_since_entry[instrument_id] = 0
        elif target_position == -1 and current_position >= 0:
            self.submit_limit(instrument_id, bt.Side.SELL, best_bid_price, self.order_size)
            self.orders_sent += 1
            self.updates_since_entry[instrument_id] = 0
        elif target_position == 0:
            if current_position > 0:
                self.submit_limit(instrument_id, bt.Side.SELL, best_bid_price, self.order_size)
                self.orders_sent += 1
                self.updates_since_entry[instrument_id] = 0
            elif current_position < 0:
                self.submit_limit(instrument_id, bt.Side.BUY, best_ask_price, self.order_size)
                self.orders_sent += 1
                self.updates_since_entry[instrument_id] = 0
