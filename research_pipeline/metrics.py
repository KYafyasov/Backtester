import numpy as np


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

    result["num_fills"] = len(fills_df)
    result["total_volume"] = fills_df["quantity"].sum() if len(fills_df) else 0

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
