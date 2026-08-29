"""Полная валидация MLP: walk-forward, permutation test. Подбор гиперпараметров пропускаем —
архитектура и LR уже зафиксированы в train_mlp.py, здесь фокус на стабильности и значимости."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json
import numpy as np
import torch
import torch.nn as nn
from feature_extraction import replay_and_extract_features

DATA_PATH = "synthetic_signal_large_slow.jsonl"
FEATURE_COLS = ["imbalance", "spread", "imbalance_ma_5", "imbalance_ma_20", "momentum_5", "bid_qty", "ask_qty"]

EMBARGO_ROWS = 200
N_FOLDS = 5
N_PERMUTATIONS = 10  # MLP дороже LogReg — держим минимальным для разумного времени
EPOCHS = 5            # меньше эпох, чем в основном train (10) — здесь важна скорость, не пик качества
BATCH_SIZE = 4096
LR = 1e-3


class MLP(nn.Module):
    def __init__(self, n_features):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(n_features, 64), nn.ReLU(),
            nn.Linear(64, 32), nn.ReLU(),
            nn.Linear(32, 1),
        )

    def forward(self, x):
        return self.net(x).squeeze(-1)


def train_mlp_quick(X_train, y_train, device, epochs=EPOCHS):
    mean = X_train.mean(axis=0)
    std = X_train.std(axis=0)
    std[std == 0] = 1.0
    X_train_norm = (X_train - mean) / std

    model = MLP(X_train.shape[1]).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=LR)
    criterion = nn.BCEWithLogitsLoss()
    n = len(X_train_norm)

    for epoch in range(epochs):
        model.train()
        perm = np.random.permutation(n)
        for i in range(0, n, BATCH_SIZE):
            idx = perm[i:i+BATCH_SIZE]
            xb = torch.tensor(X_train_norm[idx], dtype=torch.float32, device=device)
            yb = torch.tensor(y_train[idx], dtype=torch.float32, device=device)
            optimizer.zero_grad()
            loss = criterion(model(xb), yb)
            loss.backward()
            optimizer.step()

    return model, mean, std


def evaluate(model, X, y, mean, std, device, batch_size=65536):
    X_norm = (X - mean) / std
    model.eval()
    correct = 0
    with torch.no_grad():
        for i in range(0, len(X_norm), batch_size):
            xb = torch.tensor(X_norm[i:i+batch_size], dtype=torch.float32, device=device)
            yb = y[i:i+batch_size]
            pred = (torch.sigmoid(model(xb)) > 0.5).cpu().numpy().astype(int)
            correct += (pred == yb).sum()
    return correct / len(X_norm)


def train_and_eval(X_train, y_train, X_test, y_test, device, epochs=EPOCHS):
    model, mean, std = train_mlp_quick(X_train, y_train, device, epochs)
    acc = evaluate(model, X_test, y_test, mean, std, device)
    return acc, model


def walk_forward(X, y, n_folds, embargo, device):
    n = len(X)
    fold_size = n // (n_folds + 1)
    results = []

    for fold in range(n_folds):
        train_end = fold_size * (fold + 1)
        test_start = train_end + embargo
        test_end = test_start + fold_size
        if test_end > n:
            break

        train_idx = np.arange(0, train_end)
        test_idx = np.arange(test_start, test_end)

        acc, _ = train_and_eval(X[train_idx], y[train_idx], X[test_idx], y[test_idx], device)
        baseline = y[test_idx].mean()
        results.append({"fold": fold, "train_size": len(train_idx), "test_size": len(test_idx),
                         "test_acc": float(acc), "baseline": float(max(baseline, 1 - baseline))})
        print(f"  Fold {fold}: train_size={len(train_idx)} test_size={len(test_idx)} "
              f"test_acc={acc:.4f} baseline={max(baseline, 1-baseline):.4f}")

    return results


def permutation_test(X, y, train_idx, test_idx, device, n_permutations, seed=42):
    real_acc, _ = train_and_eval(X[train_idx], y[train_idx], X[test_idx], y[test_idx], device)

    rng = np.random.default_rng(seed)
    permuted_accs = []
    for i in range(n_permutations):
        y_shuffled = y[train_idx].copy()
        rng.shuffle(y_shuffled)
        acc, _ = train_and_eval(X[train_idx], y_shuffled, X[test_idx], y[test_idx], device)
        permuted_accs.append(acc)
        print(f"  permutation {i+1}/{n_permutations}: acc={acc:.4f}")

    permuted_accs = np.array(permuted_accs)
    p_value = (permuted_accs >= real_acc).mean()
    print(f"Real test accuracy:      {real_acc:.4f}")
    print(f"Permuted mean accuracy:  {permuted_accs.mean():.4f} (std={permuted_accs.std():.4f})")
    print(f"p-value (perm >= real):  {p_value:.4f}")
    return real_acc, permuted_accs, p_value


def main():
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")

    print("Извлечение фичей...")
    dataset = replay_and_extract_features(DATA_PATH, horizon=20, imbalance_windows=(5, 20))
    n = len(dataset)
    print(f"Собрано {n} наблюдений\n")

    X = np.array([[row[c] for c in FEATURE_COLS] for row in dataset], dtype=np.float32)
    y = np.array([row["label"] for row in dataset], dtype=np.int64)

    print("=== Шаг 1: Walk-forward (5 последовательных окон) ===")
    wf_results = walk_forward(X, y, N_FOLDS, EMBARGO_ROWS, device)
    wf_accs = [r["test_acc"] for r in wf_results]
    print(f"Средний test_acc по фолдам: {np.mean(wf_accs):.4f} (std={np.std(wf_accs):.4f})")

    print("\n=== Шаг 2: Permutation test ===")
    train_end = int(n * 0.6)
    val_end = train_end + EMBARGO_ROWS + int(n * 0.2)
    train_idx = np.arange(0, train_end)
    test_start = val_end + EMBARGO_ROWS
    test_idx = np.arange(test_start, n)
    real_acc, permuted_accs, p_value = permutation_test(X, y, train_idx, test_idx, device, N_PERMUTATIONS)

    verdict = "ЗНАЧИМ" if p_value < 0.05 else "НЕ значим"
    print(f"\n=== Итог: сигнал {verdict} (p={p_value:.4f}, порог 0.05) ===")

    summary = {
        "walk_forward_folds": wf_results,
        "walk_forward_mean_acc": float(np.mean(wf_accs)),
        "permutation_real_acc": float(real_acc),
        "permutation_mean_acc": float(permuted_accs.mean()),
        "permutation_p_value": float(p_value),
    }
    with open("my_scripts/ml/validation_report_mlp.json", "w") as f:
        json.dump(summary, f, indent=2)
    print("Отчёт сохранен в my_scripts/ml/validation_report_mlp.json")


if __name__ == "__main__":
    main()
