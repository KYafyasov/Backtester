"""Minimal deterministic two-instrument mean-reversion backtest."""

from pathlib import Path

import back_tester as bt


DATA_PATH = Path(__file__).resolve().parents[1] / "test/data/m5_two_instrument.jsonl"


class MeanReversionStrategy(bt.Strategy):
    """Rest inside a wide spread; cancel the independent probe instrument."""

    def __init__(self):
        super().__init__()
        self.callbacks = []
        self.mean_reversion_order_id = None
        self.cancel_probe_order_id = None
        self.final_positions = None

    def on_book_update(self, update):
        self.callbacks.append(
            ("book", update.instrument_id, update.sequence, update.engine_ts_ns)
        )
        if update.instrument_id == 1 and update.sequence == 2:
            self.mean_reversion_order_id = self.submit_limit(
                1, bt.Side.BUY, 101_000_000_000, 2
            )
        elif update.instrument_id == 2 and update.sequence == 4:
            self.cancel_probe_order_id = self.submit_limit(
                2, bt.Side.BUY, 49_000_000_000, 1
            )
        elif update.instrument_id == 2 and update.sequence == 5:
            assert self.cancel_order(self.cancel_probe_order_id)
        elif update.instrument_id == 1 and update.sequence == 7:
            self.final_positions = {
                instrument_id: self.position(instrument_id).net_quantity
                for instrument_id in (1, 2)
            }

    def on_trade(self, trade):
        self.callbacks.append(
            ("trade", trade.instrument_id, trade.sequence, trade.engine_ts_ns)
        )

    def on_fill(self, fill):
        self.callbacks.append(
            ("fill", fill.instrument_id, fill.sequence, fill.engine_ts_ns)
        )


def run_example(data_path=DATA_PATH):
    strategy = MeanReversionStrategy()
    result = bt.backtest.run(
        strategy,
        str(data_path),
        bt.DateRange(),
        bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        [
            bt.InstrumentMeta(instrument_id=1, contract_multiplier=10),
            bt.InstrumentMeta(instrument_id=2, contract_multiplier=5),
        ],
    )
    return strategy, result


def main():
    strategy, result = run_example()
    terminal_states = (
        result.order_log_df.groupby("client_order_id", sort=True)["state"]
        .last()
        .to_dict()
    )
    print(f"callbacks={strategy.callbacks}")
    print(f"final_positions={strategy.final_positions}")
    print(f"terminal_states={terminal_states}")
    print(f"fills={len(result.fills_df)}")
    print(f"final_pnl={result.pnl_series.iloc[-1]:.1f}")


if __name__ == "__main__":
    main()
