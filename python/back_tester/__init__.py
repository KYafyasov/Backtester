"""Deterministic options back-testing API backed by the native runtime."""

from ._backtester import (
    BacktestConfig,
    BookLevel,
    BookUpdate,
    DateRange,
    Fill,
    FillModel,
    InstrumentMeta,
    LiquiditySource,
    LiquidityRole,
    OpenOrder,
    OrderState,
    Position,
    Reject,
    RejectReason,
    Result,
    RiskSnapshot,
    Side,
    SlippageModel,
    Strategy,
    Trade,
    run,
    version,
)
from .analysis import ExecutionReport, build_execution_report, compare_results


class _Backtest:
    """Namespace preserving the documented ``backtest.run(...)`` entry point."""

    run = staticmethod(run)


backtest = _Backtest()

__all__ = [
    "BacktestConfig",
    "BookLevel",
    "BookUpdate",
    "DateRange",
    "Fill",
    "FillModel",
    "ExecutionReport",
    "InstrumentMeta",
    "LiquiditySource",
    "LiquidityRole",
    "OpenOrder",
    "OrderState",
    "Position",
    "Reject",
    "RejectReason",
    "Result",
    "RiskSnapshot",
    "Side",
    "SlippageModel",
    "Strategy",
    "Trade",
    "backtest",
    "build_execution_report",
    "compare_results",
    "run",
    "version",
]
