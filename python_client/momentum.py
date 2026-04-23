from strategy.base_strategy import BaseStrategy
from strategy.config import MarketData, StrategyConfig, Subscription

counter = 0


class MomentumStrategy(BaseStrategy):
    def on_start(self):
        print("strategy started")

    def on_bar(self, bar):
        if bar.close > bar.open:
            self.buy(bar.symbol, qty=1.0)

    def on_tick(self, tick):
        global counter
        print(
            f"tick #{counter}: {tick.symbol}, bid: {tick.bid}, ask: {tick.ask}, ts: {tick.timestamp_ns}"
        )
        if tick.bid > 105:
            self.buy(tick.symbol, qty=1.0)
        counter += 1

    def on_fill(self, fill):
        print(f"fill: {fill.filled_quantity} @ {fill.avg_fill_price}")

    def on_stop(self):
        print("strategy stopped")


config = StrategyConfig(
    strategy_id="momentum_1",
    account_id="acct_001",
    gateway=StrategyConfig.Gateway.PAPER_TRADING,
    market_data=MarketData(
        MarketData.Feed.SIMULATED,
        [
            Subscription("AAPL", "1m"),
            Subscription("A", "1s"),
            Subscription("B", "5m"),
            Subscription("C", "5s"),
            Subscription("D", "1m"),
            Subscription("E", "1m"),
            Subscription("F", "1m"),
            Subscription("G", "1m"),
            Subscription("H", "1m"),
            Subscription("I", "1m"),
        ],
    ),
)
MomentumStrategy(config).run()
