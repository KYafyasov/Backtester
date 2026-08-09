"""Обучение MLP (PyTorch, GPU) на тех же 7 фичах, что LightGBM. Та же инфраструктура train/val/test с embargo."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json
import numpy as np
import torch
import torch.nn as nn
from feature_extraction import replay_and_extract_features

DATA_PATH = "synthetic_signal_large_slow.jsonl"
MODEL_PATH = "my_scripts/ml/mlp_model.pt"
SCALER_PATH = "my_scripts/ml/mlp_scaler.json"
FEATURE_COLS = ["imbalance", "spread", "imbalance_ma_5", "imbalance_ma_20", "momentum_5", "bid_qty", "ask_qty"]

TRAIN_FRAC = 0.6
VAL_FRAC = 0.2
EMBARGO_ROWS = 200
EPOCHS = 10
BATCH_SIZE = 4096
LR = 1e-3


def split_with_embargo(n, train_frac, val_frac, embargo):
    train_end = int(n * train_frac)
    val_start = train_end + embargo
    val_end = val_start + int(n * val_frac)
    test_start = val_end + embargo
    train_idx = np.arange(0, train_end)
    val_idx = np.arange(val_start, min(val_end, n))
    test_idx = np.arange(test_start, n)
    return train_idx, val_idx, test_idx


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


def evaluate(model, X, y, device, batch_size=65536):
    model.eval()
    correct = 0
    with torch.no_grad():
        for i in range(0, len(X), batch_size):
            xb = torch.tensor(X[i:i+batch_size], dtype=torch.float32, device=device)
            yb = y[i:i+batch_size]
            logits = model(xb)
            pred = (torch.sigmoid(logits) > 0.5).cpu().numpy().astype(int)
            correct += (pred == yb).sum()
    return correct / len(X)


def main():
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")

    print("Извлечение фичей из", DATA_PATH, "...")
    dataset = replay_and_extract_features(DATA_PATH, horizon=20, imbalance_windows=(5, 20))
    n = len(dataset)
    print(f"Собрано {n} наблюдений")

    X = np.array([[row[c] for c in FEATURE_COLS] for row in dataset], dtype=np.float32)
    y = np.array([row["label"] for row in dataset], dtype=np.int64)

    train_idx, val_idx, test_idx = split_with_embargo(n, TRAIN_FRAC, VAL_FRAC, EMBARGO_ROWS)
    print(f"train={len(train_idx)}  val={len(val_idx)}  test={len(test_idx)}")

    X_train, y_train = X[train_idx], y[train_idx]
    X_val, y_val = X[val_idx], y[val_idx]
    X_test, y_test = X[test_idx], y[test_idx]

    # нормализация — считаем nmean/std ТОЛЬBКО на train, применяем ко всем
    mean = X_train.mean(axis=0)
    std = X_train.std(axis=0)
    std[std == 0] = 1.0
    X_train = (X_train - mean) / std
    X_val = (X_val - mean) / std
    X_test = (X_test - mean) / std

    model = MLP(len(FEATURE_COLS)).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=LR)
    criterion = nn.BCEWithLogitsLoss()

    n_train = len(X_train)
    print("Обучение MLP...")
    for epoch in range(EPOCHS):
        model.train()
        perm = np.random.permutation(n_train)
        total_loss = 0.0
        for i in range(0, n_train, BATCH_SIZE):
            idx = perm[i:i+BATCH_SIZE]
            xb = torch.tensor(X_train[idx], dtype=torch.float32, device=device)
            yb = torch.tensor(y_train[idx], dtype=torch.float32, device=device)

            optimizer.zero_grad()
            logits = model(xb)
            loss = criterion(logits, yb)
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * len(idx)

        avg_loss = total_loss / n_train
        val_acc = evaluate(model, X_val, y_val, device)
        print(f"  Epoch {epoch+1}/{EPOCHS}: loss={avg_loss:.4f} val_acc={val_acc:.4f}")

    train_acc = evaluate(model, X_train, y_train, device)
    val_acc = evaluate(model, X_val, y_val, device)
    test_acc = evaluate(model, X_test, y_test, device)

    print(f"\nTrain accuracy: {train_acc:.4f}")
    print(f"Val accuracy:   {val_acc:.4f}")
    print(f"Test accuracy:  {test_acc:.4f}")
    print(f"Baseline (доля класса 1 в test): {y_test.mean():.4f}")

    os.makedirs(os.path.dirname(MODEL_PATH), exist_ok=True)
    torch.save(model.state_dict(), MODEL_PATH)
    with open(SCALER_PATH, "w") as f:
        json.dump({"features": FEATURE_COLS, "mean": mean.tolist(), "std": std.tolist(),
                    "train_acc": train_acc, "val_acc": val_acc, "test_acc": test_acc}, f, indent=2)
    print(f"\nМодель сохранена в {MODEL_PATH}")
    print(f"Scaler сохранён в {SCALER_PATH}")


if __name__ == "__main__":
    main()
