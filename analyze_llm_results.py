"""
Считает торговые метрики (PnL, Sharpe, Profit Factor, win rate, max drawdown)
по результатам LLM multi-agent debate — в той же логике, что metrics.py
для остальных стратегий (LogReg/LightGBM/MLP/TCN/imbalance и т.д.), чтобы
можно было сравнивать напрямую.

Работает как со старыми результатами (без сохранённых OHLC — тогда бары
пересчитываются заново из исходного jsonl с теми же параметрами генерации),
так и с новыми (где OHLC уже сохранены в самом результате).

Логика PnL идентична train_dqn_v2.py: reward = position * (next_close - close),
TRANSACTION_COST штрафуется при смене позиции — тот же принцип, чтобы
сравнение с DQN тоже было корректным (одинаковая модель издержек).

Запуск (если в results.jsonl уже есть OHLC — --data не нужен):
    python3 analyze_llm_results.py --results llm_exp_groq_v2.jsonl

Запуск (для старых файлов без OHLC — нужно пересчитать бары):
    python3 analyze_llm_results.py --results llm_exp_groq.jsonl \\
        --data synthetic_signal_subset.jsonl --bar-events 5000 --skip-events 0
"""
from __future__ import annotations

import argparse
import json
import random
import sys
from dataclasses import dataclass

TRANSACTION_COST = 0.5  # тот же штраф, что в train_dqn_v2.py — для честного сравнения
DECISION_TO_POSITION = {"LONG": 1, "SHORT": -1, "FLAT": 0}
N_PERMUTATIONS = 1000  # сколько случайных перестановок для permutation test


def load_bars_close(path: str, bar_events: int, skip_events: int) -> dict[int, float]:
    """Пересчитывает close-цены баров из исходного jsonl — нужно только
    для старых результатов, где OHLC не сохранялись в самом результате."""
    state = {"best_bid": None, "best_ask": None, "bid_qty": 0, "ask_qty": 0}
    closes: dict[int, float] = {}
    mids: list[float] = []
    bar_idx = 0
    events_seen = 0

    def flush_bar():
        nonlocal bar_idx
        if not mids:
            return
        closes[bar_idx] = mids[-1]
        bar_idx += 1
        mids.clear()

    with open(path) as f:
        for line in f:
            event = json.loads(line)
            if event.get("hd", {}).get("instrument_id") != 1:
                continue
            action = event.get("action")
            if action not in ("A", "M"):
                continue
            price = int(event["price"])
            size = event["size"]
            side = event["side"]
            if side == "B":
                state["best_bid"] = price
                state["bid_qty"] = size
            elif side == "A":
                state["best_ask"] = price
                state["ask_qty"] = size
            if state["best_bid"] is None or state["best_ask"] is None:
                continue
            total = state["bid_qty"] + state["ask_qty"]
            if total == 0:
                continue
            events_seen += 1
            if events_seen <= skip_events:
                continue
            mid = (state["best_bid"] + state["best_ask"]) / 2
            mids.append(mid)
            if len(mids) >= bar_events:
                flush_bar()
    flush_bar()
    return closes


def load_results(path: str) -> list[dict]:
    results = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                results.append(json.loads(line))
    return results


def compute_pnl_series(results: list[dict], closes: dict[int, float],
                        decisions: list[str] | None = None) -> list[float]:
    """Считает список PnL по барам. Если decisions передан явно (для
    permutation test) — используются они вместо judge_decision из results,
    но порядок и bar_index остаются от results (только решения переставлены)."""
    results = sorted(results, key=lambda r: r["bar_index"])
    if decisions is None:
        decisions = [r["judge_decision"] for r in results]

    pnls: list[float] = []
    position = 0

    for i, r in enumerate(results):
        bar_idx = r["bar_index"]
        target_position = DECISION_TO_POSITION.get(decisions[i], 0)

        close_now = closes.get(bar_idx)
        close_next = None
        if i + 1 < len(results):
            close_next = closes.get(results[i + 1]["bar_index"])

        if close_now is None or close_next is None:
            continue

        pnl = target_position * (close_next - close_now)
        if target_position != position:
            pnl -= TRANSACTION_COST
        position = target_position
        pnls.append(pnl)

    return pnls


def permutation_test(results: list[dict], closes: dict[int, float],
                      n_permutations: int = N_PERMUTATIONS, seed: int = 42) -> dict:
    """Перемешивает decisions случайным образом n_permutations раз и сравнивает
    реальный total PnL с распределением случайных — та же логика, что в
    Task 3 (permutation test на toxicity-классификаторах, p≈0 для Class 6).

    p-value = доля случайных перестановок, где |PnL_random| >= |PnL_real|.
    Низкий p-value (например < 0.05) означает, что реальный результат
    маловероятен при случайных решениях — то есть decisions судьи несут сигнал,
    а не просто угадывают направление тренда постфактум.
    """
    rng = random.Random(seed)
    real_decisions = [r["judge_decision"] for r in sorted(results, key=lambda r: r["bar_index"])]
    real_pnls = compute_pnl_series(results, closes, real_decisions)
    real_total = sum(real_pnls)

    random_totals = []
    for _ in range(n_permutations):
        shuffled = real_decisions.copy()
        rng.shuffle(shuffled)
        pnls = compute_pnl_series(results, closes, shuffled)
        random_totals.append(sum(pnls))

    n_extreme = sum(1 for t in random_totals if abs(t) >= abs(real_total))
    p_value = n_extreme / n_permutations

    random_mean = sum(random_totals) / len(random_totals)
    variance = sum((t - random_mean) ** 2 for t in random_totals) / len(random_totals)
    random_std = variance ** 0.5

    return {
        "real_total_pnl": real_total,
        "random_mean_pnl": random_mean,
        "random_std_pnl": random_std,
        "p_value": p_value,
        "n_permutations": n_permutations,
    }


@dataclass
class Metrics:
    n_trades: int
    total_pnl: float
    sharpe: float
    profit_factor: float
    win_rate: float
    max_drawdown: float
    n_long: int
    n_short: int
    n_flat: int


def compute_metrics(results: list[dict], closes: dict[int, float]) -> Metrics:
    """Считает метрики в стиле research_pipeline/metrics.py."""
    sorted_results = sorted(results, key=lambda r: r["bar_index"])
    n_long = sum(1 for r in sorted_results if r["judge_decision"] == "LONG")
    n_short = sum(1 for r in sorted_results if r["judge_decision"] == "SHORT")
    n_flat = sum(1 for r in sorted_results if r["judge_decision"] == "FLAT")

    pnls = compute_pnl_series(results, closes)

    if not pnls:
        return Metrics(0, 0.0, 0.0, 0.0, 0.0, 0.0, n_long, n_short, n_flat)

    total_pnl = sum(pnls)
    mean_pnl = total_pnl / len(pnls)
    variance = sum((p - mean_pnl) ** 2 for p in pnls) / len(pnls) if len(pnls) > 1 else 0.0
    std_pnl = variance ** 0.5
    sharpe = (mean_pnl / std_pnl) if std_pnl > 0 else 0.0

    gains = sum(p for p in pnls if p > 0)
    losses = -sum(p for p in pnls if p < 0)
    profit_factor = (gains / losses) if losses > 0 else float("inf") if gains > 0 else 0.0

    wins = sum(1 for p in pnls if p > 0)
    win_rate = wins / len(pnls)

    cum = 0.0
    peak = 0.0
    max_dd = 0.0
    for p in pnls:
        cum += p
        peak = max(peak, cum)
        max_dd = max(max_dd, peak - cum)

    return Metrics(
        n_trades=len(pnls),
        total_pnl=total_pnl,
        sharpe=sharpe,
        profit_factor=profit_factor,
        win_rate=win_rate,
        max_drawdown=max_dd,
        n_long=n_long,
        n_short=n_short,
        n_flat=n_flat,
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--results", required=True, help="jsonl с результатами llm_selector_prototype.py")
    ap.add_argument("--data", help="исходный synthetic_signal*.jsonl (нужен только если в results нет OHLC)")
    ap.add_argument("--bar-events", type=int, default=5000)
    ap.add_argument("--skip-events", type=int, default=0)
    ap.add_argument("--n-permutations", type=int, default=N_PERMUTATIONS,
                     help="сколько случайных перестановок для permutation test (по умолчанию 1000)")
    ap.add_argument("--no-permutation", action="store_true",
                     help="пропустить permutation test (быстрее, но без p-value)")
    args = ap.parse_args()

    results = load_results(args.results)
    if not results:
        print("Пустой файл результатов.", file=sys.stderr)
        sys.exit(1)

    if "close" in results[0]:
        print("В результатах уже есть OHLC — пересчёт баров не нужен.")
        closes = {r["bar_index"]: r["close"] for r in results}
    else:
        if not args.data:
            print("В результатах нет OHLC — нужен --data для пересчёта баров "
                  "(с теми же --bar-events/--skip-events, что при генерации).", file=sys.stderr)
            sys.exit(1)
        print(f"OHLC не сохранены в результатах — пересчитываю бары из {args.data}...")
        closes = load_bars_close(args.data, args.bar_events, args.skip_events)

    m = compute_metrics(results, closes)

    print(f"\n=== Метрики LLM multi-agent debate ({args.results}) ===")
    print(f"Сделок обработано:  {m.n_trades}")
    print(f"Total PnL:           {m.total_pnl:.2f}")
    print(f"Sharpe:              {m.sharpe:.3f}")
    print(f"Profit Factor:       {m.profit_factor:.3f}")
    print(f"Win rate:            {m.win_rate:.1%}")
    print(f"Max drawdown:        {m.max_drawdown:.2f}")
    print(f"Решения: LONG={m.n_long} SHORT={m.n_short} FLAT={m.n_flat}")

    if not args.no_permutation:
        print(f"\nЗапускаю permutation test ({args.n_permutations} перестановок)...")
        pt = permutation_test(results, closes, n_permutations=args.n_permutations)
        print(f"\n=== Permutation test ===")
        print(f"Реальный total PnL:     {pt['real_total_pnl']:.2f}")
        print(f"Случайный mean PnL:     {pt['random_mean_pnl']:.2f} (std={pt['random_std_pnl']:.2f})")
        print(f"p-value:                {pt['p_value']:.4f}")
        if pt["p_value"] < 0.05:
            print("=> p < 0.05: реальный результат маловероятен при случайных решениях "
                  "(возможен сигнал, но проверить на большей выборке)")
        else:
            print("=> p >= 0.05: реальный результат неотличим от случайных решений "
                  "(сигнала пока не видно)")


if __name__ == "__main__":
    main()
