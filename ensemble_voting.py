"""
Voting ensemble из 4 уже обученных и провалидированных моделей:
LogReg, LightGBM, MLP, TCN.

Идея: каждая модель голосует UP/DOWN на основе своей вероятности,
финальное решение — взвешенное большинство (вес = test_acc модели
из соответствующего meta-файла, чтобы более точные модели имели
больший голос — не наивное равное голосование).

TCN отличается от остальных трёх: ему нужно окно из WINDOW=20
последних сырых тиков (raw features), а не одна агрегированная
строка фичей — поэтому у него отдельный, более медленный путь
инференса (rolling window), синхронизированный по номеру строки
с остальными тремя моделями (которые все используют один и тот же
feature_extraction.py и одну и ту же строку на A/M событие).

Запуск:
    python3 ensemble_voting.py --data synthetic_signal_large_slow.jsonl --limit 200000

Зависимости: lightgbm, torch (уже должны быть установлены в проекте).
"""
from __future__ import annotations

import argparse
import json
import random
import sys
from collections import deque
from dataclasses import dataclass

import numpy as np
import torch
import torch.nn as nn
import lightgbm as lgb

sys.path.insert(0, "research_pipeline/ml")
from feature_extraction import replay_and_extract_features  # noqa: E402

TRANSACTION_COST = 0.5  # тот же штраф, что в train_dqn_v2.py / analyze_llm_results.py

MODEL_DIR = "research_pipeline/ml"

LOGREG_FEATURES = ["imbalance", "spread", "imbalance_ma_5", "imbalance_ma_20",
                    "momentum_5", "momentum_20", "volatility_20", "bid_qty",
                    "ask_qty", "trade_freq_20"]
LGB_MLP_FEATURES = ["imbalance", "spread", "imbalance_ma_5", "imbalance_ma_20",
                     "momentum_5", "bid_qty", "ask_qty"]
TCN_RAW_FEATURES = ["imbalance", "spread", "bid_qty", "ask_qty"]  # + mid_price_delta считается отдельно
TCN_WINDOW = 20


# --------------------------------------------------------------------------
# Архитектуры — идентичны strategies/neural/mlp_strategy.py и tcn_strategy.py,
# чтобы torch.load(state_dict) гарантированно совпал по форме тензоров.
# --------------------------------------------------------------------------

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


# --------------------------------------------------------------------------
# Загрузка моделей
# --------------------------------------------------------------------------

@dataclass
class Ensemble:
    logreg_coef: np.ndarray
    logreg_intercept: float
    logreg_acc: float
    lgb_model: lgb.Booster
    lgb_acc: float
    mlp_model: MLP
    mlp_mean: torch.Tensor
    mlp_std: torch.Tensor
    mlp_acc: float
    tcn_model: TCN
    tcn_mean: torch.Tensor
    tcn_std: torch.Tensor
    tcn_acc: float


def load_ensemble() -> Ensemble:
    with open(f"{MODEL_DIR}/logistic_model.json") as f:
        logreg_meta = json.load(f)

    lgb_model = lgb.Booster(model_file=f"{MODEL_DIR}/lightgbm_model_7feat.txt")
    with open(f"{MODEL_DIR}/lightgbm_meta.json") as f:
        lgb_meta = json.load(f)

    with open(f"{MODEL_DIR}/mlp_scaler.json") as f:
        mlp_scaler = json.load(f)
    mlp_model = MLP(len(LGB_MLP_FEATURES))
    mlp_model.load_state_dict(torch.load(f"{MODEL_DIR}/mlp_model.pt", map_location="cpu"))
    mlp_model.eval()

    with open(f"{MODEL_DIR}/tcn_scaler.json") as f:
        tcn_scaler = json.load(f)
    tcn_model = TCN(n_features=len(tcn_scaler["mean"]))
    tcn_model.load_state_dict(torch.load(f"{MODEL_DIR}/tcn_model.pt", map_location="cpu"))
    tcn_model.eval()

    return Ensemble(
        logreg_coef=np.array(logreg_meta["coef"]),
        logreg_intercept=logreg_meta["intercept"],
        logreg_acc=logreg_meta["test_acc"],
        lgb_model=lgb_model,
        lgb_acc=lgb_meta["test_acc"],
        mlp_model=mlp_model,
        mlp_mean=torch.tensor(mlp_scaler["mean"], dtype=torch.float32),
        mlp_std=torch.tensor(mlp_scaler["std"], dtype=torch.float32),
        mlp_acc=mlp_scaler["test_acc"],
        tcn_model=tcn_model,
        tcn_mean=torch.tensor(tcn_scaler["mean"], dtype=torch.float32),
        tcn_std=torch.tensor(tcn_scaler["std"], dtype=torch.float32),
        tcn_acc=tcn_scaler["test_acc"],
    )


def sigmoid(x: float) -> float:
    return 1.0 / (1.0 + np.exp(-x))


def predict_logreg(ens: Ensemble, row: dict) -> float:
    x = np.array([row[c] for c in LOGREG_FEATURES])
    z = np.dot(ens.logreg_coef, x) + ens.logreg_intercept
    return sigmoid(z)


def predict_lgb(ens: Ensemble, row: dict) -> float:
    x = np.array([[row[c] for c in LGB_MLP_FEATURES]])
    return float(ens.lgb_model.predict(x)[0])


def predict_mlp(ens: Ensemble, row: dict) -> float:
    x = torch.tensor([row[c] for c in LGB_MLP_FEATURES], dtype=torch.float32)
    x_norm = (x - ens.mlp_mean) / ens.mlp_std
    with torch.no_grad():
        logit = ens.mlp_model(x_norm.unsqueeze(0))
    return torch.sigmoid(logit).item()


def predict_tcn(ens: Ensemble, window: deque) -> float:
    seq = torch.tensor(list(window), dtype=torch.float32).unsqueeze(0)
    seq_norm = (seq - ens.tcn_mean) / ens.tcn_std
    with torch.no_grad():
        logit = ens.tcn_model(seq_norm)
    return torch.sigmoid(logit).item()


def weighted_vote(probs: dict[str, float], weights: dict[str, float]) -> tuple[str, float]:
    """UP если взвешенная сумма (prob - 0.5) положительна, иначе DOWN.
    Возвращает решение и объединённую уверенность (0-1)."""
    score = sum((probs[m] - 0.5) * weights[m] for m in probs)
    total_weight = sum(weights[m] for m in probs)
    norm_score = score / total_weight if total_weight > 0 else 0.0
    decision = "LONG" if norm_score > 0 else "SHORT" if norm_score < 0 else "FLAT"
    confidence = min(1.0, abs(norm_score) * 2 + 0.5)
    return decision, confidence


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", required=True, help="путь к synthetic_signal*.jsonl")
    ap.add_argument("--limit", type=int, default=None,
                     help="максимум строк-наблюдений для обработки (по умолчанию — весь файл)")
    ap.add_argument("--out", default="ensemble_results.jsonl")
    args = ap.parse_args()

    print("Загружаю модели (LogReg, LightGBM, MLP, TCN)...")
    ens = load_ensemble()
    weights = {"logreg": ens.logreg_acc, "lgb": ens.lgb_acc,
               "mlp": ens.mlp_acc, "tcn": ens.tcn_acc}
    print(f"Веса голосования (test_acc): {weights}")

    print(f"Извлечение фичей из {args.data}...")
    dataset = replay_and_extract_features(args.data, horizon=20, imbalance_windows=(5, 20))
    if args.limit:
        dataset = dataset[:args.limit]
    print(f"Собрано {len(dataset)} наблюдений.")

    tcn_window: deque = deque(maxlen=TCN_WINDOW)
    mid_price_prev = None

    n_long = n_short = n_flat = 0
    results = []

    with open(args.out, "w") as out_f:
        for i, row in enumerate(dataset):
            p_logreg = predict_logreg(ens, row)
            p_lgb = predict_lgb(ens, row)
            p_mlp = predict_mlp(ens, row)

            mid_price = row["mid_price"]
            mid_price_delta = (mid_price - mid_price_prev) if mid_price_prev is not None else 0.0
            mid_price_prev = mid_price
            raw_feat = [row[c] for c in TCN_RAW_FEATURES] + [mid_price_delta]
            tcn_window.append(raw_feat)

            probs = {"logreg": p_logreg, "lgb": p_lgb, "mlp": p_mlp}
            if len(tcn_window) >= TCN_WINDOW:
                probs["tcn"] = predict_tcn(ens, tcn_window)

            decision, confidence = weighted_vote(probs, weights)
            if decision == "LONG":
                n_long += 1
            elif decision == "SHORT":
                n_short += 1
            else:
                n_flat += 1

            result = {
                "row_index": i,
                "mid_price": mid_price,
                "label": row["label"],
                "probs": probs,
                "decision": decision,
                "confidence": confidence,
            }
            out_f.write(json.dumps(result) + "\n")
            results.append(result)

            if (i + 1) % 50000 == 0:
                print(f"  {i + 1}/{len(dataset)} обработано...")

    print(f"\nГотово. LONG={n_long} SHORT={n_short} FLAT={n_flat}")
    print(f"Результаты сохранены в {args.out}")

    # -------------------- метрики (та же формула, что analyze_llm_results.py) --------------------
    pnls = []
    position = 0
    for i, r in enumerate(results):
        target_position = {"LONG": 1, "SHORT": -1, "FLAT": 0}[r["decision"]]
        if i + 1 >= len(results):
            break
        next_price = results[i + 1]["mid_price"]
        pnl = target_position * (next_price - r["mid_price"])
        if target_position != position:
            pnl -= TRANSACTION_COST
        position = target_position
        pnls.append(pnl)

    if pnls:
        total_pnl = sum(pnls)
        mean_pnl = total_pnl / len(pnls)
        variance = sum((p - mean_pnl) ** 2 for p in pnls) / len(pnls) if len(pnls) > 1 else 0.0
        std_pnl = variance ** 0.5
        sharpe = (mean_pnl / std_pnl) if std_pnl > 0 else 0.0
        gains = sum(p for p in pnls if p > 0)
        losses = -sum(p for p in pnls if p < 0)
        profit_factor = (gains / losses) if losses > 0 else float("inf") if gains > 0 else 0.0
        win_rate = sum(1 for p in pnls if p > 0) / len(pnls)

        print(f"\n=== Метрики ансамбля ===")
        print(f"Сделок:         {len(pnls)}")
        print(f"Total PnL:      {total_pnl:.2f}")
        print(f"Sharpe:         {sharpe:.3f}")
        print(f"Profit Factor:  {profit_factor:.3f}")
        print(f"Win rate:       {win_rate:.1%}")


if __name__ == "__main__":
    main()
