import json
import random

def generate_synthetic_lob(
    output_path="synthetic.jsonl",
    n_instruments=2,
    n_events_per_instrument=2000,
    start_price=100,
    min_spread=2,
    ts_step_range=(5, 50),
    trade_probability=0.15,
    seed=42,
):
    random.seed(seed)
    events = []
    sequence = 1
    order_id_counter = 100
    ts_ns = 100

    state = {}
    for i in range(1, n_instruments + 1):
        state[i] = {
            "best_bid": start_price - min_spread,
            "best_ask": start_price + min_spread,
            "bid_order_id": None,
            "ask_order_id": None,
        }

    for instr_id in range(1, n_instruments + 1):
        order_id_counter += 1
        bid_id = order_id_counter
        state[instr_id]["bid_order_id"] = bid_id
        events.append({
            "ts_recv": f"1970-01-01T00:00:00.{ts_ns:09d}Z",
            "hd": {"ts_event": f"1970-01-01T00:00:00.{ts_ns:09d}Z", "instrument_id": instr_id},
            "action": "A", "side": "B",
            "price": str(state[instr_id]["best_bid"]),
            "size": random.randint(1, 10),
            "flags": 0, "sequence": sequence,
            "order_id": str(bid_id),
        })
        sequence += 1

        order_id_counter += 1
        ask_id = order_id_counter
        state[instr_id]["ask_order_id"] = ask_id
        events.append({
            "ts_recv": f"1970-01-01T00:00:00.{ts_ns:09d}Z",
            "hd": {"ts_event": f"1970-01-01T00:00:00.{ts_ns:09d}Z", "instrument_id": instr_id},
            "action": "A", "side": "A",
            "price": str(state[instr_id]["best_ask"]),
            "size": random.randint(1, 10),
            "flags": 128, "sequence": sequence,
            "order_id": str(ask_id),
        })
        sequence += 1

    total_events = n_events_per_instrument * n_instruments
    for _ in range(total_events):
        instr_id = random.randint(1, n_instruments)
        s = state[instr_id]
        ts_ns += random.randint(*ts_step_range)

        if random.random() < trade_probability:
            trade_side = random.choice(["B", "A"])
            trade_price = s["best_bid"] if trade_side == "B" else s["best_ask"]
            events.append({
                "ts_recv": f"1970-01-01T00:00:00.{ts_ns:09d}Z",
                "hd": {"ts_event": f"1970-01-01T00:00:00.{ts_ns:09d}Z", "instrument_id": instr_id},
                "action": "T", "side": trade_side,
                "price": str(trade_price),
                "size": random.randint(1, 5),
                "flags": 128,
                "sequence": sequence,
            })
            sequence += 1
            continue

        side = random.choice(["B", "A"])
        if side == "B":
            new_price = max(1, min(s["best_bid"] + random.randint(-2, 2), s["best_ask"] - min_spread))
            s["best_bid"] = new_price
            target_order_id = s["bid_order_id"]
        else:
            new_price = max(s["best_bid"] + min_spread, s["best_ask"] + random.randint(-2, 2))
            s["best_ask"] = new_price
            target_order_id = s["ask_order_id"]

        events.append({
            "ts_recv": f"1970-01-01T00:00:00.{ts_ns:09d}Z",
            "hd": {"ts_event": f"1970-01-01T00:00:00.{ts_ns:09d}Z", "instrument_id": instr_id},
            "action": "M", "side": side,
            "price": str(new_price),
            "size": random.randint(1, 10),
            "flags": 128,
            "sequence": sequence,
            "order_id": str(target_order_id),
        })
        sequence += 1

    events.sort(key=lambda e: (e["hd"]["ts_event"], e["hd"]["instrument_id"]))
    for i, e in enumerate(events, start=1):
        e["sequence"] = i

    with open(output_path, "w") as f:
        for e in events:
            f.write(json.dumps(e) + "\n")
    print(f"Написано {len(events)} событий в {output_path}")

if __name__ == "__main__":
    generate_synthetic_lob()
