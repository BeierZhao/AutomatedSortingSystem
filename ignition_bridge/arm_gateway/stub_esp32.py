"""A stand-in for the ESP32's /controller/... HTTP server.

Used by the tests (in-process) and from the command line to try the
gateway and the Ignition project without the arm:

    arm-gateway-stub --port 8081 --motion-delay 2 --gateway-url http://127.0.0.1:8080

It is not a simulation of the arm. It answers every endpoint of guide
section 5.1 after a configurable delay with a configurable result.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math

import aiohttp
from aiohttp import web

log = logging.getLogger("stub_esp32")

MOTION_ENDPOINTS = ("set-arm-position", "set-joint-position", "move-conveyor")
RESULTS = ("auto", "success", "invalid", "rejected")
MAX_REACH_MM = 295.0  # link 1 + link 2, a rough limit for "auto"


class StubEsp32:
    def __init__(self, host: str = "127.0.0.1", port: int = 8081, gateway_url: str | None = None) -> None:
        self.host = host
        self.port = port
        self.gateway_url = gateway_url.rstrip("/") if gateway_url else None

        # Behaviour, changeable at any time (tests set these directly,
        # the command line uses /stub/config).
        self.motion_delay = 0.0  # seconds before a successful motion answers
        self.read_delay = 0.0  # seconds before the read endpoints answer
        # "auto": rejected without permit or while moving, invalid when out of
        # reach, otherwise success. Any other value is returned as is.
        self.results = {name: "auto" for name in MOTION_ENDPOINTS}
        self.operation_done = True  # answer of update-position-config
        self.http_status: int | None = None  # force this status on every /controller call

        # State
        self.permit = False
        self.moving = False
        self.pose = {"x": 150.0, "y": 0.0, "z": 50.0}
        self.angles = [0.0, 67.0, 32.0, 0.0]
        self.distance = 183
        self.position_config: dict | None = None
        self.requests: list[tuple[str, dict]] = []  # every /controller call received

        self._runner: web.AppRunner | None = None

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}"

    def requests_to(self, name: str) -> list[dict]:
        return [body for endpoint, body in self.requests if endpoint == name]

    async def start(self) -> None:
        app = web.Application()
        app.router.add_post("/controller/{name}", self._controller)
        app.router.add_post("/stub/config", self._configure)
        self._runner = web.AppRunner(app, access_log=None)
        await self._runner.setup()
        await web.TCPSite(self._runner, self.host, self.port).start()

    async def stop(self) -> None:
        if self._runner:
            await self._runner.cleanup()
            self._runner = None

    # ------------------------------------------------------------------ handlers

    async def _controller(self, request: web.Request) -> web.Response:
        name = request.match_info["name"]
        text = await request.text()
        try:
            body = json.loads(text) if text.strip() else {}
        except ValueError:
            body = None
        if not isinstance(body, dict):
            return web.json_response({"error": "malformed JSON"}, status=400)
        self.requests.append((name, body))
        log.info("<- %s %s", name, json.dumps(body))

        if self.http_status is not None:
            return web.json_response({"error": "forced by stub"}, status=self.http_status)

        handler = getattr(self, "_do_" + name.replace("-", "_"), None)
        if handler is None:
            return web.json_response({"error": "unknown endpoint"}, status=404)
        reply = await handler(body)
        log.info("-> %s %s", name, json.dumps(reply))
        return web.json_response(reply)

    async def _configure(self, request: web.Request) -> web.Response:
        """POST /stub/config, for example {"motion_delay": 3, "results": {"set-arm-position": "invalid"}}."""
        try:
            body = await request.json()
            for key, value in body.items():
                if key == "results":
                    for name, result in value.items():
                        if name not in MOTION_ENDPOINTS or result not in RESULTS:
                            raise ValueError(f"bad result {name}={result}")
                        self.results[name] = result
                elif key in ("motion_delay", "read_delay"):
                    setattr(self, key, float(value))
                elif key == "operation_done":
                    self.operation_done = bool(value)
                elif key == "http_status":
                    self.http_status = None if value is None else int(value)
                elif key == "distance":
                    self.distance = int(value)
                else:
                    raise ValueError(f"unknown key {key}")
        except (ValueError, TypeError, AttributeError) as e:
            return web.json_response({"error": str(e)}, status=400)
        return web.json_response(
            {
                "motion_delay": self.motion_delay,
                "read_delay": self.read_delay,
                "results": self.results,
                "operation_done": self.operation_done,
                "http_status": self.http_status,
                "distance": self.distance,
            }
        )

    async def _motion(self, name: str, valid: bool, apply) -> dict:
        result = self.results[name]
        if result == "auto":
            if not self.permit or self.moving:
                result = "rejected"
            elif not valid:
                result = "invalid"
            else:
                result = "success"
        if result == "success":
            self.moving = True
            try:
                await asyncio.sleep(self.motion_delay)
            finally:
                self.moving = False
            apply()
        return {"operation_result": result}

    async def _do_set_arm_position(self, body: dict) -> dict:
        try:
            x, y, z = float(body["x"]), float(body["y"]), float(body["z"])
        except (KeyError, TypeError, ValueError):
            raise web.HTTPBadRequest(text='{"error":"x, y, z (numbers) required"}', content_type="application/json")
        valid = x >= 0 and math.sqrt(x * x + y * y + z * z) <= MAX_REACH_MM

        def apply() -> None:
            self.pose = {"x": x, "y": y, "z": z}
            self.angles[0] = round(math.degrees(math.atan2(y, x)), 1)

        return await self._motion("set-arm-position", valid, apply)

    async def _do_set_joint_position(self, body: dict) -> dict:
        joint, angle = body.get("joint_number"), body.get("angle")
        if not isinstance(joint, int) or isinstance(joint, bool) or not isinstance(angle, (int, float)):
            raise web.HTTPBadRequest(
                text='{"error":"joint_number (integer), angle (number) required"}',
                content_type="application/json",
            )
        valid = 1 <= joint <= 4 and -90 <= angle <= 110

        def apply() -> None:
            self.angles[joint - 1] = float(angle)

        return await self._motion("set-joint-position", valid, apply)

    async def _do_move_conveyor(self, body: dict) -> dict:
        return await self._motion("move-conveyor", True, lambda: None)

    async def _do_set_running_permit(self, body: dict) -> dict:
        if not isinstance(body.get("permit"), bool):
            raise web.HTTPBadRequest(text='{"error":"permit (boolean) required"}', content_type="application/json")
        self.permit = body["permit"]
        if self.gateway_url:
            status = "idle" if self.permit else "stop"
            asyncio.get_running_loop().create_task(self.push("report-current-status", {"status": status}))
        return {}

    async def _do_update_position_config(self, body: dict) -> dict:
        if self.operation_done:
            self.position_config = body
        return {"operation_done": self.operation_done}

    async def _do_get_current_arm_position(self, body: dict) -> dict:
        await asyncio.sleep(self.read_delay)
        return dict(self.pose)

    async def _do_get_current_joint_position(self, body: dict) -> dict:
        await asyncio.sleep(self.read_delay)
        return {"angles": list(self.angles)}

    async def _do_get_obstacle_distance(self, body: dict) -> dict:
        await asyncio.sleep(self.read_delay)
        return {"distance": self.distance}

    # ------------------------------------------------------------------ reports

    async def push(self, report: str, body: dict) -> int | None:
        """POSTs a /scada/... report to the gateway. Returns the HTTP status."""
        if not self.gateway_url:
            return None
        try:
            async with aiohttp.ClientSession() as session:
                async with session.post(
                    f"{self.gateway_url}/scada/{report}", json=body, timeout=aiohttp.ClientTimeout(total=2)
                ) as resp:
                    return resp.status
        except (aiohttp.ClientError, asyncio.TimeoutError) as e:
            log.warning("report %s not delivered: %s", report, e)
            return None


async def _run(stub: StubEsp32) -> None:
    await stub.start()
    log.info("stub ESP32 listening on %s", stub.base_url)
    if stub.gateway_url:
        await stub.push("report-current-status", {"status": "stop"})  # like the firmware at boot
    try:
        await asyncio.Event().wait()
    finally:
        await stub.stop()


def main() -> None:
    parser = argparse.ArgumentParser(description="Stub ESP32 /controller HTTP server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--motion-delay", type=float, default=2.0, help="seconds a successful motion takes")
    parser.add_argument(
        "--result",
        choices=RESULTS,
        default="auto",
        help="answer of the motion commands. auto = rejected without permit, invalid when out of reach",
    )
    parser.add_argument(
        "--gateway-url",
        help="gateway /scada address, e.g. http://127.0.0.1:8080. If set, the stub reports "
        '"stop" at start and "idle"/"stop" when the running permit changes',
    )
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    stub = StubEsp32(args.host, args.port, args.gateway_url)
    stub.motion_delay = args.motion_delay
    stub.results = {name: args.result for name in MOTION_ENDPOINTS}
    try:
        asyncio.run(_run(stub))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
