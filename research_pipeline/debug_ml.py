import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import back_tester as bt
from strategies.ml_logistic import MLLogisticStrategy

strategy = MLLogisticStrategy([1, 2], prob_threshold=0.52, confirmation_steps=1, min_hold_updates=15, order_size=1)
result = bt.backtest.run(
    strategy, "synthetic_signal.jsonl", bt.DateRange(),
    bt.BacktestConfig(order_latency_ns=5, book_depth=1),
    [bt.InstrumentMeta(instrument_id=1, contract_multiplier=10),
     bt.InstrumentMeta(instrument_id=2, contract_multiplier=5)],
)
print(f"orders_sent={strategy.orders_sent} fills={len(result.fills_df)}")
strategy.print_debug()
