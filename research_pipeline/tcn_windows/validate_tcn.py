"""Полная валидация TCN: walk-forward, permutation test. Запуск из Windows: py validate_tcn.py"""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json
import time
import numpy as np
import torch
import torch.nn as nn
from feature_extraction_tcn import replay_and_extract_sequences

DATA_PATH = r"C:\WSL\Ubuntu1\rootfs\home\nikolay\back-tester-2026\synthetic_signal_large.jsonl"

WINDOW = 20
HORIZON = 20
EMBARGO_ROWS = 200
N_FOLDS = 5
N_PERMUTATIONS = 10
EPOCHS = 5
BATCH_SIZE = 2048
LR = 1e-3


class CausalConv1d(nn.Module):
    def __init__(self, in_ch, out_ch, kernel_size, dilation):
        super().__init__()
        self.pad = (kernel_size - 1) * dilation
        self.conv = nn.Conv1d(in_ch, out_ch, kernel_size, padding=self.pad, dilation=dilation)

    def forward(self, x):
        out = self.conv(x)
        return out[:, :, :-self.pad] if self.pad > 0 else out


class TCNBlock(nn.Module):
    def __init__(self, in_ch, out_ch, kernel_size, dilation):
        super().__init__()
        self.conv1 = CausalConv1d(in_ch, out_ch, kernel_size, dilation)
        self.relu1 = nn.ReLU()
        self.conv2 = CausalConv1d(out_ch, out_ch, kernel_size, dilation)
        self.relu2 = nn.ReLU()
        self.downsample = nn.Conv1d(in_ch, out_ch, 1) if in_ch != out_ch else None

    def forward(self, x):
        out = self.relu1(self.conv1(x))
        out = self.relu2(self.conv2(out))
        res = x if self.downsample is None else self.downsample(x)
        return out + res


class TCN(nn.Module):
    def __init__(self, n_features, channels=(32, 32, 32), kernel_size=3):
        super().__init__()
        layers = []
        in_ch = n_features
        for i, out_ch in enumerate(channels):
            dilation = 2 ** i
            layers.append(TCNBlock(in_ch, out_ch, kernel_size, dilation))
            in_ch = out_ch
        self.tcn = nn.Sequential(*layers)
        self.head = nn.Linear(in_ch, 1)

    def forward(self, x):
        x = x.transpose(1, 2)
        out = self.tcn(x)
        last = out[:, :, -1]
        return self.head(last).squeeze(-1)


def train_tcn_quick(X_train, y_train, device, epochs=EPOCHS):
    mean = X_train.reshape(-1, X_train.shape[-1]).mean(axis=0)
    std = X_train.reshape(-1, X_train.shape[-1]).std(axis=0)
    std[std == 0] = 1.0

    model = TCN(n_features=X_train.shape[-1]).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=LR)
    criterion = nn.BCEWithLogitsLoss()
    n = len(X_train)

    for epoch in range(epochs):
        model.train()
        perm = np.random.permutation(n)
        for i in range(0, n, BATCH_SIZE):
            idx = perm[i:i+BATCH_SIZE]
            xb = (X_train[idx] - mean) / std
            xb = torch.tensor(xb, dtype=torch.float32, device=device)
            yb = torch.tensor(y_train[idx], dtype=torch.float32, device=device)
            optimizer.zero_grad()
            loss = criterion(model(xb), yb)
            loss.backward()
            optimizer.step()

    return model, mean, std


def evaluate(model, X, y, mean, std, device, batch_size=8192):
    model.eval()
    correct = 0
    with torch.no_grad():
        for i in range(0, len(X), batch_size):
            xb = (X[i:i+batch_size] - mean) / std
            xb = torch.tensor(xb, dtype=torch.float32, device=device)
            yb = y[i:i+batch_size]
            pred = (torch.sigmoid(model(xb)) > 0.5).cpu().numpy().astype(int)
            correct += (pred == yb).sum()
    return correct / len(X)


def train_and_eval(X_train, y_train, X_test, y_test, device, epochs=EPOCHS):
    model, mean, std = train_tcn_quick(X_train, y_train, device, epochs)
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

        t0 = time.time()
        acc, _ = train_and_eval(X[train_idx], y[train_idx], X[test_idx], y[test_idx], device)
        baseline = y[test_idx].mean()
        results.append({"fold": fold, "train_size": int(len(train_idx)), "test_size": int(len(test_idx)),
                         "test_acc": float(acc), "baseline": float(max(baseline, 1 - baseline))})
        print(f"  Fold {fold}: train_size={len(train_idx)} test_size={len(test_idx)} "
              f"test_acc={acc:.4f} baseline={max(baseline, 1-baseline):.4f} ({time.time()-t0:.1f}s)")

    return results


def permutation_test(X, y, train_idx, test_idx, device, n_permutations, seed=42):
    real_acc, _ = train_and_eval(X[train_idx], y[train_idx], X[test_idx], y[test_idx], device)

    rng = np.random.default_rng(seed)
    permuted_accs = []
    for i in range(n_permutations):
        y_shuffled = y[train_idx].copy()
        rng.shuffle(y_shuffled)
        t0 = time.time()
        acc, _ = train_and_eval(X[train_idx], y_shuffled, X[test_idx], y[test_idx], device)
        permuted_accs.append(acc)
        print(f"  permutation {i+1}/{n_permutations}: acc={acc:.4f} ({time.time()-t0:.1f}s)")

    permuted_accs = np.array(permuted_accs)
    p_value = (permuted_accs >= real_acc).mean()
    print(f"Real test accuracy:      {real_acc:.4f}")
    print(f"Permuted mean accuracy:  {permuted_accs.mean():.4f} (std={permuted_accs.std():.4f})")
    print(f"p-value (perm >= real):  {p_value:.4f}")
    return real_acc, permuted_accs, p_value


def main():
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")

    print("Извлечение окон...")
    X, y = replay_and_extract_sequences(DATA_PATH, horizon=HORIZON, window=WINDOW)
    n = len(X)
    print(f"X.shape={X.shape}\n")

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
    with open("tcn_validation_report.json", "w") as f:
        json.dump(summary, f, indent=2)
    print("Отчёт сохранён в tcn_validation_report.json")


if __name__ == "__main__":
    main()
