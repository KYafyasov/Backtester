"""Обучение LightGBM: та же инфраструктура (feature_extraction, train/val/test с embargo)."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json
import numpy as np
import lightgbm as lgb
from feature_extraction import replay_and_extract_features

DATA_PATH = "synthetic_signal_large_slow.jsonl"
MODEL_PATH = "my_scripts/ml/lightgbm_model_7feat.txt"
FEATURE_COLS = ["imbalance", "spread", "imbalance_ma_5", "imbalance_ma_20", "momentum_5", "bid_qty", "ask_qty"]

TRAIN_FRAC = 0.6
VAL_FRAC = 0.2
EMBARGO_ROWS = 200


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
    print(f"train={len(train_idx)}  val={len(val_idx)}  test={len(test_idx)}")

    X_train, y_train = X[train_idx], y[train_idx]
    X_val, y_val = X[val_idx], y[val_idx]
    X_test, y_test = X[test_idx], y[test_idx]

    train_set = lgb.Dataset(X_train, label=y_train, feature_name=FEATURE_COLS)
    val_set = lgb.Dataset(X_val, label=y_val, reference=train_set)

    params = {
        "objective": "binary",
        "metric": "binary_logloss",
        "num_leaves": 31,
        "learning_rate": 0.05,
        "feature_fraction": 0.8,
        "bagging_fraction": 0.8,
        "bagging_freq": 5,
        "verbose": -1,
    }

    print("Обучение LightGBM...")
    model = lgb.train(
        params, train_set,
        num_boost_round=300,
        valid_sets=[train_set, val_set],
        valid_names=["train", "val"],
        callbacks=[lgb.early_stopping(stopping_rounds=20), lgb.log_evaluation(period=50)],
    )

    def acc(X_, y_):
        pred = model.predict(X_, num_iteration=model.best_iteration)
        pred_label = (pred > 0.5).astype(int)
        return (pred_label == y_).mean()

    train_acc = acc(X_train, y_train)
    val_acc = acc(X_val, y_val)
    test_acc = acc(X_test, y_test)

    print(f"Train accuracy: {train_acc:.4f}")
    print(f"Val accuracy:   {val_acc:.4f}")
    print(f"Test accuracy:  {test_acc:.4f}")
    print(f"Baseline (доля класса 1 в test): {y_test.mean():.4f}")
    print(f"Best iteration: {model.best_iteration}")

    print("\nFeature importance (gain):")
    importance = model.feature_importance(importance_type="gain")
    for name, imp in sorted(zip(FEATURE_COLS, importance), key=lambda x: -x[1]):
        print(f"  {name:20s} {imp:.1f}")

    os.makedirs(os.path.dirname(MODEL_PATH), exist_ok=True)
    model.save_model(MODEL_PATH, num_iteration=model.best_iteration)
    print(f"\nМодель сохранена в {MODEL_PATH}")

    meta = {"features": FEATURE_COLS, "train_acc": train_acc, "val_acc": val_acc, "test_acc": test_acc,
            "best_iteration": model.best_iteration}
    with open("my_scripts/ml/lightgbm_meta.json", "w") as f:
        json.dump(meta, f, indent=2)


if __name__ == "__main__":
    main()
