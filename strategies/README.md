# Strategies

All trading and meta-strategies for the project live here.

## Structure

- `common/` — shared interfaces and signal structures (`TrackingMixin`).
- `rule_based/` — imbalance, momentum, mean-reversion, imbalance_confirmed.
- `ml/` — Logistic Regression, LightGBM.
- `neural/` — MLP, TCN.
- `meta/` — fixed ensemble, gating model, contextual bandit (planned).
- `llm/` — LLM selector and its prompts/validation (planned).
- `rl/` — RL environment and RL meta-agent (draft DQN in research_pipeline/ml/train_dqn.py, needs further work).

All strategies were validated on synthetic data via walk-forward and permutation
testing (see `research_pipeline/ml/validation_report*.json`).

Run comparison across all strategies:
```bash
python research_pipeline/run_comparison.py --strategies all -d synthetic_signal_large_slow.jsonl
```
