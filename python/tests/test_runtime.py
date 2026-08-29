import ctypes
import gc
import json
import threading
import weakref

import numpy as np
import pytest

import back_tester as bt


def row(
    sequence,
    *,
    instrument=1,
    timestamp="1970-01-01T00:00:00.000000100Z",
    action="A",
    side="B",
    price="100",
    size=10,
    order_id=None,
    flags=128,
):
    value = {
        "ts_recv": timestamp,
        "hd": {"ts_event": timestamp, "instrument_id": instrument},
        "action": action,
        "side": side,
        "price": price,
        "size": size,
        "flags": flags,
        "sequence": sequence,
    }
    if order_id is not None:
        value["order_id"] = str(order_id)
    elif action not in {"T", "R"}:
        value["order_id"] = str(sequence)
    return value


def write_rows(tmp_path, rows, name="events.jsonl"):
    path = tmp_path / name
    path.write_text(
        "".join(json.dumps(value) + "\n" for value in rows), encoding="utf-8"
    )
    return path


def metadata(*ids):
    return [bt.InstrumentMeta(instrument_id=value) for value in ids]


def empty_strategy_run(path, strategy=None, *, config=None, instruments=None):
    strategy = strategy or bt.Strategy()
    return bt.run(
        strategy,
        str(path),
        bt.DateRange(),
        config,
        instruments,
    )


def test_top_depth_trade_filter_owned_payload_and_multi_instrument(tmp_path):
    rows = []
    for level in range(16):
        rows.append(
            row(
                len(rows) + 1,
                price=str(100 - level),
                size=level + 1,
                flags=0,
                order_id=1000 + level,
            )
        )
        rows.append(
            row(
                len(rows) + 1,
                side="A",
                price=str(101 + level),
                size=level + 2,
                flags=128 if level == 15 else 0,
                order_id=2000 + level,
            )
        )
    rows.append(
        row(
            len(rows) + 1,
            timestamp="1970-01-01T00:00:00.000000200Z",
            action="T",
            side="B",
            price="101",
            size=3,
        )
    )
    rows.append(
        row(
            len(rows) + 1,
            instrument=2,
            timestamp="1970-01-01T00:00:00.000000300Z",
            side="A",
            price="50",
            size=7,
        )
    )
    path = write_rows(tmp_path, rows)

    class Capture(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.books = []
            self.trades = []
            self.gil = []

        def on_book_update(self, update):
            self.gil.append(bool(ctypes.pythonapi.PyGILState_Check()))
            self.books.append(update)
            assert (
                self.position(update.instrument_id).instrument_id
                == update.instrument_id
            )
            assert self.open_orders(update.instrument_id) == []

        def on_trade(self, trade):
            self.gil.append(bool(ctypes.pythonapi.PyGILState_Check()))
            self.trades.append(trade)

    strategy = Capture()
    empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(book_depth=15),
        instruments=metadata(1, 2),
    )
    assert [book.instrument_id for book in strategy.books] == [1, 2]
    assert len(strategy.books[0].bids) == 15
    assert len(strategy.books[0].asks) == 15
    assert [level.price for level in strategy.books[0].bids[:3]] == [
        100_000_000_000,
        99_000_000_000,
        98_000_000_000,
    ]
    assert [level.price for level in strategy.books[0].asks[:3]] == [
        101_000_000_000,
        102_000_000_000,
        103_000_000_000,
    ]
    assert len(strategy.trades) == 1
    assert strategy.trades[0].sequence == 33
    assert all(strategy.gil)
    gc.collect()
    assert strategy.books[0].bids[0].quantity == 1


def test_real_submission_delayed_fill_state_and_bulk_results(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=4, order_id=11),
        ],
    )

    class Buy(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.callbacks = []
            self.order_id = None

        def on_book_update(self, update):
            self.callbacks.append(("book", update.engine_ts_ns))
            self.order_id = self.submit_limit(
                update.instrument_id, bt.Side.BUY, 101_000_000_000, 6
            )
            assert (
                self.open_orders(update.instrument_id)[0].state
                == bt.OrderState.PENDING_NEW
            )

        def on_fill(self, fill):
            self.callbacks.append(("fill", fill.engine_ts_ns))
            assert fill.engine_ts_ns == 105
            assert fill.liquidity_source == bt.LiquiditySource.QUOTE_CROSS
            assert fill.trigger_source_sequence == 2
            assert self.position(fill.instrument_id).net_quantity == 6
            assert self.open_orders(fill.instrument_id) == []

    strategy = Buy()
    result = empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        instruments=metadata(1),
    )
    assert strategy.callbacks == [("book", 100), ("fill", 105)]
    fills = result.fills_df
    orders = result.order_log_df
    assert fills["quantity"].tolist() == [6]
    assert fills["remaining_quantity"].tolist() == [0]
    assert fills["liquidity_source"].tolist() == [1]
    assert fills["trigger_source_sequence"].tolist() == [2]
    assert orders["event_type"].tolist() == [0, 1, 2]
    assert list(fills.columns) == [
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
        "reference_price_ticks",
        "liquidity_role",
        "slippage_ticks",
        "fee_micros",
        "order_submit_ts_ns",
        "order_arrival_ts_ns",
        "time_to_fill_ns",
    ]
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
        "int64",
        "uint8",
        "uint32",
        "int64",
        "int64",
        "int64",
        "int64",
    ]
    assert result.pnl_series.index.dtype == np.dtype("int64")
    assert result.pnl_series.dtype == np.dtype("float64")
    assert result.dataset_id is None
    assert result.verified_metadata is None

    retained = fills["quantity"].to_numpy(copy=False)
    assert not retained.flags.owndata
    assert type(retained.base.base).__name__ == "PyCapsule"
    del result, fills
    gc.collect()
    assert retained.tolist() == [6]


def test_integrated_slippage_fee_risk_and_analysis(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=4, order_id=11),
        ],
    )

    class CostAndRisk(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.submitted = False
            self.snapshot = None

        def on_book_update(self, update):
            if self.submitted:
                return
            self.submitted = True
            self.submit_limit(update.instrument_id, bt.Side.BUY, 103_000_000_000, 2)
            self.submit_limit(update.instrument_id, bt.Side.BUY, 103_000_000_000, 4)
            self.snapshot = self.risk(update.instrument_id)

    strategy = CostAndRisk()
    result = empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(
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
        instruments=[
            bt.InstrumentMeta(
                instrument_id=1,
                tick_size_ticks=1_000_000_000,
                price_scale=1_000_000_000,
            )
        ],
    )

    assert strategy.snapshot.reserved_buy_quantity == 2
    assert strategy.snapshot.active_orders == 1
    assert result.fills_df.iloc[0][
        [
            "reference_price_ticks",
            "price_ticks",
            "liquidity_role",
            "slippage_ticks",
            "fee_micros",
            "time_to_fill_ns",
        ]
    ].tolist() == [101_000_000_000, 103_000_000_000, 1, 2, 500_000, 5]
    assert result.rejects_df["reason"].tolist() == [11]
    assert result.final_positions_df.iloc[0].to_dict() == {
        "instrument_id": 1.0,
        "net_quantity": 2.0,
        "realized_pnl": -0.5,
        "unrealized_pnl": -6.0,
        "total_pnl": -6.5,
    }
    report = bt.build_execution_report(result)
    assert report.rejected_orders == 1
    assert report.taker_quantity == 2
    assert report.max_drawdown == 6.5


def test_optional_run_summary_is_written_atomically_after_success(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=4, order_id=11),
        ],
    )

    class Buy(bt.Strategy):
        def on_book_update(self, update):
            self.submit_limit(update.instrument_id, bt.Side.BUY, 101_000_000_000, 1)

    summary_path = tmp_path / "diagnostics" / "run_summary.json"
    result = bt.run(
        Buy(),
        str(path),
        bt.DateRange(),
        bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        metadata(1),
        run_summary_path=str(summary_path),
    )

    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    assert summary["schema_version"] == 1
    assert summary["status"] == "success"
    assert summary["data_path"] == str(path)
    assert summary["dataset_id"] is None
    assert summary["verified_metadata"] is None
    assert summary["date_range"] == {
        "start_ts_ns": -(2**63),
        "end_ts_ns": 2**63 - 1,
    }
    assert summary["config"] == {
        "market_data_latency_ns": 0,
        "order_latency_ns": 5,
        "book_depth": 1,
        "allow_unverified_metadata": False,
        "fill_model": "fill_at_touch",
        "slippage_model": "none",
        "taker_slippage_tick_count": 0,
        "maker_fee_micros_per_contract": 0,
        "taker_fee_micros_per_contract": 0,
        "max_order_quantity": 2**63 - 1,
        "max_abs_position": 2**63 - 1,
        "max_open_quantity": 2**63 - 1,
        "max_active_orders": 2**32 - 1,
    }
    assert summary["instruments"] == [
        {
            "instrument_id": 1,
            "tick_size_ticks": 1,
            "price_scale": 1_000_000_000,
            "contract_multiplier": 1,
        }
    ]
    assert summary["counts"] == {
        "scheduled_events": 2,
        "market_deliveries": 1,
        "new_order_arrivals": 1,
        "cancel_arrivals": 0,
        "source_trade_events": 0,
        "callbacks": {"book_update": 1, "trade": 0, "fill": 1, "reject": 0},
        "fills": 1,
        "order_log_rows": 3,
        "pnl_points": 1,
    }
    assert summary["source_audit"] == {
        "input_format": "mbo_jsonl",
        "records_read": 2,
        "records_warmed": 0,
        "records_replayed": 2,
        "records_after_end": 0,
        "replayed_book_records": 2,
        "replayed_trade_records": 0,
        "first_replayed_sequence": 1,
        "last_replayed_sequence": 2,
        "replayed_sequence_digest_fnv1a64": summary["source_audit"][
            "replayed_sequence_digest_fnv1a64"
        ],
        "manifest_records": None,
        "multi_source": None,
        "checks": {
            "read_accounting": True,
            "replay_type_accounting": True,
            "trade_callbacks_match_replayed_trades": True,
            "l2_market_deliveries_match_replayed_records": None,
            "sequence_span_matches_replayed_records": True,
            "full_manifest_replay": False,
            "full_manifest_counts_match": None,
        },
    }
    assert len(summary["source_audit"]["replayed_sequence_digest_fnv1a64"]) == 16
    assert summary["duration_ns"] > 0
    assert summary["error"] is None
    assert len(result.fills_df) == 1
    assert not summary_path.with_name("run_summary.json.tmp").exists()


def test_run_summary_records_failure_without_masking_callback_error(tmp_path):
    path = write_rows(tmp_path, [row(1)])
    summary_path = tmp_path / "failed_run.json"

    class Failing(bt.Strategy):
        def on_book_update(self, update):
            raise LookupError("summary callback failure")

    with pytest.raises(LookupError, match="summary callback failure"):
        bt.run(
            Failing(),
            str(path),
            bt.DateRange(),
            None,
            metadata(1),
            run_summary_path=str(summary_path),
        )

    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    assert summary["status"] == "failed"
    assert summary["counts"]["scheduled_events"] == 1
    assert summary["counts"]["market_deliveries"] == 1
    assert summary["counts"]["callbacks"] == {
        "book_update": 1,
        "trade": 0,
        "fill": 0,
        "reject": 0,
    }
    assert summary["counts"]["fills"] is None
    assert summary["counts"]["order_log_rows"] is None
    assert summary["counts"]["pnl_points"] is None
    assert summary["source_audit"]["records_read"] == 1
    assert summary["source_audit"]["records_replayed"] == 1
    assert summary["source_audit"]["first_replayed_sequence"] == 1
    assert summary["source_audit"]["last_replayed_sequence"] == 1
    assert "summary callback failure" in summary["error"]["message"]


def test_empty_run_summary_path_is_rejected_before_replay(tmp_path):
    path = write_rows(tmp_path, [row(1)])
    with pytest.raises(ValueError, match="run_summary_path must not be empty"):
        bt.run(
            bt.Strategy(),
            str(path),
            bt.DateRange(),
            None,
            metadata(1),
            run_summary_path="",
        )


def test_three_argument_fallback_discovers_ids_and_uses_positive_latency(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=1, order_id=11),
        ],
    )

    class DefaultConfig(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.book_time = None
            self.fill_time = None

        def on_book_update(self, update):
            self.book_time = update.engine_ts_ns
            self.submit_limit(1, bt.Side.BUY, 101_000_000_000, 1)

        def on_fill(self, fill):
            self.fill_time = fill.engine_ts_ns

    strategy = DefaultConfig()
    result = bt.backtest.run(strategy, str(path), bt.DateRange())
    assert strategy.fill_time == strategy.book_time + 1
    assert result.fills_df["instrument_id"].tolist() == [1]


def test_context_reject_and_cancel_are_non_recursive(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=4, order_id=11),
        ],
    )

    class Commands(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.callback_depth = 0
            self.max_depth = 0
            self.events = []

        def on_book_update(self, update):
            self.callback_depth += 1
            self.max_depth = max(self.max_depth, self.callback_depth)
            self.events.append(("book", update.sequence))
            rejected_id = self.submit_limit(999, bt.Side.BUY, 1, 1)
            assert not self.cancel_order(999)
            assert not self.cancel_order(rejected_id)
            self.events.append(("book_done", update.sequence))
            self.callback_depth -= 1

        def on_reject(self, reject):
            self.callback_depth += 1
            self.max_depth = max(self.max_depth, self.callback_depth)
            self.events.append(("reject", reject.reason))
            self.callback_depth -= 1

    strategy = Commands()
    empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        instruments=metadata(1),
    )
    assert strategy.events == [
        ("book", 2),
        ("book_done", 2),
        ("reject", bt.RejectReason.UNKNOWN_INSTRUMENT),
        ("reject", bt.RejectReason.UNKNOWN_ORDER),
        ("reject", bt.RejectReason.ALREADY_TERMINAL),
    ]
    assert strategy.max_depth == 1


def test_cancel_after_fill_is_already_terminal_and_non_recursive(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=4, order_id=11),
        ],
    )

    class CancelFilled(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.callback_depth = 0
            self.max_depth = 0
            self.events = []
            self.order_id = None

        def enter_callback(self):
            self.callback_depth += 1
            self.max_depth = max(self.max_depth, self.callback_depth)

        def leave_callback(self):
            self.callback_depth -= 1

        def on_book_update(self, update):
            self.enter_callback()
            self.events.append(("book", update.sequence))
            self.order_id = self.submit_limit(
                update.instrument_id, bt.Side.BUY, 101_000_000_000, 6
            )
            self.leave_callback()

        def on_fill(self, fill):
            self.enter_callback()
            self.events.append(("fill", fill.client_order_id))
            assert fill.client_order_id == self.order_id
            assert fill.quantity == 6
            assert fill.remaining_quantity == 0
            assert not self.cancel_order(fill.client_order_id)
            self.events.append(("fill_done", fill.client_order_id))
            self.leave_callback()

        def on_reject(self, reject):
            self.enter_callback()
            self.events.append(("reject", reject.client_order_id, reject.reason))
            self.leave_callback()

    strategy = CancelFilled()
    result = empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        instruments=metadata(1),
    )

    assert strategy.events == [
        ("book", 2),
        ("fill", strategy.order_id),
        ("fill_done", strategy.order_id),
        (
            "reject",
            strategy.order_id,
            bt.RejectReason.ALREADY_TERMINAL,
        ),
    ]
    assert strategy.max_depth == 1
    assert result.fills_df["quantity"].tolist() == [6]
    assert result.fills_df["remaining_quantity"].tolist() == [0]


def test_same_strategy_concurrent_run_is_rejected_and_context_is_thread_local(
    tmp_path,
):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=4, order_id=11),
        ],
    )
    entered = threading.Event()
    release = threading.Event()

    class Blocking(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.callbacks = 0

        def on_book_update(self, update):
            self.callbacks += 1
            if self.callbacks == 1:
                entered.set()
                assert release.wait(timeout=5)

    strategy = Blocking()
    with pytest.raises(RuntimeError, match="only during a callback"):
        _ = strategy.now_ns

    failures = []

    def first_run():
        try:
            empty_strategy_run(path, strategy, instruments=metadata(1))
        except BaseException as error:
            failures.append(error)

    worker = threading.Thread(target=first_run)
    worker.start()
    assert entered.wait(timeout=2)
    try:
        with pytest.raises(RuntimeError, match="strategy is already running"):
            empty_strategy_run(path, strategy, instruments=metadata(1))
        with pytest.raises(RuntimeError, match="only during a callback"):
            _ = strategy.now_ns
    finally:
        release.set()
    worker.join(timeout=2)

    assert not worker.is_alive()
    assert failures == []
    with pytest.raises(RuntimeError, match="only during a callback"):
        _ = strategy.now_ns

    empty_strategy_run(path, strategy, instruments=metadata(1))
    assert strategy.callbacks == 2


def test_nested_same_strategy_run_is_rejected_without_losing_context(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=4, order_id=11),
        ],
    )

    class Nested(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.error = None
            self.time_after_reject = None

        def on_book_update(self, update):
            try:
                empty_strategy_run(path, self, instruments=metadata(1))
            except RuntimeError as error:
                self.error = str(error)
            self.time_after_reject = self.now_ns

    strategy = Nested()
    empty_strategy_run(path, strategy, instruments=metadata(1))

    assert strategy.error == "strategy is already running"
    assert strategy.time_after_reject == 100
    with pytest.raises(RuntimeError, match="only during a callback"):
        _ = strategy.now_ns


def test_delivery_callback_order_is_fills_then_trades_then_book(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="102", size=4, order_id=11),
            row(
                3,
                timestamp="1970-01-01T00:00:00.000000200Z",
                action="M",
                side="A",
                price="101",
                size=4,
                flags=0,
                order_id=11,
            ),
            row(
                4,
                timestamp="1970-01-01T00:00:00.000000200Z",
                action="T",
                side="B",
                price="101",
                size=1,
            ),
        ],
    )

    class Ordered(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.events = []
            self.fill_times = []
            self.submitted = False

        def on_book_update(self, update):
            self.events.append(("book", update.sequence))
            if not self.submitted:
                self.submitted = True
                self.submit_limit(1, bt.Side.BUY, 101_000_000_000, 1)

        def on_fill(self, fill):
            self.events.append(("fill", fill.sequence))
            self.fill_times.append((fill.exchange_ts_ns, fill.engine_ts_ns))
            assert fill.liquidity_source == bt.LiquiditySource.QUOTE_CROSS
            assert fill.trigger_source_sequence == 3

        def on_trade(self, trade):
            self.events.append(("trade", trade.sequence))

    strategy = Ordered()
    empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        instruments=metadata(1),
    )
    assert strategy.events == [
        ("book", 2),
        ("fill", 1),
        ("trade", 4),
        ("book", 4),
    ]
    assert strategy.fill_times == [(200, 200)]


def test_trade_wins_when_it_precedes_quote_cross_in_atomic_group(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="102", size=4, order_id=11),
            row(
                3,
                timestamp="1970-01-01T00:00:00.000000200Z",
                action="T",
                side="B",
                price="101",
                size=1,
                flags=0,
            ),
            row(
                4,
                timestamp="1970-01-01T00:00:00.000000200Z",
                action="M",
                side="A",
                price="101",
                size=1,
                order_id=11,
            ),
        ],
    )

    class Ordered(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.events = []

        def on_book_update(self, update):
            self.events.append(("book", update.sequence))
            if update.sequence == 2:
                self.submit_limit(1, bt.Side.BUY, 101_000_000_000, 25)

        def on_fill(self, fill):
            self.events.append(("fill", fill.liquidity_source))
            assert fill.quantity == 25
            assert fill.price == 101_000_000_000
            assert fill.trigger_source_sequence == 3

        def on_trade(self, trade):
            self.events.append(("trade", trade.sequence))

    strategy = Ordered()
    result = empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        instruments=metadata(1),
    )

    assert strategy.events == [
        ("book", 2),
        ("fill", bt.LiquiditySource.TRADE_CROSS),
        ("trade", 3),
        ("book", 4),
    ]
    assert result.fills_df["quantity"].tolist() == [25]
    assert result.fills_df["liquidity_source"].tolist() == [2]
    assert result.fills_df["trigger_source_sequence"].tolist() == [3]


def test_trade_only_empty_book_does_not_emit_initial_depth(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, action="T", side="B", price="100", size=1),
            row(
                2,
                timestamp="1970-01-01T00:00:00.000000200Z",
                side="B",
                price="99",
                size=2,
                order_id=10,
            ),
        ],
    )

    class Capture(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.events = []

        def on_trade(self, trade):
            self.events.append(("trade", trade.sequence))

        def on_book_update(self, update):
            self.events.append(("book", update.sequence))

    strategy = Capture()
    empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        instruments=metadata(1),
    )
    assert strategy.events == [("trade", 1), ("book", 2)]


def test_queue_aware_partial_fills_and_lifecycle_observability(tmp_path):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="100", size=5, flags=0, order_id=10),
            row(2, side="A", price="101", size=5, order_id=11),
            row(
                3,
                timestamp="1970-01-01T00:00:00.000000200Z",
                action="T",
                side="A",
                price="100",
                size=7,
            ),
            row(
                4,
                timestamp="1970-01-01T00:00:00.000000300Z",
                action="T",
                side="A",
                price="100",
                size=2,
            ),
        ],
    )

    class QueueStrategy(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.order_id = None
            self.fills = []

        def on_book_update(self, update):
            if self.order_id is None:
                self.order_id = self.submit_limit(
                    update.instrument_id, bt.Side.BUY, 100_000_000_000, 4
                )

        def on_fill(self, fill):
            open_orders = self.open_orders(fill.instrument_id)
            self.fills.append(
                (
                    fill.quantity,
                    fill.remaining_quantity,
                    open_orders[0].queue_ahead_quantity if open_orders else None,
                )
            )

    strategy = QueueStrategy()
    result = empty_strategy_run(
        path,
        strategy,
        config=bt.BacktestConfig(
            order_latency_ns=5,
            book_depth=1,
            fill_model=bt.FillModel.QUEUE_AWARE,
        ),
        instruments=metadata(1),
    )

    assert strategy.fills == [(2, 2, 0), (2, 0, None)]
    assert result.fills_df["quantity"].tolist() == [2, 2]
    assert result.fills_df["remaining_quantity"].tolist() == [2, 0]

    lifecycle = result.order_log_df
    assert lifecycle["transition_sequence"].tolist() == [1, 2, 3, 4]
    assert lifecycle["previous_state"].tolist() == [0, 0, 1, 2]
    assert lifecycle["state"].tolist() == [0, 1, 2, 3]
    assert lifecycle["queue_ahead_quantity"].tolist() == [0, 5, 0, 0]


@pytest.mark.parametrize("callback", ["book", "trade", "fill", "reject"])
def test_callback_exception_stops_joins_rethrows_and_second_run(tmp_path, callback):
    path = write_rows(
        tmp_path,
        [
            row(1, side="B", price="99", size=8, flags=0, order_id=10),
            row(2, side="A", price="101", size=4, order_id=11),
            row(
                3,
                timestamp="1970-01-01T00:00:00.000000200Z",
                action="T",
                side="B",
                price="100",
                size=1,
            ),
        ],
        name=f"{callback}.jsonl",
    )

    class Failing(bt.Strategy):
        def on_book_update(self, update):
            if callback == "book":
                raise LookupError("exact callback failure")
            if callback == "fill":
                self.submit_limit(1, bt.Side.BUY, 100_000_000_000, 1)
            if callback == "reject":
                self.submit_limit(999, bt.Side.BUY, 1, 1)

        def on_trade(self, trade):
            if callback == "trade":
                raise LookupError("exact callback failure")

        def on_fill(self, fill):
            if callback == "fill":
                assert fill.liquidity_source == bt.LiquiditySource.TRADE_CROSS
                assert fill.trigger_source_sequence == 3
                raise LookupError("exact callback failure")

        def on_reject(self, reject):
            if callback == "reject":
                raise LookupError("exact callback failure")

    with pytest.raises(LookupError, match="exact callback failure"):
        empty_strategy_run(
            path,
            Failing(),
            config=bt.BacktestConfig(order_latency_ns=5, book_depth=1),
            instruments=metadata(1),
        )

    clean = bt.Strategy()
    empty_strategy_run(path, clean, instruments=metadata(1))
    with pytest.raises(RuntimeError, match="only during a callback"):
        _ = clean.now_ns


def test_strategy_retained_during_run_and_collectible_after(tmp_path):
    path = write_rows(tmp_path, [row(1)])
    seen = []

    class Lifetime(bt.Strategy):
        def on_book_update(self, update):
            seen.append(update.sequence)

    strategy = Lifetime()
    reference = weakref.ref(strategy)
    empty_strategy_run(path, strategy, instruments=metadata(1))
    assert seen == [1]
    del strategy
    gc.collect()
    assert reference() is None


def test_gil_released_during_native_run(tmp_path):
    rows = [
        row(
            sequence,
            timestamp=f"1970-01-01T00:00:00.{sequence:09d}Z",
            action="T",
            side="B",
            price="100",
            size=1,
        )
        for sequence in range(1, 2001)
    ]
    path = write_rows(tmp_path, rows)
    running = True
    counter = 0

    def worker():
        nonlocal counter
        while running:
            counter += 1

    thread = threading.Thread(target=worker)
    thread.start()
    try:
        before = counter
        empty_strategy_run(path, bt.Strategy(), instruments=metadata(1))
        assert counter > before
    finally:
        running = False
        thread.join(timeout=1)
    assert not thread.is_alive()


def test_empty_results_exact_dtypes_and_deterministic_twenty_runs(tmp_path):
    path = write_rows(tmp_path, [row(1)])
    normalized = []
    for _ in range(20):
        result = empty_strategy_run(path, instruments=metadata(1))
        normalized.append(
            (
                tuple(result.fills_df.dtypes.astype(str)),
                tuple(result.order_log_df.dtypes.astype(str)),
                tuple(result.pnl_series),
            )
        )
    assert len(set(normalized)) == 1
    fills = empty_strategy_run(path, instruments=metadata(1)).fills_df
    assert fills.empty
    assert fills["client_order_id"].dtype == np.dtype("uint64")
    assert fills["side"].dtype == np.dtype("int8")


@pytest.mark.parametrize(
    "contents, message",
    [
        ("{broken\\n", "parse error"),
        (
            json.dumps(row(2))
            + "\n"
            + json.dumps(
                row(
                    1,
                    timestamp="1970-01-01T00:00:00.000000200Z",
                )
            )
            + "\n",
            "source sequence is not strictly increasing",
        ),
        (json.dumps(row(1, flags=0)) + "\n", "unterminated atomic market group"),
    ],
)
def test_input_failures_are_not_suppressed(tmp_path, contents, message):
    path = tmp_path / "bad.jsonl"
    path.write_text(contents, encoding="utf-8")
    with pytest.raises(RuntimeError, match=message):
        empty_strategy_run(path, instruments=metadata(1))
