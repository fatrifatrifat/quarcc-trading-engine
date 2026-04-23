from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from enum import StrEnum
from typing import Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))


@dataclass
class Subscription:
    symbol: str
    period: Optional[str] = None


@dataclass
class MarketData:
    class Feed(StrEnum):
        SIMULATED = "simulated"
        ALPACA = "alpaca"
        IBKR = "ibkr"

    feed: Feed
    subscriptions: list[Subscription] = field(default_factory=list)


@dataclass
class AdapterConfig:
    """Only needed when gateway == "grpc_adapter"."""

    venue: str
    credentials_path: str
    port: int


@dataclass
class StrategyConfig:
    class Gateway(StrEnum):
        GRPC_ADAPTER = "grpc_adapter"
        PAPER_TRADING = "paper trading"
        # BACKTESTING = "backtest"

    strategy_id: str
    account_id: str
    # "paper trading", "alpaca", or "grpc_adapter"
    gateway: Gateway
    market_data: Optional[MarketData] = None
    adapter: Optional[AdapterConfig] = None

    def to_proto(self):
        """Convert to a RegisterStrategyRequest protobuf message."""
        from gen.python.contracts import execution_service_pb2

        req = execution_service_pb2.RegisterStrategyRequest()
        req.strategy_id = self.strategy_id
        req.account_id = self.account_id
        req.gateway = self.gateway

        if self.adapter is not None:
            req.adapter.venue = self.adapter.venue
            req.adapter.credentials_path = self.adapter.credentials_path
            req.adapter.port = self.adapter.port

        if self.market_data is not None:
            req.market_data.feed = self.market_data.feed
            for sub in self.market_data.subscriptions:
                s = req.market_data.subscriptions.add()
                s.symbol = sub.symbol
                if sub.period:
                    s.period = sub.period

        return req
