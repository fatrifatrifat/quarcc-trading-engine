import os
import sys

# Add generated code to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import logging
from typing import Optional

import grpc

from gen.python.contracts import (
    common_pb2,
    execution_service_pb2,
    execution_service_pb2_grpc,
    strategy_signal_pb2,
)

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
        confidence: float = 1.0,
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
        signal.confidence = confidence

        # Set timestamp
        from datetime import datetime

        signal.generated_at = datetime.now().isoformat()

        try:
            # Submit via gRPC
            response = self.stub.SubmitSignal(signal)

            if response.accepted:
                self.logger.info(f"✓ Signal accepted! Order ID: {response.order_id}")
                return response.order_id
            else:
                self.logger.warning(f"✗ Signal rejected: {response.rejection_reason}")
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
                self.logger.info(f"✓ Cancel signal accepted!")
            else:
                self.logger.warning(
                    f"✗ Cancel signal rejected: {response.rejection_reason}"
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
        confidence: float = 1.0,
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
        signal.confidence = confidence
        signal.order_id = order_id

        # set timestamp
        from datetime import datetime

        signal.generated_at = datetime.now().isoformat()

        try:
            # submit via grpc
            response = self.stub.ReplaceOrder(signal)

            if response.accepted:
                self.logger.info(
                    f"✓ Replace signal accepted! Order ID: {response.order_id}"
                )
                return response.order_id
            else:
                self.logger.warning(
                    f"✗ Replace signal rejected: {response.rejection_reason}"
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
                    "unrealized_pnl": p.unrealized_pnl,
                    "realized_pnl": p.realized_pnl,
                }
                for p in position_list.positions
            ]
        except grpc.RpcError as e:
            self.logger.error(f"Failed to get positions: {e}")
            return []

    def activate_kill_switch(self, reason: str, initiated_by: str):
        """Emergency: Stop all trading"""
        request = execution_service_pb2.KillSwitchRequest(
            reason=reason, initiated_by=initiated_by
        )

        try:
            self.stub.ActivateKillSwitch(request)
            self.logger.error(f"⚠️  KILL SWITCH ACTIVATED: {reason}")
        except grpc.RpcError as e:
            self.logger.error(f"Failed to activate kill switch: {e}")

    def close(self):
        """Close connection"""
        self.channel.close()


# Example usage
if __name__ == "__main__":
    # Create client
    client = ExecutionClient("localhost:50051")

    # Submit a buy signal
    for i in range(5):
        print("\n--- Submitting BUY signal ---")
        order_id = client.submit_signal(
            strategy_id="mean_reversion_1",
            symbol="ACDC",
            side="BUY",
            quantity=1.5,
            confidence=0.85,
        )

        # if order_id:
        #     print(f"Order submitted: {order_id}")

        #     import time

        #     time.sleep(2)

        #     print("\n--- Submitting REPLACE signal ---")
        #     order_id = client.replace_order(
        #         strategy_id="SMA_CROSS_v1.0",
        #         order_id=order_id,
        #         symbol="ACDC",
        #         side="BUY",
        #         quantity=1,
        #         confidence=0.85,
        #     )

        #     if order_id:
        #         print(f"Order replaced: {order_id}")

        #         print("\n--- Submitting CANCEL signal ---")
        #         client.cancel_signal(
        #             strategy_id="SMA_CROSS_v1.0",
        #             order_id=order_id,
        #         )

    import time

    time.sleep(0.5)

    # Get all positions
    print("\n--- All positions ---")
    positions = client.get_all_positions()
    print(positions)
    for pos in positions:
        print(f"{pos['symbol']}: {pos['quantity']} shares @ ${pos['avg_price']:.2f}")

    # Close
    client.close()
