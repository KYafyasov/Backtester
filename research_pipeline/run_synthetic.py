import back_tester as bt

DATA_PATH = "synthetic_signal_large.jsonl"

class NoOpStrategy(bt.Strategy):
    """Ничего не делает — просто проверяем, что движок читает файл и не падает."""
    def __init__(self):
        super().__init__()
        self.book_updates = 0
        self.trades = 0

    def on_book_update(self, update):
        self.book_updates += 1

    def on_trade(self, trade):
        self.trades += 1


def main():
    strategy = NoOpStrategy()
    result = bt.backtest.run(
        strategy,
        DATA_PATH,
        bt.DateRange(),
        bt.BacktestConfig(order_latency_ns=5, book_depth=1),
        [
            bt.InstrumentMeta(instrument_id=1, contract_multiplier=10),
            bt.InstrumentMeta(instrument_id=2, contract_multiplier=5),
        ],
    )
    print(f"book_updates={strategy.book_updates}")
    print(f"trades={strategy.trades}")
    print(f"fills={len(result.fills_df)}")
    print(f"final_pnl={result.pnl_series.iloc[-1] if len(result.pnl_series) else 'N/A'}")


if __name__ == "__main__":
    main()
