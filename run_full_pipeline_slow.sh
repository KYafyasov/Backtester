#!/bin/bash
set -e
cd ~/back-tester-2026

echo "=========================================="
echo "ШАГ 1: Переобучение LogReg на медленном датасете"
echo "=========================================="
sed -i 's/DATA_PATH = "synthetic_signal_large.jsonl"/DATA_PATH = "synthetic_signal_large_slow.jsonl"/' my_scripts/ml/train_logistic.py
time uv run python3 my_scripts/ml/train_logistic.py

echo "=========================================="
echo "ШАГ 2: Валидация LogReg (walk-forward + permutation test)"
echo "=========================================="
sed -i 's/DATA_PATH = "synthetic_signal_large.jsonl"/DATA_PATH = "synthetic_signal_large_slow.jsonl"/' my_scripts/ml/validate_logistic.py
time uv run python3 my_scripts/ml/validate_logistic.py

echo "=========================================="
echo "ШАГ 3: Переобучение LightGBM на медленном датасете"
echo "=========================================="
sed -i 's/DATA_PATH = "synthetic_signal_large.jsonl"/DATA_PATH = "synthetic_signal_large_slow.jsonl"/' my_scripts/ml/train_lightgbm.py
time uv run python3 my_scripts/ml/train_lightgbm.py

echo "=========================================="
echo "ШАГ 4: Валидация LightGBM (walk-forward + permutation test)"
echo "=========================================="
sed -i 's/DATA_PATH = "synthetic_signal_large.jsonl"/DATA_PATH = "synthetic_signal_large_slow.jsonl"/' my_scripts/ml/validate_lightgbm.py
time uv run python3 my_scripts/ml/validate_lightgbm.py

echo "=========================================="
echo "ШАГ 5: Переобучение MLP на медленном датасете"
echo "=========================================="
sed -i 's/DATA_PATH = "synthetic_signal_large.jsonl"/DATA_PATH = "synthetic_signal_large_slow.jsonl"/' my_scripts/ml/train_mlp.py
time uv run python3 my_scripts/ml/train_mlp.py

echo "=========================================="
echo "ШАГ 6: Валидация MLP (walk-forward + permutation test)"
echo "=========================================="
sed -i 's/DATA_PATH = "synthetic_signal_large.jsonl"/DATA_PATH = "synthetic_signal_large_slow.jsonl"/' my_scripts/ml/validate_mlp.py
time uv run python3 my_scripts/ml/validate_mlp.py

echo "=========================================="
echo "ШАГ 7: Полное сравнение всех стратегий на медленном датасете"
echo "=========================================="
time uv run python3 my_scripts/run_comparison.py --strategies all -d synthetic_signal_large_slow.jsonl

echo "=========================================="
echo "ГОТОВО! Пайплайн завершён (TCN обучается отдельно, в Windows на GPU)."
echo "=========================================="
