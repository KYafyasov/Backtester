# Research Pipeline (synthetic data)

Full research pipeline: synthetic L3 data generator with a controllable embedded
signal -> C++ engine -> comparison of rule-based / ML / DL strategies -> statistical validation.

## Data generator

- `generate_synthetic_signal_stream.py` — streaming generator with an embedded signal
  (order flow imbalance -> future price movement); `regime_change_prob` controls how
  often the underlying market regime flips.
- `synthetic_signal_large.jsonl` (not in git, generated) — fast regime (0.03).
- `synthetic_signal_large_slow.jsonl` (not in git, generated) — slow regime (0.01).

## Model training and validation (`ml/`)

- `feature_extraction.py` — single feature-extraction function used both for offline
  training and as the reference implementation for the online strategy logic.
- `train_logistic.py` / `validate_logistic.py` — LogReg.
- `train_lightgbm.py` / `validate_lightgbm.py` — LightGBM.
- `train_mlp.py` / `validate_mlp.py` — MLP (PyTorch).
- `tcn_windows/` — TCN is trained separately via native Windows Python on GPU
  (WSL1 does not support CUDA passthrough).
- Validation: train/val/test split with embargo, walk-forward (5 folds), permutation test.
  All models are statistically significant (p=0.0000).

## Strategy comparison

- `run_comparison.py --strategies all -d <file>` — runs all strategies through the C++
  engine; metrics: PnL, Sharpe, Max Drawdown, Profit Factor, Win Rate.
- `grid_search.py <strategy>` — sweeps execution parameters (confirmation_steps, min_hold_updates).

## Key finding

Regime sensitivity: the same strategies/models produce dramatically different results
depending on how often the underlying market trend flips (`regime_change_prob`).
In the slow regime (0.01), LightGBM/MLP reach a Profit Factor of ~4.85-4.86;
in the fast regime (0.03), most strategies are unprofitable without a regime-detection layer.

Full automated run: `../run_full_pipeline_slow.sh`
