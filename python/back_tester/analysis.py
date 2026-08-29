"""Deterministic post-trade diagnostics built from a frozen backtest result."""

from dataclasses import asdict, dataclass
from typing import Mapping

import pandas as pd


@dataclass(frozen=True)
class ExecutionReport:
    """Compact execution, risk, cost, latency, and PnL summary."""

    submitted_orders: int
    accepted_orders: int
    rejected_orders: int
    cancelled_orders: int
    fill_events: int
    partial_fill_events: int
    submitted_quantity: int
    filled_quantity: int
    fill_ratio: float
    maker_quantity: int
    taker_quantity: int
    total_fees_micros: int
    slippage_tick_volume: int
    median_time_to_fill_ns: float
    p95_time_to_fill_ns: float
    average_queue_ahead_at_accept: float
    final_net_pnl: float
    max_drawdown: float

    def as_dict(self) -> dict[str, int | float]:
        """Return a stable mapping suitable for JSON or tabular output."""

        return asdict(self)

    def to_frame(self) -> pd.DataFrame:
        """Return a two-column presentation-friendly table."""

        return pd.DataFrame(
            {"metric": list(self.as_dict()), "value": list(self.as_dict().values())}
        )


def build_execution_report(result) -> ExecutionReport:
    """Compute execution-quality metrics without rerunning the simulation."""

    fills = result.fills_df
    orders = result.order_log_df
    pnl = result.pnl_series

    submitted = orders[orders["event_type"] == 0]
    accepted = orders[orders["event_type"] == 1]
    submitted_quantity = int(submitted["order_quantity"].sum())
    filled_quantity = int(fills["quantity"].sum())
    fill_ratio = (
        float(filled_quantity / submitted_quantity) if submitted_quantity else 0.0
    )
    time_to_fill = fills["time_to_fill_ns"]
    drawdown = pnl.cummax().clip(lower=0.0) - pnl

    return ExecutionReport(
        submitted_orders=len(submitted),
        accepted_orders=len(accepted),
        rejected_orders=int((orders["event_type"] == 5).sum()),
        cancelled_orders=int((orders["event_type"] == 4).sum()),
        fill_events=len(fills),
        partial_fill_events=int((fills["remaining_quantity"] > 0).sum()),
        submitted_quantity=submitted_quantity,
        filled_quantity=filled_quantity,
        fill_ratio=fill_ratio,
        maker_quantity=int(fills.loc[fills["liquidity_role"] == 0, "quantity"].sum()),
        taker_quantity=int(fills.loc[fills["liquidity_role"] == 1, "quantity"].sum()),
        total_fees_micros=int(fills["fee_micros"].sum()),
        slippage_tick_volume=int((fills["slippage_ticks"] * fills["quantity"]).sum()),
        median_time_to_fill_ns=(
            float(time_to_fill.median()) if not time_to_fill.empty else 0.0
        ),
        p95_time_to_fill_ns=(
            float(time_to_fill.quantile(0.95)) if not time_to_fill.empty else 0.0
        ),
        average_queue_ahead_at_accept=(
            float(accepted["queue_ahead_quantity"].mean())
            if not accepted.empty
            else 0.0
        ),
        final_net_pnl=float(pnl.iloc[-1]) if not pnl.empty else 0.0,
        max_drawdown=float(drawdown.max()) if not drawdown.empty else 0.0,
    )


def compare_results(results: Mapping[str, object]) -> pd.DataFrame:
    """Compare named runs as one row per deterministic configuration."""

    rows = []
    for name, result in results.items():
        rows.append({"run": name, **build_execution_report(result).as_dict()})
    return pd.DataFrame(rows).set_index("run")
