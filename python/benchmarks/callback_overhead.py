"""Measure 1,000 no-op callbacks inside the pybind callback window."""

import platform
import statistics

from back_tester import Strategy
from back_tester._backtester import _benchmark_book_callbacks


CALLBACKS_PER_SAMPLE = 1_000
WARMUP_SAMPLES = 5
MEASURED_SAMPLES = 20


class NoOp(Strategy):
    def on_book_update(self, update):
        pass


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[int(fraction * (len(ordered) - 1))]


def report(label, values):
    print(
        f"{label}: min={min(values)} mean={statistics.mean(values):.1f} "
        f"p50={percentile(values, 0.50)} p95={percentile(values, 0.95)} "
        f"p99={percentile(values, 0.99)} ns/sample"
    )


def sample(depth):
    strategy = NoOp()
    for _ in range(WARMUP_SAMPLES):
        _benchmark_book_callbacks(strategy, depth, CALLBACKS_PER_SAMPLE)
    values = [
        _benchmark_book_callbacks(strategy, depth, CALLBACKS_PER_SAMPLE)
        for _ in range(MEASURED_SAMPLES)
    ]
    report(f"top-{depth}", values)
    print(
        f"top-{depth} "
        f"mean_per_callback={statistics.mean(values) / CALLBACKS_PER_SAMPLE:.1f} ns"
    )


if __name__ == "__main__":
    print(
        f"python={platform.python_version()} implementation={platform.python_implementation()} "
        f"os={platform.system()}-{platform.release()} arch={platform.machine()}"
    )
    print(
        f"warmup_samples={WARMUP_SAMPLES} measured_samples={MEASURED_SAMPLES} "
        f"callbacks_per_sample={CALLBACKS_PER_SAMPLE}"
    )
    print(
        "timed_region=prebuilt_payload+GIL_window+pybind_dispatch+no-op_callback; "
        "excluded=parsing,startup,logging,DataFrame"
    )
    print(
        "native_baseline=not_reported (a side-effect-free Release loop is "
        "compiler-elided; callback totals are unadjusted)"
    )
    sample(1)
    sample(15)
