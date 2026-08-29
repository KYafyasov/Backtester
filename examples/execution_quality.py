"""Demonstrate integrated slippage, fees, risk, and execution reporting."""

from pathlib import Path

import back_tester as bt


DATA_PATH = Path(__file__).resolve().parents[1] / "test/data/queue_aware_demo.jsonl"


class ExecutionQualityStrategy(bt.Strategy):
    """Submit one marketable limit and one intentionally oversized order."""

    def __init__(self):
        super().__init__()
        self.submitted = False
        self.risk_after_submit = None

    def on_book_update(self, update):
        if self.submitted:
            return
        self.submitted = True
        self.submit_limit(update.instrument_id, bt.Side.BUY, 103_000_000_000, 2)
        self.submit_limit(update.instrument_id, bt.Side.BUY, 103_000_000_000, 4)
        self.risk_after_submit = self.risk(update.instrument_id)


def instrument_metadata():
    return [
        bt.InstrumentMeta(
            instrument_id=1,
            tick_size_ticks=1_000_000_000,
            price_scale=1_000_000_000,
        )
    ]


def run_with_config(config, data_path=DATA_PATH):
    strategy = ExecutionQualityStrategy()
    result = bt.run(
        strategy,
        str(data_path),
        bt.DateRange(),
        config,
        instrument_metadata(),
    )
    return strategy, result


def run_example(data_path=DATA_PATH):
    return run_with_config(
        bt.BacktestConfig(
            order_latency_ns=5,
            book_depth=1,
            slippage_model=bt.SlippageModel.FIXED_TICKS,
            taker_slippage_tick_count=2,
            taker_fee_micros_per_contract=250_000,
            max_order_quantity=3,
            max_abs_position=5,
            max_open_quantity=5,
            max_active_orders=2,
        ),
        data_path,
    )


def run_baseline(data_path=DATA_PATH):
    return run_with_config(
        bt.BacktestConfig(order_latency_ns=5, book_depth=1), data_path
    )


def main():
    strategy, result = run_example()
    _, baseline = run_baseline()
    print("Risk after submit:")
    print(
        {
            "reserved_buy_quantity": strategy.risk_after_submit.reserved_buy_quantity,
            "active_orders": strategy.risk_after_submit.active_orders,
            "worst_case_long": strategy.risk_after_submit.worst_case_long,
        }
    )
    print("\nExecution report:")
    print(bt.build_execution_report(result).to_frame().to_string(index=False))
    print("\nBaseline vs realistic model:")
    comparison = bt.compare_results({"optimistic": baseline, "realistic": result})
    print(
        comparison[
            [
                "accepted_orders",
                "rejected_orders",
                "filled_quantity",
                "total_fees_micros",
                "slippage_tick_volume",
                "final_net_pnl",
            ]
        ].to_string()
    )
    print("\nFills:")
    print(
        result.fills_df[
            [
                "reference_price_ticks",
                "price_ticks",
                "liquidity_role",
                "slippage_ticks",
                "fee_micros",
                "time_to_fill_ns",
            ]
        ].to_string(index=False)
    )
    print("\nRejects:")
    print(result.rejects_df.to_string(index=False))
    print("\nFinal positions:")
    print(result.final_positions_df.to_string(index=False))


if __name__ == "__main__":
    main()
