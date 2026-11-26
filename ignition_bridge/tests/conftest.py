"""Test environment: the gateway running against the stub ESP32, driven by
an asyncua client (in the role of Ignition) and an HTTP client (in the role
of the ESP32 pushing reports)."""

from __future__ import annotations

import asyncio
import logging
import socket
from typing import Any

import aiohttp
import pytest_asyncio
from asyncua import Client, ua

from arm_gateway.config import Config
from arm_gateway.gateway import Gateway
from arm_gateway.stub_esp32 import StubEsp32

logging.getLogger("asyncua").setLevel(logging.ERROR)

TYPES = {bool: ua.VariantType.Boolean, int: ua.VariantType.Int32, float: ua.VariantType.Double, str: ua.VariantType.String}


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Env:
    def __init__(self, profile: str, **overrides: Any) -> None:
        self.stub = StubEsp32("127.0.0.1", free_port())
        # Tests choose results explicitly instead of the stub's "auto" rules.
        self.stub.results = {name: "success" for name in self.stub.results}
        settings = dict(
            profile=profile,
            esp32_base_url=self.stub.base_url,
            poll_interval_s=0.1,
            heartbeat_interval_s=0.1,
            motion_timeout_s=1.0,
            request_timeout_s=0.5,
            http_host="127.0.0.1",
            http_port=free_port(),
            opcua_endpoint=f"opc.tcp://127.0.0.1:{free_port()}/arm-gateway",
        )
        settings.update(overrides)
        self.cfg = Config(**settings)
        self.gateway = Gateway(self.cfg)
        self.client = Client(self.cfg.opcua_endpoint)
        self.http: aiohttp.ClientSession | None = None
        self.ns: int | None = None

    async def start(self) -> None:
        await self.stub.start()
        await self.gateway.start()
        await self.client.connect()
        self.ns = await self.client.get_namespace_index(self.cfg.opcua_namespace_uri)
        self.http = aiohttp.ClientSession()

    async def stop(self) -> None:
        if self.http:
            await self.http.close()
        try:
            await self.client.disconnect()
        except Exception:
            pass
        await self.gateway.stop()
        await self.stub.stop()

    # --- OPC UA, as Ignition would use it

    def node(self, path: str):
        return self.client.get_node(ua.NodeId(path, self.ns))

    async def read(self, path: str) -> Any:
        return await self.node(path).read_value()

    async def write(self, path: str, value: Any) -> None:
        await self.node(path).write_value(ua.DataValue(ua.Variant(value, TYPES[type(value)])))

    async def wait_for(self, path: str, value: Any, timeout: float = 3.0) -> None:
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            actual = await self.read(path)
            if actual == value:
                return
            if asyncio.get_running_loop().time() > deadline:
                raise AssertionError(f"{path} is {actual!r}, expected {value!r} within {timeout} s")
            await asyncio.sleep(0.02)

    async def flags(self, command: str) -> dict[str, bool]:
        """The result flags of a command that are currently true or false."""
        result = {}
        for flag in ("Done", "Invalid", "Rejected", "Failed", "CommFault"):
            try:
                result[flag] = await self.read(f"Cmd/{command}/{flag}")
            except ua.UaStatusCodeError:
                pass  # this command does not have that flag
        return result

    async def execute(self, command: str, **params: Any) -> dict[str, bool]:
        """Runs the handshake of guide 7.4 and returns the flags afterwards."""
        for name, value in params.items():
            await self.write(f"Cmd/{command}/{name}", value)
        await self.write(f"Cmd/{command}/Execute", True)
        await self.wait_for(f"Cmd/{command}/Execute", False)
        assert await self.read(f"Cmd/{command}/Busy") is False
        return await self.flags(command)

    # --- HTTP, as the ESP32 would use it

    async def report(self, name: str, body: Any = None, raw: bytes | None = None) -> tuple[int, Any]:
        url = f"http://127.0.0.1:{self.cfg.http_port}/scada/{name}"
        kwargs = {"data": raw} if raw is not None else {"json": body}
        async with self.http.post(url, **kwargs) as resp:
            return resp.status, await resp.json()


def only(flags: dict[str, bool]) -> str:
    """Asserts exactly one result flag is set and returns its name."""
    on = [name for name, value in flags.items() if value]
    assert len(on) == 1, f"expected exactly one result flag, got {flags}"
    return on[0]


@pytest_asyncio.fixture
async def envs():
    """Factory for test environments. Everything it starts is stopped afterwards."""
    created: list[Env] = []

    async def factory(profile: str = "arm4dof", **overrides: Any) -> Env:
        env = Env(profile, **overrides)
        created.append(env)
        await env.start()
        return env

    yield factory
    for env in created:
        await env.stop()


@pytest_asyncio.fixture
async def arm(envs) -> Env:
    return await envs("arm4dof")


@pytest_asyncio.fixture
async def sorting(envs) -> Env:
    return await envs("sorting")
