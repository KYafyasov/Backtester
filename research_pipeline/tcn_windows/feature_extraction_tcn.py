"""Извлечение окон последовательностей для TCN. Запускается из Windows-Python (py)."""
import json
import numpy as np


def replay_and_extract_sequences(path, horizon=20, window=20):
    state = {}
    tick_history = {}
    mid_price_prev = {}
    sequences = []

    with open(path) as f:
        for line in f:
            event = json.loads(line)
            instr_id = event["hd"]["instrument_id"]
            action = event["action"]

            if instr_id not in state:
                state[instr_id] = {"best_bid": None, "best_ask": None, "bid_qty": 0, "ask_qty": 0}
                tick_history[instr_id] = []
                mid_price_prev[instr_id] = None

            s = state[instr_id]

            if action == "T":
                continue

            if action in ("A", "M"):
                price = int(event["price"])
                size = event["size"]
                side = event["side"]
                if side == "B":
                    s["best_bid"] = price
                    s["bid_qty"] = size
                elif side == "A":
                    s["best_ask"] = price
                    s["ask_qty"] = size
            else:
                continue

            if s["best_bid"] is None or s["best_ask"] is None:
                continue

            bid_qty, ask_qty = s["bid_qty"], s["ask_qty"]
            total = bid_qty + ask_qty
            if total == 0:
                continue

            imbalance = (bid_qty - ask_qty) / total
            spread = s["best_ask"] - s["best_bid"]
            mid_price = (s["best_bid"] + s["best_ask"]) / 2

            prev_mp = mid_price_prev[instr_id]
            mid_price_delta = (mid_price - prev_mp) if prev_mp is not None else 0.0
            mid_price_prev[instr_id] = mid_price

            raw_feat = [imbalance, spread, bid_qty, ask_qty, mid_price_delta]
            hist = tick_history[instr_id]
            hist.append(raw_feat)
            if len(hist) > window + horizon + 5:
                hist.pop(0)

            if len(hist) >= window:
                seq = hist[-window:]
                sequences.append({"instrument_id": instr_id, "seq": seq, "mid_price": mid_price})

    by_instrument = {}
    for i, s in enumerate(sequences):
        by_instrument.setdefault(s["instrument_id"], []).append(i)

    labels = [None] * len(sequences)
    for instr_id, idxs in by_instrument.items():
        prices = [sequences[i]["mid_price"] for i in idxs]
        for pos in range(len(idxs) - horizon):
            future_price = prices[pos + horizon]
            current_price = prices[pos]
            labels[idxs[pos]] = 1 if future_price > current_price else 0

    X_list, y_list = [], []
    for s, label in zip(sequences, labels):
        if label is None:
            continue
        X_list.append(s["seq"])
        y_list.append(label)

    X = np.array(X_list, dtype=np.float32)
    y = np.array(y_list, dtype=np.int64)
    return X, y
