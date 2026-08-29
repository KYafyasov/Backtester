import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root, для strategies/

import back_tester as bt
import pandas as pd
import itertools

from metrics import compute_metrics
from strategies.rule_based.momentum import MomentumStrategy
from strategies.rule_based.mean_reversion import MeanReversionStrategy
from strategies.rule_based.imbalance_confirmed import ImbalanceConfirmedStrategy
from strategies.ml.ml_logistic import MLLogisticStrategy
from strategies.ml.lightgbm_strategy import LightGBMStrategy
from strategies.neural.mlp_strategy import MLPStrategy
from strategies.neural.tcn_strategy import TCNStrategy

DATA_PATH = "synthetic_signal_large.jsonl"
INSTRUMENT_IDS = [1, 2]
INSTRUMENTS = [
    bt.InstrumentMeta(instrument_id=1, contract_multiplier=10),
    bt.InstrumentMeta(instrument_id=2, contract_multiplier=5),
]
BOOK_DEPTH = 1

# сетки параметров для перебора
MOMENTUM_GRID = {"lookback": [3, 5, 8, 15]}
MEAN_REVERSION_GRID = {"lookback": [10, 20, 40], "threshold_ticks": [2, 3, 5]}
CONFIRMED_GRID = {"confirmation_steps": [2, 3, 5], "min_hold_updates": [6, 15, 30]}


def run_one(strategy):
    result = bt.backtest.run(
        strategy, DATA_PATH, bt.DateRange(),
        bt.BacktestConfig(order_latency_ns=5, book_depth=BOOK_DEPTH),
        INSTRUMENTS,
    )
    metrics = compute_metrics(result.pnl_series, result.fills_df, strategy.trade_pnls)
    metrics["orders_sent"] = strategy.orders_sent
    return metrics


def grid_momentum():
    rows = []
    for lookback in MOMENTUM_GRID["lookback"]:
        strategy = MomentumStrategy(INSTRUMENT_IDS, lookback=lookback, order_size=1)
        m = run_one(strategy)
        m["params"] = f"lookback={lookback}"
        rows.append(m)
    return pd.DataFrame(rows)


def grid_mean_reversion():
    rows = []
    for lookback, thr in itertools.product(MEAN_REVERSION_GRID["lookback"], MEAN_REVERSION_GRID["threshold_ticks"]):
        strategy = MeanReversionStrategy(INSTRUMENT_IDS, lookback=lookback, threshold_ticks=thr, order_size=1)
        m = run_one(strategy)
        m["params"] = f"lookback={lookback},thr={thr}"
        rows.append(m)
    return pd.DataFrame(rows)


def grid_imbalance_confirmed():
    rows = []
    for conf, hold in itertools.product(CONFIRMED_GRID["confirmation_steps"], CONFIRMED_GRID["min_hold_updates"]):
        strategy = ImbalanceConfirmedStrategy(INSTRUMENT_IDS, threshold=0.3, confirmation_steps=conf,
                                                min_hold_updates=hold, order_size=1)
        m = run_one(strategy)
        m["params"] = f"conf={conf},hold={hold}"
        rows.append(m)
    return pd.DataFrame(rows)


def grid_ml_logistic():
    rows = []
    for conf, hold in itertools.product(CONFIRMED_GRID["confirmation_steps"], CONFIRMED_GRID["min_hold_updates"]):
        strategy = MLLogisticStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=conf,
                                        min_hold_updates=hold, order_size=1)
        m = run_one(strategy)
        m["params"] = f"conf={conf},hold={hold}"
        rows.append(m)
    return pd.DataFrame(rows)


def grid_lightgbm():
    rows = []
    for conf, hold in itertools.product(CONFIRMED_GRID["confirmation_steps"], CONFIRMED_GRID["min_hold_updates"]):
        strategy = LightGBMStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=conf,
                                      min_hold_updates=hold, order_size=1)
        m = run_one(strategy)
        m["params"] = f"conf={conf},hold={hold}"
        rows.append(m)
    return pd.DataFrame(rows)


def grid_mlp():
    rows = []
    for conf, hold in itertools.product(CONFIRMED_GRID["confirmation_steps"], CONFIRMED_GRID["min_hold_updates"]):
        strategy = MLPStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=conf,
                                 min_hold_updates=hold, order_size=1)
        m = run_one(strategy)
        m["params"] = f"conf={conf},hold={hold}"
        rows.append(m)
    return pd.DataFrame(rows)


TCN_GRID = {"confirmation_steps": [5, 10], "min_hold_updates": [30, 80]}


def grid_tcn():
    rows = []
    for conf, hold in itertools.product(TCN_GRID["confirmation_steps"], TCN_GRID["min_hold_updates"]):
        strategy = TCNStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=conf,
                                 min_hold_updates=hold, order_size=1)
        m = run_one(strategy)
        m["params"] = f"conf={conf},hold={hold}"
        rows.append(m)
    return pd.DataFrame(rows)


def show(name, df):
    df = df.set_index("params")
    cols = ["final_pnl", "sharpe", "profit_factor", "win_rate", "orders_sent"]
    pd.set_option("display.width", 200)
    print(f"\n=== {name} ===")
    print(df[cols].sort_values("final_pnl", ascending=False).to_string(float_format=lambda x: f"{x:.4f}"))


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "all"

    if target in ("momentum", "all"):
        show("momentum", grid_momentum())
    if target in ("mean_reversion", "all"):
        show("mean_reversion", grid_mean_reversion())
    if target in ("imbalance_confirmed", "all"):
        show("imbalance_confirmed", grid_imbalance_confirmed())
    if target in ("ml_logistic", "all"):
        show("ml_logistic", grid_ml_logistic())
    if target in ("lightgbm", "all"):
        show("lightgbm", grid_lightgbm())
    if target in ("mlp", "all"):
        show("mlp", grid_mlp())
    if target in ("tcn", "all"):
        show("tcn", grid_tcn())


if __name__ == "__main__":
    main()
