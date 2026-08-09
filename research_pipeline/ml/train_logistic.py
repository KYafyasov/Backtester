"""Обучение LogReg: train/val/test split с embargo между сегментами.
embargo нужен, потому что фичи imbalance_ma_20 используют скользящее окно —
без зазора события на границе train/test могут пересекаться и создавать утечку."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json
import numpy as np
from sklearn.linear_model import LogisticRegression
from feature_extraction import replay_and_extract_features

DATA_PATH = "synthetic_signal_large_slow.jsonl"
MODEL_PATH = "my_scripts/ml/logistic_model.json"
FEATURE_COLS = ["imbalance", "spread", "imbalance_ma_5", "imbalance_ma_20", "momentum_5", "momentum_20", "volatility_20", "bid_qty", "ask_qty", "trade_freq_20"]

TRAIN_FRAC = 0.6
VAL_FRAC = 0.2
# TEST_FRAC = 0.2 (остаток)
EMBARGO_ROWS = 200  # зазор между сегментами, в строках наблюдений


def split_with_embargo(n, train_frac, val_frac, embargo):
    train_end = int(n * train_frac)
    val_start = train_end + embargo
    val_end = val_start + int(n * val_frac)
    test_start = val_end + embargo

    train_idx = np.arange(0, train_end)
    val_idx = np.arange(val_start, min(val_end, n))
    test_idx = np.arange(test_start, n)
    return train_idx, val_idx, test_idx


def main():
    print("Извлечение фичей из", DATA_PATH, "...")
    dataset = replay_and_extract_features(DATA_PATH, horizon=20, imbalance_windows=(5, 20))
    n = len(dataset)
    print(f"Собрано {n} наблюдений")

    X = np.array([[row[c] for c in FEATURE_COLS] for row in dataset])
    y = np.array([row["label"] for row in dataset])

    train_idx, val_idx, test_idx = split_with_embargo(n, TRAIN_FRAC, VAL_FRAC, EMBARGO_ROWS)
    print(f"train={len(train_idx)}  val={len(val_idx)}  test={len(test_idx)}  (embargo={EMBARGO_ROWS} между каждым)")

    X_train, y_train = X[train_idx], y[train_idx]
    X_val, y_val = X[val_idx], y[val_idx]
    X_test, y_test = X[test_idx], y[test_idx]

    model = LogisticRegression(max_iter=1000)
    model.fit(X_train, y_train)

    train_acc = model.score(X_train, y_train)
    val_acc = model.score(X_val, y_val)
    test_acc = model.score(X_test, y_test)

    print(f"Train accuracy: {train_acc:.4f}")
    print(f"Val accuracy:   {val_acc:.4f}")
    print(f"Test accuracy:  {test_acc:.4f}")
    print(f"Baseline (доля класса 1 в test): {y_test.mean():.4f}")

    weights = {
        "features": FEATURE_COLS,
        "coef": model.coef_[0].tolist(),
        "intercept": float(model.intercept_[0]),
        "train_acc": train_acc,
        "val_acc": val_acc,
        "test_acc": test_acc,
    }
    os.makedirs(os.path.dirname(MODEL_PATH), exist_ok=True)
    with open(MODEL_PATH, "w") as f:
        json.dump(weights, f, indent=2)
    print(f"Модель сохранена в {MODEL_PATH}")


if __name__ == "__main__":
    main()
