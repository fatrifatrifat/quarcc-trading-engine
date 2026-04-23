import os
import sys

# Add generated code to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import logging
from typing import TYPE_CHECKING, Callable, Optional

import grpc

from gen.python.contracts import (
    common_pb2,
    execution_service_pb2,
    execution_service_pb2_grpc,
    market_data_pb2,
    strategy_signal_pb2,
)

if TYPE_CHECKING:
    from strategy.config import StrategyConfig

logging.basicConfig(level=logging.INFO)


class ExecutionClient:
    """Python client for C++ execution engine"""

    def __init__(self, server_address: str = "localhost:50051"):
        self.logger = logging.getLogger(__name__)
        self.channel = grpc.insecure_channel(server_address)
        self.stub = execution_service_pb2_grpc.ExecutionServiceStub(self.channel)
        self.logger.info(f"Connected to execution engine at {server_address}")

    def submit_signal(
        self,
        strategy_id: str,
        symbol: str,
        side: str,  # "BUY" or "SELL"
        quantity: float,
    ) -> Optional[str]:
        """
        Submit a trading signal.

        Returns:
            Order ID if accepted, None if rejected
        """
        # Create signal
        signal = strategy_signal_pb2.StrategySignal()
        signal.strategy_id = strategy_id
        signal.symbol = symbol
        signal.side = common_pb2.Side.BUY if side == "BUY" else common_pb2.Side.SELL
        signal.target_quantity = quantity

        # Set timestamp
        from datetime import datetime

        signal.generated_at = datetime.now().isoformat()

        try:
            # Submit via gRPC
            response = self.stub.SubmitSignal(signal)

            if response.accepted:
                self.logger.info(f"Signal accepted! Order ID: {response.order_id}")
                return response.order_id
            else:
                self.logger.warning(f"Signal rejected: {response.rejection_reason}")
                return None

        except grpc.RpcError as e:
            self.logger.error(f"gRPC error: {e.code()} - {e.details()}")
            return None

    def cancel_signal(
        self,
        strategy_id: str,
        order_id: str,
    ) -> Optional[str]:
        """
        Submit a cancel signal.

        returns:
            Nothing.
        """
        # create signal
        signal = strategy_signal_pb2.CancelSignal()
        signal.strategy_id = strategy_id
        signal.order_id = order_id

        # set timestamp
        from datetime import datetime

        signal.generated_at = datetime.now().isoformat()

        try:
            # submit via grpc
            response = self.stub.CancelOrder(signal)

            if response.accepted:
                self.logger.info(f"Cancel signal accepted!")
            else:
                self.logger.warning(
                    f"Cancel signal rejected: {response.rejection_reason}"
                )
                return None

        except grpc.RpcError as e:
            self.logger.error(f"gRPC error: {e.code()} - {e.details()}")
            return None

    def replace_order(
        self,
        order_id: str,
        strategy_id: str,
        symbol: str,
        side: str,  # "BUY" or "SELL"
        quantity: float,
    ) -> Optional[str]:
        """
        Submit a replace signal.

        returns:
            Order ID if accepted, None if rejected.
        """
        # create signal
        signal = strategy_signal_pb2.ReplaceSignal()
        signal.strategy_id = strategy_id
        signal.symbol = symbol
        signal.side = common_pb2.Side.BUY if side == "BUY" else common_pb2.Side.SELL
        signal.target_quantity = quantity
        signal.order_id = order_id

        # set timestamp
        from datetime import datetime

        signal.generated_at = datetime.now().isoformat()

        try:
            # submit via grpc
            response = self.stub.ReplaceOrder(signal)

            if response.accepted:
                self.logger.info(
                    f"Replace signal accepted! Order ID: {response.order_id}"
                )
                return response.order_id
            else:
                self.logger.warning(
                    f"Replace signal rejected: {response.rejection_reason}"
                )
                return None

        except grpc.RpcError as e:
            self.logger.error(f"gRPC error: {e.code()} - {e.details()}")
            return None

    def get_position(self, symbol: str):
        """Query position for a symbol"""
        request = execution_service_pb2.GetPositionRequest(symbol=symbol)

        try:
            position = self.stub.GetPosition(request)
            return {
                "symbol": position.symbol,
                "quantity": position.quantity,
                "avg_price": position.avg_price,
                # TODO: uPnL not implemented (yet?)
                "unrealized_pnl": position.unrealized_pnl,
                "realized_pnl": position.realized_pnl,
            }
        except grpc.RpcError as e:
            self.logger.error(f"Failed to get position: {e}")
            return None

    def get_all_positions(self):
        """Get all positions"""
        empty = common_pb2.Empty()

        try:
            position_list = self.stub.GetAllPositions(empty)
            return [
                {
                    "symbol": p.symbol,
                    "quantity": p.quantity,
                    "avg_price": p.avg_price,
                    # TODO: uPnL not implemented (yet?)
                    "unrealized_pnl": p.unrealized_pnl,
                    "realized_pnl": p.realized_pnl,
                }
                for p in position_list.positions
            ]
        except grpc.RpcError as e:
            self.logger.error(f"Failed to get positions: {e}")
            return []

    def activate_kill_switch(self, reason: str, initiated_by: str, strategy_id: str):
        """Stop strategy"""
        request = execution_service_pb2.KillSwitchRequest(
            reason=reason, initiated_by=initiated_by, strategy_id=strategy_id
        )

        try:
            self.stub.ActivateKillSwitch(request)
            self.logger.info(f"KILL SWITCH ACTIVATED: {reason}")
        except grpc.RpcError as e:
            self.logger.error(f"Failed to activate kill switch: {e}")

    def register_strategy(self, config: "StrategyConfig") -> bool:
        """
        Register a strategy dynamically with the engine (no config.yaml entry
        required).  Returns True if accepted, False if rejected
        """
        req = config.to_proto()
        try:
            response = self.stub.RegisterStrategy(req)
            if response.accepted:
                self.logger.info(
                    f"Strategy '{config.strategy_id}' registered successfully"
                )
                return True
            self.logger.error(
                f"Strategy '{config.strategy_id}' rejected: {response.rejection_reason}"
            )
            return False
        except grpc.RpcError as e:
            self.logger.error(
                f"gRPC error registering strategy: {e.code()} - {e.details()}"
            )
            return False

    def stream_fills(
        self,
        strategy_id: str,
        on_fill: Optional[Callable] = None,
    ) -> None:
        """
        Subscribe to the fill stream for ``strategy_id``

        Blocks until the stream ends.  *on_fill* receives a ``v1.ExecutionReport``
        protobuf message for every fill, partial fill, rejection, or cancellation
        """
        req = execution_service_pb2.SubscribeFillsRequest(strategy_id=strategy_id)
        try:
            for fill in self.stub.StreamFills(req):
                if on_fill:
                    on_fill(fill)
        except grpc.RpcError as e:
            self.logger.info(f"Fill stream ended: {e.code()} - {e.details()}")

    def stream_market_data(
        self,
        strategy_id: str,
        on_tick: Optional[Callable] = None,
        on_bar: Optional[Callable] = None,
    ) -> None:
        """
        Subscribe to the market data stream for ``strategy_id``.

        Blocks until the stream ends (server shutdown or client disconnect).

        ``on_tick()`` receives a TickEvent protobuf message.
        ``on_bar()``  receives a BarEvent  protobuf message.
        """
        req = market_data_pb2.SubscribeMarketDataRequest(strategy_id=strategy_id)
        try:
            for event in self.stub.StreamMarketData(req):
                which = event.WhichOneof("event")
                if which == "tick" and on_tick:
                    on_tick(event.tick)
                elif which == "bar" and on_bar:
                    on_bar(event.bar)
        except grpc.RpcError as e:
            self.logger.info(f"Market data stream ended: {e.code()} - {e.details()}")

    def close(self):
        """Close connection"""
        self.channel.close()
