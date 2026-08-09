import sys, os, json
sys.path.insert(0, "my_scripts/ml")
import numpy as np
from feature_extraction import replay_and_extract_features

MODEL_PATH = "my_scripts/ml/logistic_model.json"
DATA_PATH = "synthetic_signal.jsonl"

with open(MODEL_PATH) as f:
    model = json.load(f)
coef = np.array(model["coef"])
intercept = model["intercept"]
feature_names = model["features"]

dataset = replay_and_extract_features(DATA_PATH, horizon=20, imbalance_windows=(5, 20))
X = np.array([[row[c] for c in feature_names] for row in dataset])

z = X @ coef + intercept
proba = 1 / (1 + np.exp(-z))

print("proba stats:")
print("  min:", proba.min())
print("  max:", proba.max())
print("  mean:", proba.mean())
print("  median:", np.median(proba))
print("  std:", proba.std())
for q in [0.01, 0.05, 0.1, 0.5, 0.9, 0.95, 0.99]:
    print(f"  quantile {q}: {np.quantile(proba, q):.4f}")

for thr in [0.51, 0.52, 0.53, 0.55, 0.6, 0.65, 0.7]:
    frac_above = (proba > thr).mean()
    frac_below = (proba < (1-thr)).mean()
    print(f"threshold {thr}: frac_above={frac_above:.4f} frac_below={frac_below:.4f} total_signal_frac={frac_above+frac_below:.4f}")
