import sys, os, json
import numpy as np

DATA_PATH = "synthetic_signal_subset.jsonl"
LOOKBACK = 20

state = {}
mp_hist = {}
deviations = []

with open(DATA_PATH) as f:
    for line in f:
        event = json.loads(line)
        instr_id = event["hd"]["instrument_id"]
        action = event["action"]
        if instr_id not in state:
            state[instr_id] = {"best_bid": None, "best_ask": None}
            mp_hist[instr_id] = []
        s = state[instr_id]
        if action in ("A", "M"):
            price = int(event["price"])
            side = event["side"]
            if side == "B":
                s["best_bid"] = price
            elif side == "A":
                s["best_ask"] = price
        else:
            continue
        if s["best_bid"] is None or s["best_ask"] is None:
            continue
        mid = (s["best_bid"] + s["best_ask"]) / 2
        hist = mp_hist[instr_id]
        hist.append(mid)
        if len(hist) > LOOKBACK:
            hist.pop(0)
        if len(hist) == LOOKBACK:
            avg = sum(hist) / len(hist)
            deviations.append(mid - avg)

deviations = np.array(deviations)
print("deviation stats (lookback=20):")
print("  min:", deviations.min())
print("  max:", deviations.max())
print("  mean:", deviations.mean())
print("  std:", deviations.std())
for q in [0.01, 0.05, 0.25, 0.5, 0.75, 0.95, 0.99]:
    print(f"  quantile {q}: {np.quantile(deviations, q):.4f}")
for thr in [2, 3, 5, 10, 20]:
    frac = (np.abs(deviations) > thr).mean()
    print(f"  |deviation| > {thr}: {frac:.4f}")
