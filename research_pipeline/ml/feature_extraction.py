"""Реплеит JSONL напрямую (без C++ движка) и считает расширенный набор фичей+метку для обучения ML."""
import json


def replay_and_extract_features(path, horizon=20, imbalance_windows=(5, 20),
                                 momentum_windows=(5, 20), volatility_window=20,
                                 trade_freq_window=20):
    """
    Фичи: imbalance, spread, imbalance_ma_5, imbalance_ma_20,
          momentum_5, momentum_20, volatility_20, bid_qty, ask_qty,
          trade_freq_20 (доля "book update" моментов из последних N, после которых была сделка).
    Строка (наблюдение) создаётся только на A/M событиях — так же, как on_book_update в движке,
    T-события не создают отдельную строку, а лишь увеличивают счётчик trades_since_last_row.
    """
    state = {}
    imbalance_history = {}
    mid_price_history = {}
    trade_flag_history = {}   # per-instrument deque из 0/1: была ли сделка с прошлой строки
    trades_since_last_row = {}

    rows = []

    with open(path) as f:
        for line in f:
            event = json.loads(line)
            instr_id = event["hd"]["instrument_id"]
            action = event["action"]

            if instr_id not in state:
                state[instr_id] = {"best_bid": None, "best_ask": None, "bid_qty": 0, "ask_qty": 0}
                imbalance_history[instr_id] = []
                mid_price_history[instr_id] = []
                trade_flag_history[instr_id] = []
                trades_since_last_row[instr_id] = 0

            s = state[instr_id]

            if action == "T":
                trades_since_last_row[instr_id] += 1
                continue  # T не создаёт строку, только считается — как on_trade в движке

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
                continue  # C/F/R пока не обрабатываем как отдельную строку

            if s["best_bid"] is None or s["best_ask"] is None:
                continue

            bid_qty, ask_qty = s["bid_qty"], s["ask_qty"]
            total = bid_qty + ask_qty
            if total == 0:
                continue

            imbalance = (bid_qty - ask_qty) / total
            spread = s["best_ask"] - s["best_bid"]
            mid_price = (s["best_bid"] + s["best_ask"]) / 2

            hist = imbalance_history[instr_id]
            hist.append(imbalance)
            if len(hist) > max(imbalance_windows) + 1:
                hist.pop(0)

            mp_hist = mid_price_history[instr_id]
            mp_hist.append(mid_price)
            if len(mp_hist) > max(max(momentum_windows), volatility_window) + 1:
                mp_hist.pop(0)

            tf_hist = trade_flag_history[instr_id]
            tf_hist.append(1 if trades_since_last_row[instr_id] > 0 else 0)
            if len(tf_hist) > trade_freq_window:
                tf_hist.pop(0)
            trades_since_last_row[instr_id] = 0

            feats = {"instrument_id": instr_id, "imbalance": imbalance,
                     "spread": spread, "mid_price": mid_price,
                     "bid_qty": bid_qty, "ask_qty": ask_qty}
            for w in imbalance_windows:
                window_vals = hist[-w:] if len(hist) >= w else hist
                feats[f"imbalance_ma_{w}"] = sum(window_vals) / len(window_vals) if window_vals else 0.0

            for mw in momentum_windows:
                if len(mp_hist) > mw:
                    feats[f"momentum_{mw}"] = mp_hist[-1] - mp_hist[-mw - 1]
                else:
                    feats[f"momentum_{mw}"] = 0.0

            if len(mp_hist) >= 2:
                window_vals = mp_hist[-volatility_window:] if len(mp_hist) >= volatility_window else mp_hist
                mean_v = sum(window_vals) / len(window_vals)
                var_v = sum((v - mean_v) ** 2 for v in window_vals) / len(window_vals)
                feats[f"volatility_{volatility_window}"] = var_v ** 0.5
            else:
                feats[f"volatility_{volatility_window}"] = 0.0

            feats[f"trade_freq_{trade_freq_window}"] = sum(tf_hist) / len(tf_hist) if tf_hist else 0.0

            rows.append(feats)

    by_instrument_indices = {}
    for i, r in enumerate(rows):
        by_instrument_indices.setdefault(r["instrument_id"], []).append(i)

    labels = [None] * len(rows)
    for instr_id, idxs in by_instrument_indices.items():
        prices = [rows[i]["mid_price"] for i in idxs]
        for pos in range(len(idxs) - horizon):
            future_price = prices[pos + horizon]
            current_price = prices[pos]
            labels[idxs[pos]] = 1 if future_price > current_price else 0

    dataset = []
    for r, label in zip(rows, labels):
        if label is None:
            continue
        r["label"] = label
        dataset.append(r)

    return dataset
