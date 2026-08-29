from types import SimpleNamespace

import pandas as pd
import pytest

from back_tester.analysis import build_execution_report, compare_results


def sample_result(fee_micros=300):
    fills = pd.DataFrame(
        {
            "quantity": [2, 3],
            "remaining_quantity": [3, 0],
            "liquidity_role": [0, 1],
            "fee_micros": [-100, fee_micros],
            "slippage_ticks": [0, 2],
            "time_to_fill_ns": [10, 30],
        }
    )
    orders = pd.DataFrame(
        {
            "event_type": [0, 1, 2, 2, 0, 5, 4],
            "order_quantity": [5, 5, 5, 5, 2, 2, 5],
            "queue_ahead_quantity": [0, 4, 0, 0, 0, 0, 0],
        }
    )
    return SimpleNamespace(
        fills_df=fills,
        order_log_df=orders,
        rejects_df=pd.DataFrame({"reason": [11]}),
        pnl_series=pd.Series([0.0, 2.0, 1.25]),
    )


def test_execution_report_covers_cost_risk_latency_and_pnl():
    report = build_execution_report(sample_result())
    assert report.submitted_orders == 2
    assert report.rejected_orders == 1
    assert report.fill_events == 2
    assert report.partial_fill_events == 1
    assert report.fill_ratio == pytest.approx(5 / 7)
    assert report.maker_quantity == 2
    assert report.taker_quantity == 3
    assert report.total_fees_micros == 200
    assert report.slippage_tick_volume == 6
    assert report.median_time_to_fill_ns == 20
    assert report.p95_time_to_fill_ns == pytest.approx(29)
    assert report.average_queue_ahead_at_accept == 4
    assert report.final_net_pnl == 1.25
    assert report.max_drawdown == 0.75


def test_named_results_compare_as_rows():
    compared = compare_results(
        {"baseline": sample_result(0), "strict": sample_result()}
    )
    assert compared.index.tolist() == ["baseline", "strict"]
    assert compared.loc["baseline", "total_fees_micros"] == -100
    assert compared.loc["strict", "total_fees_micros"] == 200
