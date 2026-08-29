import pytest

from back_tester import Strategy
from back_tester._backtester import _benchmark_book_callbacks


def test_internal_benchmark_helpers_validate_and_execute():
    assert _benchmark_book_callbacks(Strategy(), 1, 1_000) >= 0
    assert _benchmark_book_callbacks(Strategy(), 15, 1_000) >= 0

    with pytest.raises(ValueError, match="depth and iterations must be positive"):
        _benchmark_book_callbacks(Strategy(), 0, 1_000)
