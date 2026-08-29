"""Demonstrate deterministic queue-aware partial fills and lifecycle logs."""

from pathlib import Path

import back_tester as bt


DATA_PATH = Path(__file__).resolve().parents[1] / "test/data/queue_aware_demo.jsonl"


class QueueAwareStrategy(bt.Strategy):
    """Join the best bid behind five displayed contracts."""

    def __init__(self):
        super().__init__()
        self.order_id = None
        self.fill_events = []

    def on_book_update(self, update):
        if self.order_id is None:
            self.order_id = self.submit_limit(
                update.instrument_id,
                bt.Side.BUY,
                update.bids[0].price,
                4,
            )

    def on_fill(self, fill):
        self.fill_events.append(
            {
                "engine_ts_ns": fill.engine_ts_ns,
                "quantity": fill.quantity,
                "remaining_quantity": fill.remaining_quantity,
            }
        )


def run_example(data_path=DATA_PATH):
    strategy = QueueAwareStrategy()
    result = bt.backtest.run(
        strategy,
        str(data_path),
        bt.DateRange(),
        bt.BacktestConfig(
            order_latency_ns=5,
            book_depth=1,
            fill_model=bt.FillModel.QUEUE_AWARE,
        ),
        [bt.InstrumentMeta(instrument_id=1)],
    )
    return strategy, result


def main():
    strategy, result = run_example()
    columns = [
        "transition_sequence",
        "event_type",
        "previous_state",
        "state",
        "filled_quantity",
        "remaining_quantity",
        "queue_ahead_quantity",
    ]
    print(f"fill_events={strategy.fill_events}")
    print(result.order_log_df[columns].to_string(index=False))


if __name__ == "__main__":
    main()
