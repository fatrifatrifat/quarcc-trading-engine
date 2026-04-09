"""
adapter.py CLI entry-point for all venue adapters

Spawned by AdapterManager (C++ engine) as:
    python3 python_client/adapters/adapter.py \
        --port <N>          \
        --credentials <path>\
        --venue <venue_name>

The --venue flag selects which adapter class to instantiate.
"""

import argparse
import logging
import os
import signal
import sys

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("adapter")

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "../..")))

VENUE_REGISTRY: dict[str, type] = {}

def _register_venues() -> None:
    # Add future venues here as follows:
    # try:
    #     from python_client.adapters.paper_adapter import PaperAdapter
    #     VENUE_REGISTRY["paper"] = PaperAdapter
    #     from python_client.adapters.ibkr_adapter import IbkrAdapter
    #     VENUE_REGISTRY["ibkr"] = IbkrAdapter
    # except ImportError as exc:
    #     logger.warning("ibkr adapter unavailable: %s", exc)

def main() -> None:
    parser = argparse.ArgumentParser(description="Quarcc gateway adapter process")
    parser.add_argument("--port",        type=int, required=True, help="gRPC listen port")
    parser.add_argument("--venue",       type=str, required=True, help="Venue name (paper, ibkr, ...)")
    parser.add_argument("--credentials", type=str, default="",    help="Path to credentials file")
    args = parser.parse_args()

    _register_venues()

    venue = args.venue.lower()
    adapter_cls = VENUE_REGISTRY.get(venue)
    if adapter_cls is None:
        available = ", ".join(VENUE_REGISTRY) or "(none)"
        logger.error("Unknown venue '%s'. Available: %s", venue, available)
        sys.exit(1)

    logger.info("Starting adapter: venue=%s port=%d", venue, args.port)

    adapter = adapter_cls(port=args.port)

    def _handle_signal(signum, frame):
        logger.info("Received signal %d. Stopping adapter", signum)
        adapter.stop()

    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    adapter.run()
    logger.info("Adapter exited cleanly")

if __name__ == "__main__":
    main()
