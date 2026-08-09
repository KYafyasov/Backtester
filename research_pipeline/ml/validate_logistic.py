"""Полная валидация LogReg: подбор гиперпараметра на val, walk-forward, permutation test."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json
import numpy as np
from sklearn.linear_model import LogisticRegression
from feature_extraction import replay_and_extract_features

DATA_PATH = "synthetic_signal_large_slow.jsonl"
FEATURE_COLS = ["imbalance", "spread", "imbalance_ma_5", "imbalance_ma_20", "momentum_5", "momentum_20", "volatility_20", "bid_qty", "ask_qty", "trade_freq_20"]

EMBARGO_ROWS = 200
N_FOLDS = 5           # walk-forward: 5 последовательных окон train->test
N_PERMUTATIONS = 20   # permutation test: сколько раз перемешиваем метки


def train_and_eval(X_train, y_train, X_test, y_test, C=1.0):
    model = LogisticRegression(max_iter=1000, C=C)
    model.fit(X_train, y_train)
    return model.score(X_test, y_test), model


def tune_hyperparameter(X, y, train_idx, val_idx):
    """Подбор C (обратная сила регуляризации) по validation accuracy."""
    candidates = [0.01, 0.1, 1.0, 10.0, 100.0]
    best_c, best_acc = None, -1
    for c in candidates:
        acc, _ = train_and_eval(X[train_idx], y[train_idx], X[val_idx], y[val_idx], C=c)
        print(f"  C={c:<8} val_acc={acc:.4f}")
        if acc > best_acc:
            best_acc, best_c = acc, c
    print(f"Лучший C={best_c} (val_acc={best_acc:.4f})")
    return best_c


def walk_forward(X, y, n_folds, embargo, best_c):
    """Несколько последовательных окон train->test со сдвигом вперёд по времени."""
    n = len(X)
    fold_size = n // (n_folds + 1)  # +1 т.к. первое окно train-only база
    results = []

    for fold in range(n_folds):
        train_end = fold_size * (fold + 1)
        test_start = train_end + embargo
        test_end = test_start + fold_size
        if test_end > n:
            break

        train_idx = np.arange(0, train_end)
        test_idx = np.arange(test_start, test_end)

        acc, _ = train_and_eval(X[train_idx], y[train_idx], X[test_idx], y[test_idx], C=best_c)
        baseline = y[test_idx].mean()
        results.append({"fold": fold, "train_size": len(train_idx), "test_size": len(test_idx),
                         "test_acc": acc, "baseline": max(baseline, 1 - baseline)})
        print(f"  Fold {fold}: train_size={len(train_idx)} test_size={len(test_idx)} "
              f"test_acc={acc:.4f} baseline={max(baseline, 1-baseline):.4f}")

    return results


def permutation_test(X, y, train_idx, test_idx, best_c, n_permutations, seed=42):
    """Перемешиваем метки train, обучаем заново, сравниваем с реальным результатом."""
    real_acc, _ = train_and_eval(X[train_idx], y[train_idx], X[test_idx], y[test_idx], C=best_c)

    rng = np.random.default_rng(seed)
    permuted_accs = []
    for i in range(n_permutations):
        y_shuffled = y[train_idx].copy()
        rng.shuffle(y_shuffled)
        acc, _ = train_and_eval(X[train_idx], y_shuffled, X[test_idx], y[test_idx], C=best_c)
        permuted_accs.append(acc)

    permuted_accs = np.array(permuted_accs)
    p_value = (permuted_accs >= real_acc).mean()
    print(f"Real test accuracy:      {real_acc:.4f}")
    print(f"Permuted mean accuracy:  {permuted_accs.mean():.4f} (std={permuted_accs.std():.4f})")
    print(f"p-value (perm >= real):  {p_value:.4f}")
    return real_acc, permuted_accs, p_value


def main():
    print("Извлечение фичей...")
    dataset = replay_and_extract_features(DATA_PATH, horizon=20, imbalance_windows=(5, 20))
    n = len(dataset)
    print(f"Собрано {n} наблюдений\n")

    X = np.array([[row[c] for c in FEATURE_COLS] for row in dataset])
    y = np.array([row["label"] for row in dataset])

    # --- 1. Подбор гиперпараметра на train/val ---
    print("=== Шаг 1: Подбор регуляризации (C) на validation ===")
    train_end = int(n * 0.6)
    val_start = train_end + EMBARGO_ROWS
    val_end = val_start + int(n * 0.2)
    train_idx = np.arange(0, train_end)
    val_idx = np.arange(val_start, min(val_end, n))
    best_c = tune_hyperparameter(X, y, train_idx, val_idx)

    # --- 2. Walk-forward валидация с лучшим C ---
    print("\n=== Шаг 2: Walk-forward (5 последовательных окон) ===")
    wf_results = walk_forward(X, y, N_FOLDS, EMBARGO_ROWS, best_c)
    wf_accs = [r["test_acc"] for r in wf_results]
    print(f"Средний test_acc по фолдам: {np.mean(wf_accs):.4f} (std={np.std(wf_accs):.4f})")

    # --- 3. Permutation test на финальном train/test срезе ---
    print("\n=== Шаг 3: Permutation test ===")
    test_start = val_end + EMBARGO_ROWS
    test_idx = np.arange(test_start, n)
    real_acc, permuted_accs, p_value = permutation_test(
        X, y, train_idx, test_idx, best_c, N_PERMUTATIONS
    )

    verdict = "ЗНАЧИМ" if p_value < 0.05 else "НЕ значим"
    print(f"\n=== Итог: сигнал {verdict} (p={p_value:.4f}, порог 0.05) ===")

    summary = {
        "best_c": best_c,
        "walk_forward_folds": wf_results,
        "walk_forward_mean_acc": float(np.mean(wf_accs)),
        "permutation_real_acc": float(real_acc),
        "permutation_mean_acc": float(permuted_accs.mean()),
        "permutation_p_value": float(p_value),
    }
    with open("my_scripts/ml/validation_report.json", "w") as f:
        json.dump(summary, f, indent=2)
    print("Отчёт сохранён в my_scripts/ml/validation_report.json")


if __name__ == "__main__":
    main()
