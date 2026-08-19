# Options Backtester — ML/DL/RL/LLM Strategies: Run Guide

This document describes how to run every strategy added in this session:
DQN, LSTM, Voting Ensemble, and the LLM multi-agent debate prototype.

---

## Prerequisites

```bash
cd ~/back-tester-2026

# C++ engine build (uv-managed environment — torch/lightgbm/numpy live here)
SKBUILD_CMAKE_ARGS="-DCMAKE_POSITION_INDEPENDENT_CODE=ON" uv sync --locked
uv pip install torch lightgbm scikit-learn scipy pandas numpy

# LLM prototype dependencies (system/user Python — NOT the uv venv)
pip install groq google-genai requests --break-system-packages
```

**Two separate Python environments matter here:**
- `uv run python3 ...` — for anything touching `torch`, `lightgbm`, `numpy`,
  or the C++ bindings (`back_tester`). Used for: `run_comparison.py`,
  `eval_dqn.py`, `validate_ensemble.py`, `validate_lightgbm10.py`,
  `train_lstm.py`, `validate_lstm.py`.
- plain `python3 ...` — for the LLM prototype (`llm_selector_prototype.py`),
  which only needs `groq`/`google-genai`/`requests`.

---

## 1. All strategies through the C++ engine

Lists every strategy the engine currently knows:
```bash
cd research_pipeline
uv run python3 run_comparison.py --list
```

Run everything at once (rule-based + ML + DL + RL + ensemble):
```bash
uv run python3 run_comparison.py --strategies all --data ../synthetic_signal_large_slow.jsonl
```

Run a subset:
```bash
uv run python3 run_comparison.py --strategies dqn,lstm,ensemble_voting --data ../synthetic_signal_large_slow.jsonl
```

Output: a pandas table with `final_pnl`, `max_drawdown`, `sharpe`, `win_rate`,
`num_fills`, `profit_factor`, `avg_pnl_per_trade`, `num_closing_trades`,
`orders_sent`, `book_updates` for each requested strategy.

**Available strategy names:** `imbalance`, `momentum`, `mean_reversion`,
`imbalance_confirmed`, `ml_logistic`, `lightgbm`, `mlp`, `tcn`,
`ensemble_voting`, `dqn`, `lstm`.

---

## 2. Honest out-of-sample validation (walk-forward + permutation test)

These scripts do **not** use the C++ engine — they replay the raw JSONL
directly in Python. This avoids a known C++ engine issue: slicing a JSONL
mid-stream breaks order-book replay integrity (an event can reference an
`order_id` whose "add" event was in the discarded portion of the file,
causing `RuntimeError: modify references unknown historical order id`).

### 2.1 DQN

```bash
cd ~/back-tester-2026
uv run python3 eval_dqn.py \
  --data synthetic_signal_slow_test.jsonl \
  --model research_pipeline/ml/dqn_model_traintest.pt \
  --meta research_pipeline/ml/dqn_meta_traintest.json
```
Prints: full-period metrics, 5-fold walk-forward, permutation test (100
shuffles, sign of trade direction shuffled at fixed transaction frequency
to avoid confounding with commission cost).

To retrain DQN from scratch on the train-only segment:
```bash
uv run python3 -u research_pipeline/ml/train_dqn_v3_traintest.py
```
(Reads `synthetic_signal_slow_train.jsonl`, writes
`research_pipeline/ml/dqn_model_traintest.pt` +
`research_pipeline/ml/dqn_meta_traintest.json`.)

### 2.2 LSTM

```bash
cd ~/back-tester-2026/research_pipeline/lstm_windows
uv run python3 validate_lstm.py --data ../../synthetic_signal_slow_test.jsonl
```
Prints: full accuracy, 5-fold walk-forward accuracy, permutation test
(100 shuffles).

To retrain LSTM from scratch:
```bash
OMP_NUM_THREADS=2 uv run python3 -u train_lstm.py
```
(Reads `../../synthetic_signal_slow_train.jsonl`, writes `lstm_model.pt` +
`lstm_scaler.json` in the current directory — copy them to
`research_pipeline/ml/` before running the engine strategy.)

### 2.3 Voting Ensemble

```bash
cd ~/back-tester-2026
uv run python3 validate_ensemble.py --data synthetic_signal_slow_test.jsonl
```
Same walk-forward + permutation test structure. Filters by
`instrument_id==1` internally (`feature_extraction.py` does not separate
instruments on its own — mixing instrument 1 and instrument 2 prices in
one PnL series was a bug found and fixed in this session).

Optional flags:
```bash
--no-filter                    # disable confirmation_steps/min_hold filter, use raw per-tick decisions
--confirmation-steps N         # default 3
--min-hold-updates N           # default 15
--instrument-id N              # default 1
```

### 2.4 LightGBM 7-feature vs 10-feature comparison

```bash
cd ~/back-tester-2026
uv run python3 validate_lightgbm10.py --data synthetic_signal_slow_test.jsonl
```
Loads both `lightgbm_model.txt` (old, 10 features — same set as LogReg)
and `lightgbm_model_7feat.txt` (current), runs the same honest validation
on both, prints a side-by-side comparison table.

---

## 3. LLM multi-agent debate

**Environment variables required (at least one provider):**
```bash
export GROQ_API_KEY="..."
export GEMINI_API_KEY="..."
export OPENROUTER_API_KEY="..."
```
(These do not persist across terminal windows — re-export in each new
WSL session, or add to `~/.bashrc`.)

**Basic run:**
```bash
cd ~/back-tester-2026
python3 -u llm_selector_prototype.py \
  --data synthetic_signal_large_slow.jsonl \
  --bar-events 5000 \
  --limit-bars 500 \
  --skip-events 0 \
  --out llm_exp_results.jsonl
```

**Flags:**
- `--data` — source JSONL
- `--bar-events` — how many raw tick events aggregate into one bar (default 5000)
- `--limit-bars` — how many bars to process (protects against exhausting API quota)
- `--skip-events` — skip N raw valid events from the start before aggregating (order-book state is still built from event 0 — no "cold start")
- `--out` — output JSONL path

Full metrics (`final_pnl`, `max_drawdown`, `sharpe`, `win_rate`, `num_fills`,
`profit_factor`, `avg_pnl_per_trade`, `num_closing_trades`) and a
permutation test (1000 shuffles) are printed automatically at the end of
every run — no extra command needed.

**Model fallback chain:** 24 models across 3 providers (OpenRouter → Groq →
Gemini), ordered from least-used quota to most-exhausted. Automatically
switches to the next model on daily quota errors (RPD/TPD) without manual
intervention. If ALL models in the chain are exhausted, the run raises
`RuntimeError` and stops — check remaining quota via provider dashboards
before a large run.

**Resuming after a partial run:** if a run stops partway (e.g. all models
exhausted at bar #270 of 500), resume from that point:
```bash
# skip-events = (bars already processed) × bar-events
python3 -u llm_selector_prototype.py \
  --data synthetic_signal_large_slow.jsonl \
  --bar-events 5000 \
  --limit-bars 230 \
  --skip-events 1350000 \
  --out llm_exp_part2.jsonl
```

---

## 4. Known limitations

- **LLM debate is not embedded in the C++ engine.** Each decision requires
  a network round-trip (seconds), incompatible with the engine's tick-rate
  processing speed. A `LLMPrecomputedStrategy` (offline-computed decisions
  replayed through the engine) was discussed but not implemented.
- **Ensemble walk-forward via the C++ engine was not performed** for the
  same reason as the order-id slicing issue above — `validate_ensemble.py`
  runs the equivalent check as a standalone Python computation instead.
- **DQN honest test uses a model trained on 70% of the data** (train
  segment), not the full dataset — comparing against the earlier in-sample
  run (`dqn_model_slow_full.pt`, trained on the full file) showed no signs
  of overfitting (out-of-sample Sharpe was actually slightly higher).

---

## 5. File reference

| File | Purpose |
|---|---|
| `strategies/rl/dqn_strategy.py` | DQN as a `bt.Strategy` (engine-integrated) |
| `strategies/neural/lstm_strategy.py` | LSTM as a `bt.Strategy` |
| `strategies/meta/ensemble_strategy.py` | Voting ensemble as a `bt.Strategy` |
| `research_pipeline/run_comparison.py` | Registers all strategies, runs comparison table |
| `research_pipeline/ml/train_dqn_v3_traintest.py` | DQN training on train-only segment |
| `research_pipeline/lstm_windows/train_lstm.py` | LSTM training |
| `research_pipeline/lstm_windows/feature_extraction_lstm.py` | Shared window-extraction logic (train + validate) |
| `research_pipeline/lstm_windows/validate_lstm.py` | LSTM honest out-of-sample validation |
| `eval_dqn.py` | DQN honest out-of-sample validation |
| `validate_ensemble.py` | Ensemble honest out-of-sample validation |
| `validate_lightgbm10.py` | LightGBM 7-feature vs 10-feature comparison |
| `llm_selector_prototype.py` | LLM multi-agent debate (multi-provider) |
