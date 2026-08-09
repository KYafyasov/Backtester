import back_tester as bt
import numpy as np

DATA_PATH = "synthetic.jsonl"
IMBALANCE_THRESHOLD = 0.3
ORDER_SIZE = 1


class ImbalanceStrategy(bt.Strategy):
    def __init__(self, instrument_ids, threshold=IMBALANCE_THRESHOLD, order_size=ORDER_SIZE):
        super().__init__()
        self.threshold = threshold
        self.order_size = order_size
        self.instrument_ids = list(instrument_ids)
        self.book_updates = 0
        self.trades_seen = 0
        self.orders_sent = 0
        self.last_signal = {i: 0 for i in self.instrument_ids}
        self.last_known_position = {i: None for i in self.instrument_ids}
        self.last_realized_pnl = {i: 0.0 for i in self.instrument_ids}
        self.trade_pnls = []  # realized pnl delta per fill that closed/reduced position

    def on_book_update(self, update):
        self.book_updates += 1
        if not update.bids or not update.asks:
            return

        bid_qty = sum(level.quantity for level in update.bids)
        ask_qty = sum(level.quantity for level in update.asks)
        total = bid_qty + ask_qty
        if total == 0:
            return

        imbalance = (bid_qty - ask_qty) / total
        instrument_id = update.instrument_id
        best_bid_price = update.bids[0].price
        best_ask_price = update.asks[0].price

        pos = self.position(instrument_id)
        current_position = pos.net_quantity
        self.last_known_position[instrument_id] = pos

        if imbalance > self.threshold and current_position <= 0:
            self.submit_limit(instrument_id, bt.Side.BUY, best_ask_price, self.order_size)
            self.orders_sent += 1
            self.last_signal[instrument_id] = 1
        elif imbalance < -self.threshold and current_position >= 0:
            self.submit_limit(instrument_id, bt.Side.SELL, best_bid_price, self.order_size)
            self.orders_sent += 1
            self.last_signal[instrument_id] = -1

    def on_trade(self, trade):
        self.trades_seen += 1

    def on_fill(self, fill):
        instrument_id = fill.instrument_id
        pos = self.position(instrument_id)
        realized = pos.realized_pnl
        prev = self.last_realized_pnl.get(instrument_id, 0.0)
        delta = realized - prev
        if delta != 0:
            self.trade_pnls.append(delta)
        self.last_realized_pnl[instrument_id] = realized


def compute_metrics(pnl_series, fills_df, trade_pnls):
    """PnL, Max Drawdown, простой Sharpe, Win Rate, Turnover, Profit Factor, Avg PnL/trade."""
    result = {}

    if len(pnl_series) >= 1:
        result["final_pnl"] = pnl_series.iloc[-1]
    else:
        result["final_pnl"] = 0.0

    if len(pnl_series) >= 2:
        running_max = pnl_series.cummax()
        drawdown = pnl_series - running_max
        result["max_drawdown"] = drawdown.min()

        returns = pnl_series.diff().dropna()
        if returns.std() > 0:
            result["sharpe"] = returns.mean() / returns.std()
        else:
            result["sharpe"] = 0.0

        positive_steps = (returns > 0).sum()
        total_steps = (returns != 0).sum()
        result["win_rate"] = positive_steps / total_steps if total_steps > 0 else 0.0
    else:
        result["max_drawdown"] = 0.0
        result["sharpe"] = 0.0
        result["win_rate"] = 0.0

    # Turnover
    result["num_fills"] = len(fills_df)
    result["total_volume"] = fills_df["quantity"].sum() if len(fills_df) else 0

    # Profit Factor & Avg PnL per closing trade (по realized_pnl delta на каждом fill)
    trade_pnls_arr = np.array(trade_pnls)
    if len(trade_pnls_arr) > 0:
        gains = trade_pnls_arr[trade_pnls_arr > 0].sum()
        losses = -trade_pnls_arr[trade_pnls_arr < 0].sum()
        result["profit_factor"] = gains / losses if losses > 0 else (np.inf if gains > 0 else 0.0)
        result["avg_pnl_per_trade"] = trade_pnls_arr.mean()
        result["num_closing_trades"] = len(trade_pnls_arr)
    else:
        result["profit_factor"] = 0.0
        result["avg_pnl_per_trade"] = 0.0
        result["num_closing_trades"] = 0

    return result


def main():
    instrument_ids = [1, 2]
    strategy = ImbalanceStrategy(instrument_ids)
    result = bt.backtest.run(
        strategy,
        DATA_PATH,
        bt.DateRange(),
        bt.BacktestConfig(order_latency_ns=5, book_depth=5),
        [
            bt.InstrumentMeta(instrument_id=1, contract_multiplier=10),
            bt.InstrumentMeta(instrument_id=2, contract_multiplier=5),
        ],
    )

    print(f"book_updates={strategy.book_updates}")
    print(f"trades_seen={strategy.trades_seen}")
    print(f"orders_sent={strategy.orders_sent}")
    print(f"fills={len(result.fills_df)}")

    metrics = compute_metrics(result.pnl_series, result.fills_df, strategy.trade_pnls)
    print("--- Metrics ---")
    print(f"final_pnl={metrics['final_pnl']:.4f}")
    print(f"max_drawdown={metrics['max_drawdown']:.4f}")
    print(f"sharpe={metrics['sharpe']:.4f}")
    print(f"win_rate={metrics['win_rate']:.4f}")
    print(f"num_fills={metrics['num_fills']}")
    print(f"total_volume={metrics['total_volume']}")
    print(f"profit_factor={metrics['profit_factor']:.4f}")
    print(f"avg_pnl_per_trade={metrics['avg_pnl_per_trade']:.4f}")
    print(f"num_closing_trades={metrics['num_closing_trades']}")

    print("--- Positions ---")
    for iid in instrument_ids:
        pos = strategy.last_known_position[iid]
        if pos is not None:
            print(f"instrument {iid}: net_qty={pos.net_quantity} realized_pnl={pos.realized_pnl} unrealized_pnl={pos.unrealized_pnl}")
        else:
            print(f"instrument {iid}: no position data captured")


if __name__ == "__main__":
    main()
