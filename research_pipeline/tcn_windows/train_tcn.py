"""Обучение TCN (PyTorch, GPU) на окнах последовательностей. Запуск из Windows: py train_tcn.py"""
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
MODEL_PATH = "tcn_model.pt"
SCALER_PATH = "tcn_scaler.json"

WINDOW = 20
HORIZON = 20
TRAIN_FRAC = 0.6
VAL_FRAC = 0.2
EMBARGO_ROWS = 200
EPOCHS = 8
BATCH_SIZE = 2048
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


def main():
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")

    print("Извлечение окон последовательностей из", DATA_PATH, "...")
    t0 = time.time()
    X, y = replay_and_extract_sequences(DATA_PATH, horizon=HORIZON, window=WINDOW)
    print(f"Готово за {time.time()-t0:.1f}s. X.shape={X.shape}, y.shape={y.shape}")

    n = len(X)
    train_idx, val_idx, test_idx = split_with_embargo(n, TRAIN_FRAC, VAL_FRAC, EMBARGO_ROWS)
    print(f"train={len(train_idx)}  val={len(val_idx)}  test={len(test_idx)}")

    X_train, y_train = X[train_idx], y[train_idx]
    X_val, y_val = X[val_idx], y[val_idx]
    X_test, y_test = X[test_idx], y[test_idx]

    mean = X_train.reshape(-1, X_train.shape[-1]).mean(axis=0)
    std = X_train.reshape(-1, X_train.shape[-1]).std(axis=0)
    std[std == 0] = 1.0

    model = TCN(n_features=X.shape[-1]).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=LR)
    criterion = nn.BCEWithLogitsLoss()

    n_train = len(X_train)
    print("Обучение TCN...")
    for epoch in range(EPOCHS):
        model.train()
        perm = np.random.permutation(n_train)
        total_loss = 0.0
        t_epoch = time.time()
        for i in range(0, n_train, BATCH_SIZE):
            idx = perm[i:i+BATCH_SIZE]
            xb = (X_train[idx] - mean) / std
            xb = torch.tensor(xb, dtype=torch.float32, device=device)
            yb = torch.tensor(y_train[idx], dtype=torch.float32, device=device)

            optimizer.zero_grad()
            logits = model(xb)
            loss = criterion(logits, yb)
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * len(idx)

        avg_loss = total_loss / n_train
        val_acc = evaluate(model, X_val, y_val, mean, std, device)
        print(f"  Epoch {epoch+1}/{EPOCHS}: loss={avg_loss:.4f} val_acc={val_acc:.4f} ({time.time()-t_epoch:.1f}s)")

    train_acc = evaluate(model, X_train, y_train, mean, std, device)
    val_acc = evaluate(model, X_val, y_val, mean, std, device)
    test_acc = evaluate(model, X_test, y_test, mean, std, device)

    print(f"\nTrain accuracy: {train_acc:.4f}")
    print(f"Val accuracy:   {val_acc:.4f}")
    print(f"Test accuracy:  {test_acc:.4f}")
    print(f"Baseline (доля класса 1 в test): {y_test.mean():.4f}")

    torch.save(model.state_dict(), MODEL_PATH)
    with open(SCALER_PATH, "w") as f:
        json.dump({"mean": mean.tolist(), "std": std.tolist(), "window": WINDOW,
                    "train_acc": train_acc, "val_acc": val_acc, "test_acc": test_acc}, f, indent=2)
    print(f"\nМодель сохранена в {MODEL_PATH}")
    print(f"Scaler сохранён в {SCALER_PATH}")


if __name__ == "__main__":
    main()
