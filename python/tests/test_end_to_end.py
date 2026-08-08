import importlib.util
from pathlib import Path

import numpy as np
import pytest

EXPECTED_CALLBACKS = [
    ("book", 1, 2, 100),
    ("book", 2, 4, 110),
    ("book", 2, 5, 150),
    ("fill", 1, 1, 200),
    ("trade", 1, 7, 200),
    ("book", 1, 7, 200),
]
FILL_COLUMNS = [
    "exchange_ts_ns",
    "engine_ts_ns",
    "instrument_id",
    "client_order_id",
    "side",
    "price_ticks",
    "quantity",
    "remaining_quantity",
    "liquidity_source",
    "trigger_source_sequence",
    "trigger_source_id",
    "trigger_global_market_sequence",
]
ORDER_COLUMNS = [
    "engine_ts_ns",
    "instrument_id",
    "client_order_id",
    "event_type",
    "state",
    "side",
    "limit_price_ticks",
    "order_quantity",
    "filled_quantity",
    "remaining_quantity",
    "reject_reason",
]
DATA_PATH = Path(__file__).resolve().parents[2] / "test/data/m5_two_instrument.jsonl"
EXAMPLE_PATH = Path(__file__).resolve().parents[2] / "examples/mean_reversion.py"
EXAMPLE_SPEC = importlib.util.spec_from_file_location(
    "mean_reversion_example", EXAMPLE_PATH
)
assert EXAMPLE_SPEC is not None and EXAMPLE_SPEC.loader is not None
EXAMPLE_MODULE = importlib.util.module_from_spec(EXAMPLE_SPEC)
EXAMPLE_SPEC.loader.exec_module(EXAMPLE_MODULE)
MeanReversionStrategy = EXAMPLE_MODULE.MeanReversionStrategy
run_example = EXAMPLE_MODULE.run_example


def normalize(strategy, result):
    fills = tuple(
        tuple(int(value) for value in row)
        for row in result.fills_df.itertuples(index=False, name=None)
    )
    orders = tuple(
        tuple(int(value) for value in row)
        for row in result.order_log_df.itertuples(index=False, name=None)
    )
    pnl = tuple(
        (int(timestamp), float(value)) for timestamp, value in result.pnl_series.items()
    )
    return tuple(strategy.callbacks), strategy.final_positions, fills, orders, pnl


def test_two_instrument_runtime_contract_and_results():
    strategy, result = run_example()
    fills = result.fills_df
    orders = result.order_log_df

    assert strategy.callbacks == EXPECTED_CALLBACKS
    assert strategy.final_positions == {1: 2, 2: 0}

    assert list(fills.columns) == FILL_COLUMNS
    assert fills.dtypes.astype(str).tolist() == [
        "int64",
        "int64",
        "int64",
        "uint64",
        "int8",
        "int64",
        "int64",
        "int64",
        "uint8",
        "uint64",
        "uint32",
        "uint64",
    ]
    assert fills.iloc[0].to_dict() == {
        "exchange_ts_ns": 200,
        "engine_ts_ns": 200,
        "instrument_id": 1,
        "client_order_id": 1,
        "side": 1,
        "price_ticks": 101_000_000_000,
        "quantity": 2,
        "remaining_quantity": 0,
        "liquidity_source": 1,
        "trigger_source_sequence": 6,
        "trigger_source_id": 0,
        "trigger_global_market_sequence": 0,
    }

    assert list(orders.columns) == ORDER_COLUMNS
    assert orders.dtypes.astype(str).tolist() == [
        "int64",
        "int64",
        "uint64",
        "uint8",
        "uint8",
        "int8",
        "int64",
        "int64",
        "int64",
        "int64",
        "uint8",
    ]
    first = orders[orders["client_order_id"] == 1]
    assert first["engine_ts_ns"].tolist() == [100, 105, 200]
    assert first["event_type"].tolist() == [0, 1, 2]
    assert first["state"].tolist() == [0, 1, 3]
    assert first["remaining_quantity"].tolist() == [2, 2, 0]
    second = orders[orders["client_order_id"] == 2]
    assert second["engine_ts_ns"].tolist() == [110, 115, 150, 155]
    assert second["event_type"].tolist() == [0, 1, 3, 4]
    assert second["state"].tolist() == [0, 1, 4, 5]
    assert second["remaining_quantity"].tolist() == [1, 1, 1, 1]

    assert result.pnl_series.index.dtype == np.dtype("int64")
    assert result.pnl_series.dtype == np.dtype("float64")
    assert result.pnl_series.iloc[-1] == pytest.approx(-20.0)


def test_twenty_runs_are_identical():
    expected = normalize(*run_example())
    for _ in range(19):
        assert normalize(*run_example()) == expected


def test_callback_exception_does_not_poison_next_run():
    class RaisesOnTrade(MeanReversionStrategy):
        def on_trade(self, trade):
            super().on_trade(trade)
            raise RuntimeError("intentional M5 callback failure")

    with pytest.raises(RuntimeError, match="intentional M5 callback failure"):
        import back_tester as bt

        bt.backtest.run(
            RaisesOnTrade(),
            str(DATA_PATH),
            bt.DateRange(),
            bt.BacktestConfig(order_latency_ns=5, book_depth=1),
            [
                bt.InstrumentMeta(instrument_id=1, contract_multiplier=10),
                bt.InstrumentMeta(instrument_id=2, contract_multiplier=5),
            ],
        )

    strategy, result = run_example()
    assert strategy.callbacks == EXPECTED_CALLBACKS
    assert result.fills_df["quantity"].tolist() == [2]
