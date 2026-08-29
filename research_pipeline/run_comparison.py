import argparse
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root, для strategies/

import back_tester as bt
import pandas as pd

from metrics import compute_metrics
from strategies.rule_based.imbalance import ImbalanceStrategy
from strategies.rule_based.momentum import MomentumStrategy
from strategies.rule_based.mean_reversion import MeanReversionStrategy
from strategies.rule_based.imbalance_confirmed import ImbalanceConfirmedStrategy
from strategies.ml.ml_logistic import MLLogisticStrategy
from strategies.ml.lightgbm_strategy import LightGBMStrategy
from strategies.neural.mlp_strategy import MLPStrategy
from strategies.neural.tcn_strategy import TCNStrategy
from strategies.meta.ensemble_strategy import VotingEnsembleStrategy
from strategies.rl.dqn_strategy import DQNStrategy
from strategies.neural.lstm_strategy import LSTMStrategy

INSTRUMENT_IDS = [1, 2]
INSTRUMENTS = [
    bt.InstrumentMeta(instrument_id=1, contract_multiplier=10),
    bt.InstrumentMeta(instrument_id=2, contract_multiplier=5),
]

STRATEGY_FACTORIES = {
    "imbalance": lambda: ImbalanceStrategy(INSTRUMENT_IDS, threshold=0.3, order_size=1),
    "momentum": lambda: MomentumStrategy(INSTRUMENT_IDS, lookback=5, order_size=1),
    "mean_reversion": lambda: MeanReversionStrategy(INSTRUMENT_IDS, lookback=10, threshold_ticks=2, order_size=1),
    "imbalance_confirmed": lambda: ImbalanceConfirmedStrategy(INSTRUMENT_IDS, threshold=0.3, confirmation_steps=3, min_hold_updates=15, order_size=1),
    "ml_logistic": lambda: MLLogisticStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=3, min_hold_updates=15, order_size=1),
    "lightgbm": lambda: LightGBMStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=3, min_hold_updates=15, order_size=1),
    "mlp": lambda: MLPStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=3, min_hold_updates=15, order_size=1),
    "tcn": lambda: TCNStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=3, min_hold_updates=15, order_size=1),
    "ensemble_voting": lambda: VotingEnsembleStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=3, min_hold_updates=15, order_size=1),
    "dqn": lambda: DQNStrategy(INSTRUMENT_IDS, order_size=1),
    "lstm": lambda: LSTMStrategy(INSTRUMENT_IDS, prob_threshold=0.52, confirmation_steps=3, min_hold_updates=15, order_size=1),
}

BOOK_DEPTH = 1


def run_one(name, factory, data_path):
    strategy = factory()
    result = bt.backtest.run(
        strategy,
        data_path,
        bt.DateRange(),
        bt.BacktestConfig(order_latency_ns=5, book_depth=BOOK_DEPTH),
        INSTRUMENTS,
    )
    metrics = compute_metrics(result.pnl_series, result.fills_df, strategy.trade_pnls)
    metrics["strategy"] = name
    metrics["orders_sent"] = strategy.orders_sent
    metrics["book_updates"] = strategy.book_updates
    return metrics


def main():
    parser = argparse.ArgumentParser(description="Сравнение стратегий на synthetic-данных через C++ движок")
    parser.add_argument(
        "--strategies", "-s",
        type=str,
        default="all",
        help=f"Через запятую: {','.join(STRATEGY_FACTORIES.keys())}. По умолчанию all — все сразу.",
    )
    parser.add_argument(
        "--data", "-d",
        type=str,
        default="synthetic_signal.jsonl",
        help="Путь к JSONL-файлу с данными (по умолчанию synthetic_signal.jsonl)",
    )
    parser.add_argument(
        "--list", action="store_true",
        help="Показать список доступных стратегий и выйти",
    )
    args = parser.parse_args()

    if args.list:
        print("Доступные стратегии:")
        for name in STRATEGY_FACTORIES:
            print(f"  - {name}")
        return

    if args.strategies == "all":
        selected = list(STRATEGY_FACTORIES.keys())
    else:
        selected = [s.strip() for s in args.strategies.split(",")]
        unknown = [s for s in selected if s not in STRATEGY_FACTORIES]
        if unknown:
            print(f"Неизвестные стратегии: {unknown}")
            print(f"Доступные: {list(STRATEGY_FACTORIES.keys())}")
            sys.exit(1)

    print(f"Данные: {args.data}")
    print(f"Стратегии: {selected}\n")

    rows = []
    for name in selected:
        print(f"--- Running {name} ---")
        rows.append(run_one(name, STRATEGY_FACTORIES[name], args.data))

    df = pd.DataFrame(rows).set_index("strategy")
    cols = [
        "final_pnl", "max_drawdown", "sharpe", "win_rate",
        "num_fills", "total_volume", "profit_factor",
        "avg_pnl_per_trade", "num_closing_trades",
        "orders_sent", "book_updates",
    ]
    df = df[cols]
    pd.set_option("display.width", 200)
    pd.set_option("display.max_columns", 20)
    print(f"\n=== Comparison (book_depth={BOOK_DEPTH}) ===")
    print(df.to_string(float_format=lambda x: f"{x:.4f}"))


if __name__ == "__main__":
    main()
