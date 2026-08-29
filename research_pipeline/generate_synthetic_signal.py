import json
import random
import sys

def generate_synthetic_with_signal(
    output_path="synthetic_signal.jsonl",
    n_instruments=2,
    total_events=1_000_000,
    start_price=100,
    min_spread=2,
    ts_step_range=(5, 50),
    trade_probability=0.15,
    signal_strength=0.65,   # вероятность, что imbalance следуют дрейфу, а не шумят
    regime_change_prob=0.01,  # вероятность смены "истинного дрейфа" на каждом шаге
    seed=42,
    progress_every=500_000,
):
    """Синтетика со встроенног предсказуемой зависимостью:
    imbalance (размер на активной стороне) коррелирует с будущим движением цены
    с силой signal_strength. Режим дрейфа меняется случайно, имитируя смену тренда.
    """
    random.seed(seed)
    sequence = 0
    order_id_counter = 100
    ts_ns = 100

    state = {}
    for i in range(1, n_instruments + 1):
        state[i] = {
            "best_bid": start_price - min_spread,
            "best_ask": start_price + min_spread,
            "bid_order_id": None,
            "ask_order_id": None,
            "drift": random.choice([1, -1]),  # 1 = дрейф вверх, -1 = вниз
        }

    with open(output_path, "w") as f:
        for instr_id in range(1, n_instruments + 1):
            sequence += 1
            order_id_counter += 1
            bid_id = order_id_counter
            state[instr_id]["bid_order_id"] = bid_id
            ts_str = f"1970-01-01T00:00:00.{ts_ns:09d}Z"
            f.write(json.dumps({
                "ts_recv": ts_str,
                "hd": {"ts_event": ts_str, "instrument_id": instr_id},
                "action": "A", "side": "B",
                "price": str(state[instr_id]["best_bid"]),
                "size": random.randint(1, 10),
                "flags": 0, "sequence": sequence,
                "order_id": str(bid_id),
            }) + "\n")

            sequence += 1
            order_id_counter += 1
            ask_id = order_id_counter
            state[instr_id]["ask_order_id"] = ask_id
            f.write(json.dumps({
                "ts_recv": ts_str,
                "hd": {"ts_event": ts_str, "instrument_id": instr_id},
                "action": "A", "side": "A",
                "price": str(state[instr_id]["best_ask"]),
                "size": random.randint(1, 10),
                "flags": 128, "sequence": sequence,
                "order_id": str(ask_id),
            }) + "\n")

        for step in range(total_events):
            instr_id = random.randint(1, n_instruments)
            s = state[instr_id]
            ts_ns += random.randint(*ts_step_range)
            sequence += 1
            ts_str = f"1970-01-01T00:00:00.{ts_ns:09d}Z"

            # редкая смена режима (истинного дрейфа)
            if random.random() < regime_change_prob:
                s["drift"] *= -1

            if random.random() < trade_probability:
                trade_side = "B" if s["drift"] > 0 else "A"
                if random.random() >= signal_strength:
                    trade_side = random.choice(["B", "A"])
                trade_price = s["best_bid"] if trade_side == "B" else s["best_ask"]
                f.write(json.dumps({
                    "ts_recv": ts_str,
                    "hd": {"ts_event": ts_str, "instrument_id": instr_id},
                    "action": "T", "side": trade_side,
                    "price": str(trade_price),
                    "size": random.randint(1, 5),
                    "flags": 128,
                    "sequence": sequence,
                }) + "\n")
                if step % progress_every == 0 and step > 0:
                    print(f"...{step}/{total_events} событий", file=sys.stderr)
                continue

            # signal-following side: с вероятностью signal_strength двигаем сторону,
            # согласованную с дрейфом (и даём ей больший размер => виден imbalance)
            follow_signal = random.random() < signal_strength
            if follow_signal:
                side = "B" if s["drift"] > 0 else "A"
                size = random.randint(8, 20)  # крупный размер на "сигнальной" стороне
            else:
                side = random.choice(["B", "A"])
                size = random.randint(1, 10)

            if side == "B":
                # при дрейфе вверх bid двигается сильнее вверх
                step_bias = random.randint(0, 3) if follow_signal and s["drift"] > 0 else random.randint(-2, 2)
                new_price = max(1, min(s["best_bid"] + step_bias, s["best_ask"] - min_spread))
                s["best_bid"] = new_price
                target_order_id = s["bid_order_id"]
            else:
                step_bias = random.randint(-3, 0) if follow_signal and s["drift"] < 0 else random.randint(-2, 2)
                new_price = max(s["best_bid"] + min_spread, s["best_ask"] + step_bias)
                s["best_ask"] = new_price
                target_order_id = s["ask_order_id"]

            f.write(json.dumps({
                "ts_recv": ts_str,
                "hd": {"ts_event": ts_str, "instrument_id": instr_id},
                "action": "M", "side": side,
                "price": str(new_price),
                "size": size,
                "flags": 128,
                "sequence": sequence,
                "order_id": str(target_order_id),
            }) + "\n")

            if step % progress_every == 0 and step > 0:
                print(f"...{step}/{total_events} событий", file=sys.stderr)

    print(f"Готово: {sequence} событий записано в {output_path} (signal_strength={signal_strength})")


if __name__ == "__main__":
    n_events = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
    strength = float(sys.argv[2]) if len(sys.argv) > 2 else 0.65
    generate_synthetic_with_signal(total_events=n_events, signal_strength=strength)
