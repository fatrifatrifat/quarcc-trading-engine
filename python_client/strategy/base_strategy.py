from __future__ import annotations

import abc
import logging
import threading
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from strategy.config import StrategyConfig

logger = logging.getLogger(__name__)


class BaseStrategy(abc.ABC):
    def __init__(
        self,
        config: "StrategyConfig",
        server_address: str = "localhost:50051",
    ) -> None:
        import os
        import sys

        sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
        import grpc_interface

        self._config = config
        self._client = grpc_interface.ExecutionClient(server_address)
        self._threads: list[threading.Thread] = []

    def on_start(self) -> None:
        """Called once after the strategy is registered, before data arrives"""

    def on_stop(self) -> None:
        """Called when all data streams have ended"""

    def on_bar(self, bar) -> None:
        """Called for each BarEvent from the market data stream"""

    def on_tick(self, tick) -> None:
        """Called for each TickEvent from the market data stream"""

    def on_fill(self, fill) -> None:
        """
        Called for every ExecutionReport: fills, partial fills, rejections,
        and async cancellations.  ``fill`` is a ``v1.ExecutionReport`` proto
        """

    def buy(self, symbol: str, qty: float) -> Optional[str]:
        """Submit a BUY signal. Returns the local order ID or None on failure"""
        return self._client.submit_signal(self._config.strategy_id, symbol, "BUY", qty)

    def sell(self, symbol: str, qty: float) -> Optional[str]:
        """Submit a SELL signal. Returns the local order ID or None on failure"""
        return self._client.submit_signal(self._config.strategy_id, symbol, "SELL", qty)

    def cancel_order(self, order_id: str) -> None:
        """Cancel an open order."""
        self._client.cancel_signal(self._config.strategy_id, order_id)

    def get_position(self, symbol: str) -> Optional[dict]:
        """Return the aggregated position for *symbol* across all strategies"""
        return self._client.get_position(symbol)

    def get_all_positions(self) -> list[dict]:
        """Return all positions"""
        return self._client.get_all_positions()

    @property
    def strategy_id(self) -> str:
        return self._config.strategy_id

    @property
    def account_id(self) -> str:
        return self._config.account_id

    def run(self) -> None:
        """
        Register with the engine, start data + fill streams, then block

        Call ``stop()`` from another thread or a signal handler to exit
        """
        accepted = self._client.register_strategy(self._config)
        if not accepted:
            raise RuntimeError(f"Engine rejected strategy '{self._config.strategy_id}'")

        self.on_start()

        if self._config.market_data:
            t = threading.Thread(
                target=self._run_market_data_stream, daemon=True, name="md-stream"
            )
            t.start()
            self._threads.append(t)

        fill_t = threading.Thread(
            target=self._run_fill_stream, daemon=True, name="fill-stream"
        )
        fill_t.start()
        self._threads.append(fill_t)

        for t in self._threads:
            t.join()

        self.on_stop()
        self._client.close()

    def stop(self, reason="strategy stopped by user") -> None:
        """Signals the strategy to stop by killing the switch"""
        self._client.activate_kill_switch(
            reason=reason,
            initiated_by=self._config.strategy_id,
            strategy_id=self._config.strategy_id,
        )

    def _run_market_data_stream(self) -> None:
        self._client.stream_market_data(
            self._config.strategy_id,
            on_tick=self.on_tick,
            on_bar=self.on_bar,
        )

    def _run_fill_stream(self) -> None:
        self._client.stream_fills(
            self._config.strategy_id,
            on_fill=self.on_fill,
        )
