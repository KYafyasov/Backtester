"""
LLM multi-agent debate selector — прототип (мультипровайдерный: OpenRouter + Groq + Gemini).

MODEL_CHAIN объединяет ВСЕ модели, встречавшиеся в этой сессии на трёх
провайдерах (OpenRouter, Groq, Gemini), упорядоченные от наименее
использованной квоты к наиболее исчерпанной.

Зависимости:
    pip install groq google-genai requests

Переменные окружения:
    GROQ_API_KEY       — https://console.groq.com/keys
    GEMINI_API_KEY     — https://aistudio.google.com/apikey
    OPENROUTER_API_KEY — https://openrouter.ai/keys

Запуск:
    python3 llm_selector_prototype.py --data synthetic_signal_subset.jsonl --limit-bars 30
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys
import time
from dataclasses import dataclass, asdict
from datetime import datetime, timezone

try:
    from groq import Groq
except ImportError:
    Groq = None

try:
    from google import genai
except ImportError:
    genai = None

try:
    import requests
except ImportError:
    requests = None


MODEL_CHAIN = [
    # --- OpenRouter: свежие, не тронуты за сессию ---
    ("openrouter", "nvidia/nemotron-3-ultra-550b-a55b:free"),
    ("openrouter", "nvidia/nemotron-3-super-120b-a12b:free"),
    ("openrouter", "google/gemma-4-31b-it:free"),
    ("openrouter", "google/gemma-4-26b-a4b-it:free"),
    ("openrouter", "nvidia/nemotron-3-nano-30b-a3b:free"),
    ("openrouter", "nvidia/nemotron-nano-9b-v2:free"),
    ("openrouter", "inclusionai/ling-3.0-tiny:free"),
    ("openrouter", "poolside/laguna-s-2.1:free"),
    ("openrouter", "poolside/laguna-xs-2.1:free"),
    ("openrouter", "cohere/north-mini-code:free"),
    ("openrouter", "openai/gpt-oss-20b:free"),
    # --- Groq: не тронуты за сессию ---
    ("groq", "openai/gpt-oss-safeguard-20b"),
    ("groq", "allam-2-7b"),
    ("groq", "groq/compound"),
    ("groq", "groq/compound-mini"),
    # --- Gemini: не тронута за сессию ---
    ("gemini", "gemini-2.5-flash-lite"),
    # --- Groq: частично использованы, но с большим TPD-запасом ---
    ("groq", "openai/gpt-oss-120b"),
    ("groq", "qwen/qwen3.6-27b"),
    ("groq", "openai/gpt-oss-20b"),
    # --- Groq: наиболее использована (500K TPD), но исторически самый большой лимит ---
    ("groq", "llama-3.1-8b-instant"),
    # --- Groq: TPD исчерпан в этой сессии (100K/100K) ---
    ("groq", "llama-3.3-70b-versatile"),
    # --- Gemini: RPD исчерпаны в этой сессии ---
    ("gemini", "gemini-3.6-flash"),
    ("gemini", "gemini-2.5-flash"),
    ("gemini", "gemini-3.5-flash-lite"),
]
_current_model_idx = 0

RATE_LIMIT_SLEEP_SECONDS = 2.5
MAX_RETRIES = 5
RETRY_BACKOFF_BASE = 5.0
HISTORY_LOOKBACK = 5
N_PERMUTATIONS = 1000


@dataclass
class Bar:
    bar_index: int
    n_events: int
    open: float
    high: float
    low: float
    close: float
    avg_imbalance: float
    momentum: float


def load_bars(path: str, bar_events: int, limit_bars: int | None,
              skip_events: int = 0) -> list[Bar]:
    state = {"best_bid": None, "best_ask": None, "bid_qty": 0, "ask_qty": 0}
    bars: list[Bar] = []
    mids: list[float] = []
    imbalances: list[float] = []
    bar_idx = 0
    events_seen = 0

    def flush_bar():
        nonlocal bar_idx
        if not mids:
            return
        bars.append(Bar(
            bar_index=bar_idx, n_events=len(mids),
            open=mids[0], high=max(mids), low=min(mids), close=mids[-1],
            avg_imbalance=sum(imbalances) / len(imbalances), momentum=0.0,
        ))
        bar_idx += 1
        mids.clear()
        imbalances.clear()

    with open(path) as f:
        for line in f:
            if limit_bars is not None and len(bars) >= limit_bars:
                break
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
            imbalance = (state["bid_qty"] - state["ask_qty"]) / total
            mids.append(mid)
            imbalances.append(imbalance)
            if len(mids) >= bar_events:
                flush_bar()

    flush_bar()
    for i in range(1, len(bars)):
        bars[i].momentum = bars[i].close - bars[i - 1].close
    return bars


def format_history(history_bars: list[Bar]) -> str:
    if not history_bars:
        return "(истории предыдущих баров нет — это первый бар в выборке)"
    lines = ["История предыдущих баров (от старых к новым):"]
    for h in history_bars:
        lines.append(f"  бар #{h.bar_index}: close={h.close:.4f} "
                      f"imbalance={h.avg_imbalance:.4f} momentum={h.momentum:.4f}")
    return "\n".join(lines)


BULL_PROMPT = """Ты — агент BULL в системе алгоритмической торговли.
Твоя роль: находить аргументы ЗА long-позицию (или удержание long),
основываясь строго на приведённых данных. Не выдумывай факты вне данных.

{history_block}

Текущий бар #{bar_index}:
  open={open:.4f} high={high:.4f} low={low:.4f} close={close:.4f}
  order_book_imbalance={avg_imbalance:.4f} (>0 = давление на покупку)
  momentum_vs_prev_bar={momentum:.4f}

Используй историю, чтобы оценить, продолжается ли тренд или разворачивается —
это важнее, чем просто описать текущий бар. Дай короткий (2-4 предложения)
аргумент за long. Заверши строкой:
CONFIDENCE: <число от 0 до 1>"""

BEAR_PROMPT = """Ты — агент BEAR в системе алгоритмической торговли.
Твоя роль: находить аргументы ЗА short-позицию (или выход из long),
основываясь строго на приведённых данных. Не выдумывай факты вне данных.

{history_block}

Текущий бар #{bar_index}:
  open={open:.4f} high={high:.4f} low={low:.4f} close={close:.4f}
  order_book_imbalance={avg_imbalance:.4f} (>0 = давление на покупку)
  momentum_vs_prev_bar={momentum:.4f}

Используй историю, чтобы оценить, продолжается ли тренд или разворачивается —
это важнее, чем просто описать текущий бар. Дай короткий (2-4 предложения)
аргумент за short. Заверши строкой:
CONFIDENCE: <число от 0 до 1>"""

JUDGE_PROMPT = """Ты — агент JUDGE, принимающий финальное торговое решение.
Тебе даны аргументы BULL и BEAR по одному и тому же бару рыночных данных,
а также сами данные бара и история предыдущих баров.

{history_block}

Текущий бар #{bar_index}:
  open={open:.4f} high={high:.4f} low={low:.4f} close={close:.4f}
  order_book_imbalance={avg_imbalance:.4f} (>0 = давление на покупку)
  momentum_vs_prev_bar={momentum:.4f}

Аргумент BULL:
{bull_arg}

Аргумент BEAR:
{bear_arg}

ВАЖНО: НЕ выбирай сторону только потому, что у неё выше заявленный CONFIDENCE —
заявленная уверенность агента ничего не доказывает сама по себе. Оцени
самостоятельно, чьи аргументы точнее соответствуют фактическим данным
(текущий бар + история), и укажи в REASON конкретную цифру, на которую
ты опираешься.

Ответь СТРОГО в формате (без пояснений вне формата):
DECISION: LONG|SHORT|FLAT
CONFIDENCE: <число от 0 до 1>
REASON: <одно предложение, обязательно со ссылкой на конкретную цифру из данных>"""


def _is_daily_quota_error(e: Exception) -> bool:
    msg = str(e).lower()
    return "per day" in msg or "rpd" in msg or "tpd" in msg or "daily" in msg


def _call_groq(client, model, prompt):
    raw = client.chat.completions.with_raw_response.create(
        model=model, messages=[{"role": "user", "content": prompt}],
    )
    headers = raw.headers
    info = {"remaining_requests": headers.get("x-ratelimit-remaining-requests"),
            "remaining_tokens": headers.get("x-ratelimit-remaining-tokens")}
    resp = raw.parse()
    return resp.choices[0].message.content or "", info


def _call_gemini(client, model, prompt):
    resp = client.models.generate_content(model=model, contents=prompt)
    return resp.text or "", {}


def _call_openrouter(api_key, model, prompt):
    resp = requests.post(
        "https://openrouter.ai/api/v1/chat/completions",
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        json={"model": model, "messages": [{"role": "user", "content": prompt}]},
        timeout=60,
    )
    if resp.status_code != 200:
        raise RuntimeError(f"OpenRouter HTTP {resp.status_code}: {resp.text[:300]}")
    data = resp.json()
    text = data["choices"][0]["message"]["content"] or ""
    info = {
        "remaining_requests": resp.headers.get("x-ratelimit-remaining-requests"),
        "remaining_tokens": resp.headers.get("x-ratelimit-remaining-tokens"),
    }
    return text, info


def call_llm(clients: dict, prompt: str) -> str:
    global _current_model_idx
    last_err = None

    while _current_model_idx < len(MODEL_CHAIN):
        provider, model = MODEL_CHAIN[_current_model_idx]
        client = clients.get(provider)
        if client is None:
            print(f"    [пропуск] {provider}:{model} — клиент недоступен", file=sys.stderr)
            _current_model_idx += 1
            continue

        for attempt in range(MAX_RETRIES):
            try:
                if provider == "groq":
                    text, info = _call_groq(client, model, prompt)
                elif provider == "gemini":
                    text, info = _call_gemini(client, model, prompt)
                else:  # openrouter — client это строка api_key
                    text, info = _call_openrouter(client, model, prompt)
                if info.get("remaining_requests") is not None:
                    print(f"      [квота {provider}:{model}] запросов: {info['remaining_requests']}, "
                          f"токенов: {info.get('remaining_tokens')}", file=sys.stderr)
                return text
            except Exception as e:  # noqa: BLE001
                last_err = e
                if _is_daily_quota_error(e):
                    print(f"    [квота] {provider}:{model} исчерпала дневной лимит — "
                          f"переключаюсь на следующую модель", file=sys.stderr)
                    break
                wait = RETRY_BACKOFF_BASE * (attempt + 1)
                print(f"    [warn] {provider}:{model} вызов упал ({e}), retry через {wait:.0f}s "
                      f"({attempt + 1}/{MAX_RETRIES})", file=sys.stderr)
                time.sleep(wait)
        else:
            print(f"    [warn] {provider}:{model} не отвечает после {MAX_RETRIES} попыток — "
                  f"переключаюсь на следующую модель", file=sys.stderr)
        _current_model_idx += 1

    raise RuntimeError(f"Все модели в MODEL_CHAIN исчерпаны или недоступны: {last_err}")


def parse_confidence(text: str) -> float:
    m = re.search(r"CONFIDENCE:\s*([0-9]*\.?[0-9]+)", text)
    if not m:
        return 0.5
    try:
        return float(m.group(1))
    except ValueError:
        return 0.5


def parse_judge_decision(text: str) -> dict:
    decision_m = re.search(r"DECISION:\s*(LONG|SHORT|FLAT)", text, re.IGNORECASE)
    confidence_m = re.search(r"CONFIDENCE:\s*([0-9]*\.?[0-9]+)", text)
    reason_m = re.search(r"REASON:\s*(.+)", text)
    confidence = 0.0
    if confidence_m:
        try:
            confidence = float(confidence_m.group(1))
        except ValueError:
            confidence = 0.0
    return {"decision": decision_m.group(1).upper() if decision_m else "FLAT",
            "confidence": confidence,
            "reason": reason_m.group(1).strip() if reason_m else ""}


def run_debate(clients: dict, bar: Bar, history_bars: list[Bar]) -> dict:
    history_block = format_history(history_bars)
    fmt_kwargs = {**asdict(bar), "history_block": history_block}

    bull_text = call_llm(clients, BULL_PROMPT.format(**fmt_kwargs))
    time.sleep(RATE_LIMIT_SLEEP_SECONDS)
    bear_text = call_llm(clients, BEAR_PROMPT.format(**fmt_kwargs))
    time.sleep(RATE_LIMIT_SLEEP_SECONDS)
    judge_text = call_llm(clients, JUDGE_PROMPT.format(bull_arg=bull_text, bear_arg=bear_text, **fmt_kwargs))
    time.sleep(RATE_LIMIT_SLEEP_SECONDS)

    judge = parse_judge_decision(judge_text)
    return {
        "bar_index": bar.bar_index,
        "open": bar.open, "high": bar.high, "low": bar.low, "close": bar.close,
        "avg_imbalance": bar.avg_imbalance, "momentum": bar.momentum,
        "bull_argument": bull_text.strip(), "bull_confidence": parse_confidence(bull_text),
        "bear_argument": bear_text.strip(), "bear_confidence": parse_confidence(bear_text),
        "judge_decision": judge["decision"], "judge_confidence": judge["confidence"],
        "judge_reason": judge["reason"],
    }


TRANSACTION_COST = 0.5
DECISION_TO_POSITION = {"LONG": 1, "SHORT": -1, "FLAT": 0}


def compute_pnl_series(results, decisions=None):
    results_sorted = sorted(results, key=lambda r: r["bar_index"])
    if decisions is None:
        decisions = [r["judge_decision"] for r in results_sorted]
    pnls = []
    position = 0
    for i, r in enumerate(results_sorted):
        target = DECISION_TO_POSITION.get(decisions[i], 0)
        if i + 1 >= len(results_sorted):
            break
        next_close = results_sorted[i + 1]["close"]
        pnl = target * (next_close - r["close"])
        if target != position:
            pnl -= TRANSACTION_COST
        position = target
        pnls.append(pnl)
    return pnls


def compute_full_metrics(results):
    results_sorted = sorted(results, key=lambda r: r["bar_index"])
    pnls = compute_pnl_series(results)

    n_long = sum(1 for r in results_sorted if r["judge_decision"] == "LONG")
    n_short = sum(1 for r in results_sorted if r["judge_decision"] == "SHORT")
    n_flat = sum(1 for r in results_sorted if r["judge_decision"] == "FLAT")

    if not pnls:
        return {"final_pnl": 0.0, "max_drawdown": 0.0, "sharpe": 0.0, "win_rate": 0.0,
                "num_fills": 0, "profit_factor": 0.0, "avg_pnl_per_trade": 0.0,
                "num_closing_trades": 0, "n_long": n_long, "n_short": n_short, "n_flat": n_flat}

    final_pnl = sum(pnls)
    mean_pnl = final_pnl / len(pnls)
    variance = sum((p - mean_pnl) ** 2 for p in pnls) / len(pnls) if len(pnls) > 1 else 0.0
    std_pnl = variance ** 0.5
    sharpe = (mean_pnl / std_pnl) if std_pnl > 0 else 0.0

    nonzero = [p for p in pnls if p != 0]
    win_rate = (sum(1 for p in nonzero if p > 0) / len(nonzero)) if nonzero else 0.0

    gains = sum(p for p in pnls if p > 0)
    losses = -sum(p for p in pnls if p < 0)
    profit_factor = (gains / losses) if losses > 0 else (float("inf") if gains > 0 else 0.0)

    cum = 0.0
    peak = 0.0
    max_dd = 0.0
    for p in pnls:
        cum += p
        peak = max(peak, cum)
        max_dd = max(max_dd, peak - cum)

    trade_pnls = []
    current = 0.0
    position = 0
    in_trade = False
    decisions = [r["judge_decision"] for r in results_sorted]
    for i, p in enumerate(pnls):
        target = DECISION_TO_POSITION.get(decisions[i], 0)
        if target != position:
            if in_trade:
                trade_pnls.append(current)
            current = 0.0
            in_trade = (target != 0)
        current += p
        position = target
    if in_trade:
        trade_pnls.append(current)
    avg_pnl_per_trade = (sum(trade_pnls) / len(trade_pnls)) if trade_pnls else 0.0

    return {"final_pnl": final_pnl, "max_drawdown": max_dd, "sharpe": sharpe, "win_rate": win_rate,
            "num_fills": len(pnls), "profit_factor": profit_factor,
            "avg_pnl_per_trade": avg_pnl_per_trade, "num_closing_trades": len(trade_pnls),
            "n_long": n_long, "n_short": n_short, "n_flat": n_flat}


def permutation_test(results, n_permutations=N_PERMUTATIONS, seed=42):
    results_sorted = sorted(results, key=lambda r: r["bar_index"])
    real_decisions = [r["judge_decision"] for r in results_sorted]
    real_total = sum(compute_pnl_series(results, real_decisions))

    rng = random.Random(seed)
    random_totals = []
    for _ in range(n_permutations):
        shuffled = real_decisions.copy()
        rng.shuffle(shuffled)
        random_totals.append(sum(compute_pnl_series(results, shuffled)))

    n_extreme = sum(1 for t in random_totals if abs(t) >= abs(real_total))
    p_value = n_extreme / n_permutations
    random_mean = sum(random_totals) / len(random_totals)
    random_var = sum((t - random_mean) ** 2 for t in random_totals) / len(random_totals)

    return {"real_total_pnl": real_total, "random_mean_pnl": random_mean,
            "random_std_pnl": random_var ** 0.5, "p_value": p_value,
            "n_permutations": n_permutations}


def print_report(results, out_path):
    m = compute_full_metrics(results)
    print(f"\n=== Метрики LLM multi-agent debate ({out_path}) ===")
    print(f"final_pnl:            {m['final_pnl']:.2f}")
    print(f"max_drawdown:         {m['max_drawdown']:.2f}")
    print(f"sharpe:               {m['sharpe']:.3f}")
    print(f"win_rate:             {m['win_rate']:.1%}")
    print(f"num_fills:            {m['num_fills']}")
    print(f"profit_factor:        {m['profit_factor']:.3f}")
    print(f"avg_pnl_per_trade:    {m['avg_pnl_per_trade']:.2f}")
    print(f"num_closing_trades:   {m['num_closing_trades']}")
    print(f"Решения: LONG={m['n_long']} SHORT={m['n_short']} FLAT={m['n_flat']}")

    print(f"\nЗапускаю permutation test ({N_PERMUTATIONS} перестановок)...")
    pt = permutation_test(results)
    print(f"\n=== Permutation test ===")
    print(f"Реальный total PnL:  {pt['real_total_pnl']:.2f}")
    print(f"Случайный mean PnL:  {pt['random_mean_pnl']:.2f} (std={pt['random_std_pnl']:.2f})")
    print(f"p-value:             {pt['p_value']:.4f}")
    if pt["p_value"] < 0.05:
        print("=> p < 0.05: результат маловероятен при случайных решениях (возможен сигнал)")
    else:
        print("=> p >= 0.05: результат неотличим от случайных решений (сигнала не видно)")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", required=True)
    ap.add_argument("--bar-events", type=int, default=5000)
    ap.add_argument("--limit-bars", type=int, default=30)
    ap.add_argument("--skip-events", type=int, default=0)
    ap.add_argument("--out", default="llm_selector_prototype_results.jsonl")
    args = ap.parse_args()

    clients = {}
    groq_key = os.environ.get("GROQ_API_KEY")
    if groq_key and Groq is not None:
        clients["groq"] = Groq(api_key=groq_key)
    else:
        print("GROQ_API_KEY не задан или пакет groq не установлен — Groq-модели пропущены.", file=sys.stderr)

    gemini_key = os.environ.get("GEMINI_API_KEY")
    if gemini_key and genai is not None:
        clients["gemini"] = genai.Client(api_key=gemini_key)
    else:
        print("GEMINI_API_KEY не задан или пакет google-genai не установлен — Gemini-модели пропущены.",
              file=sys.stderr)

    openrouter_key = os.environ.get("OPENROUTER_API_KEY")
    if openrouter_key and requests is not None:
        clients["openrouter"] = openrouter_key
    else:
        print("OPENROUTER_API_KEY не задан или пакет requests не установлен — OpenRouter-модели пропущены.",
              file=sys.stderr)

    if not clients:
        print("Ни один провайдер не доступен.", file=sys.stderr)
        sys.exit(1)

    print(f"Агрегация {args.data} в бары по {args.bar_events} событий (лимит {args.limit_bars} баров)...")
    bars = load_bars(args.data, bar_events=args.bar_events, limit_bars=args.limit_bars,
                      skip_events=args.skip_events)
    print(f"Получено {len(bars)} баров.")
    if not bars:
        print("Баров нет — нечего обрабатывать.", file=sys.stderr)
        sys.exit(1)

    results = []
    t0 = time.time()
    with open(args.out, "w") as out_f:
        for i, bar in enumerate(bars):
            print(f"[{i + 1}/{len(bars)}] бар #{bar.bar_index} "
                  f"(close={bar.close:.4f}, imbalance={bar.avg_imbalance:.4f}) — debate...")
            history_bars = bars[max(0, i - HISTORY_LOOKBACK):i]
            try:
                result = run_debate(clients, bar, history_bars)
            except Exception as e:  # noqa: BLE001
                print(f"  [error] пропускаю бар #{bar.bar_index}: {e}", file=sys.stderr)
                continue
            result["timestamp"] = datetime.now(timezone.utc).isoformat()
            out_f.write(json.dumps(result, ensure_ascii=False) + "\n")
            out_f.flush()
            results.append(result)
            print(f"  -> DECISION={result['judge_decision']} "
                  f"conf={result['judge_confidence']:.2f}: {result['judge_reason']}")

    elapsed = time.time() - t0
    print(f"\nГотово за {elapsed:.1f}s. Обработано {len(results)}/{len(bars)} баров.")
    print(f"Результаты сохранены в {args.out}")

    if results:
        print_report(results, args.out)


if __name__ == "__main__":
    main()
