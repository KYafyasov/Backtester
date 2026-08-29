"""DQN на synthetic данных (быстрый режим). Лёгкая Python-среда, без прогона через C++ движок
(для скорости обучения — движок был бы слишком медленным для миллионов RL-шагов)."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json
import random
import numpy as np
import torch
import torch.nn as nn
from collections import deque

DATA_PATH = "synthetic_signal_subset.jsonl"  # быстрый режим, 1М событий
FEATURE_COLS = ["imbalance", "spread", "imbalance_ma_5", "imbalance_ma_20", "momentum_5"]
STATE_DIM = len(FEATURE_COLS) + 1  # + текущая позиция

EPISODES = 5
EPSILON_START = 1.0
EPSILON_END = 0.05
EPSILON_DECAY_STEPS = 200_000
GAMMA = 0.99
LR = 1e-3
BATCH_SIZE = 256
REPLAY_CAPACITY = 50_000
TARGET_UPDATE_EVERY = 2000
TRANSACTION_COST = 0.5  # штраф за пересечение спреда при смене позиции (в единицах цены)


def load_trajectory(path):
    """Собирает последовательность (features, mid_price) по инструменту 1 (для простоты одна серия)."""
    state = {"best_bid": None, "best_ask": None, "bid_qty": 0, "ask_qty": 0}
    imbalance_hist = []
    mid_hist = []
    trajectory = []

    with open(path) as f:
        for line in f:
            event = json.loads(line)
            if event["hd"]["instrument_id"] != 1:
                continue
            action = event["action"]
            if action == "T":
                continue
            if action in ("A", "M"):
                price = int(event["price"])
                size = event["size"]
                side = event["side"]
                if side == "B":
                    state["best_bid"] = price
                    state["bid_qty"] = size
                elif side == "A":
                    state["best_ask"] = price
                    state["ask_qty"] = size
            else:
                continue

            if state["best_bid"] is None or state["best_ask"] is None:
                continue
            bid_qty, ask_qty = state["bid_qty"], state["ask_qty"]
            total = bid_qty + ask_qty
            if total == 0:
                continue

            imbalance = (bid_qty - ask_qty) / total
            spread = state["best_ask"] - state["best_bid"]
            mid = (state["best_bid"] + state["best_ask"]) / 2

            imbalance_hist.append(imbalance)
            if len(imbalance_hist) > 21:
                imbalance_hist.pop(0)
            imbalance_ma_5 = sum(imbalance_hist[-5:]) / len(imbalance_hist[-5:])
            imbalance_ma_20 = sum(imbalance_hist[-20:]) / len(imbalance_hist[-20:])

            mid_hist.append(mid)
            if len(mid_hist) > 6:
                mid_hist.pop(0)
            momentum_5 = mid_hist[-1] - mid_hist[0] if len(mid_hist) == 6 else 0.0

            feats = [imbalance, spread, imbalance_ma_5, imbalance_ma_20, momentum_5]
            trajectory.append((feats, mid))

    return trajectory


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


def main():
    device = torch.device("cpu")  # sequential env loop — GPU не даёт выигрыша здесь
    print("Загрузка траектории...")
    trajectory = load_trajectory(DATA_PATH)
    print(f"Длина траектории: {len(trajectory)}")

    q_net = QNet(STATE_DIM).to(device)
    target_net = QNet(STATE_DIM).to(device)
    target_net.load_state_dict(q_net.state_dict())
    optimizer = torch.optim.Adam(q_net.parameters(), lr=LR)

    replay = deque(maxlen=REPLAY_CAPACITY)
    global_step = 0

    for episode in range(EPISODES):
        position = 0  # -1, 0, +1
        entry_price = 0.0
        cum_reward = 0.0

        for t in range(len(trajectory) - 1):
            feats, mid = trajectory[t]
            state = np.array(feats + [position], dtype=np.float32)

            epsilon = max(EPSILON_END, EPSILON_START - global_step / EPSILON_DECAY_STEPS)
            if random.random() < epsilon:
                action = random.randint(0, 2)  # 0=hold, 1=buy(+1), 2=sell(-1)
            else:
                with torch.no_grad():
                    q = q_net(torch.tensor(state, device=device).unsqueeze(0))
                    action = int(q.argmax(dim=1).item())

            target_position = {0: position, 1: 1, 2: -1}[action]
            next_feats, next_mid = trajectory[t + 1]

            # награда: изменение mark-to-market PnL минус издержки при смене позиции
            reward = position * (next_mid - mid)
            if target_position != position:
                reward -= TRANSACTION_COST
            position = target_position

            next_state = np.array(next_feats + [position], dtype=np.float32)
            replay.append((state, action, reward, next_state))
            cum_reward += reward
            global_step += 1

            if len(replay) >= BATCH_SIZE:
                batch = random.sample(replay, BATCH_SIZE)
                s_b = torch.tensor(np.array([b[0] for b in batch]), device=device)
                a_b = torch.tensor([b[1] for b in batch], device=device)
                r_b = torch.tensor([b[2] for b in batch], dtype=torch.float32, device=device)
                ns_b = torch.tensor(np.array([b[3] for b in batch]), device=device)

                q_values = q_net(s_b).gather(1, a_b.unsqueeze(1)).squeeze(1)
                with torch.no_grad():
                    next_q = target_net(ns_b).max(dim=1)[0]
                    target = r_b + GAMMA * next_q

                loss = nn.functional.mse_loss(q_values, target)
                optimizer.zero_grad()
                loss.backward()
                optimizer.step()

            if global_step % TARGET_UPDATE_EVERY == 0:
                target_net.load_state_dict(q_net.state_dict())

        print(f"Episode {episode+1}/{EPISODES}: cum_reward={cum_reward:.2f} epsilon={epsilon:.3f}")

    torch.save(q_net.state_dict(), "my_scripts/ml/dqn_model.pt")
    with open("my_scripts/ml/dqn_meta.json", "w") as f:
        json.dump({"features": FEATURE_COLS, "state_dim": STATE_DIM}, f, indent=2)
    print("Модель сохранена в my_scripts/ml/dqn_model.pt")


if __name__ == "__main__":
    main()
