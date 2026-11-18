"""Command line entry point: ``arm-gateway --config config.toml``."""

from __future__ import annotations

import argparse
import asyncio
import logging
import logging.handlers
import sys

from .config import ConfigError, load_config
from .gateway import Gateway


def setup_logging(level: str, file: str | None) -> None:
    handlers: list[logging.Handler] = [logging.StreamHandler()]
    if file:
        handlers.append(
            logging.handlers.RotatingFileHandler(file, maxBytes=2_000_000, backupCount=3, encoding="utf-8")
        )
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=handlers,
    )
    # asyncua logs every session detail at INFO.
    logging.getLogger("asyncua").setLevel(logging.WARNING)


async def run(gateway: Gateway) -> None:
    await gateway.start()
    try:
        await asyncio.Event().wait()
    finally:
        await gateway.stop()


def main() -> None:
    parser = argparse.ArgumentParser(description="HTTP to OPC UA gateway for the arm controller")
    parser.add_argument("--config", default="config.toml", help="path of the TOML config file")
    args = parser.parse_args()
    try:
        config = load_config(args.config)
    except (OSError, ConfigError, ValueError, TypeError) as e:
        sys.exit(f"cannot load {args.config}: {e}")
    setup_logging(config.log_level, config.log_file)
    try:
        asyncio.run(run(Gateway(config)))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
