class TrackingMixin:
    """Общая логика учёта fills/realized_pnl для всех стратегий — подмешивается к bt.Strategy."""

    def _init_tracking(self, instrument_ids):
        self.instrument_ids = list(instrument_ids)
        self.book_updates = 0
        self.trades_seen = 0
        self.orders_sent = 0
        self.last_known_position = {i: None for i in self.instrument_ids}
        self.last_realized_pnl = {i: 0.0 for i in self.instrument_ids}
        self.trade_pnls = []

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
