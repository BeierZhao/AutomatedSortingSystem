"""The gateway: OPC UA server, /scada HTTP server, ESP32 HTTP client.

Everything runs in one asyncio event loop:

- ``/scada/...`` requests from the ESP32 update report nodes.
- A periodic task reads position, joint angles and obstacle distance.
- A write of ``true`` to a ``Cmd/<Command>/Execute`` node starts the
  handshake of guide section 7.4.
"""

from __future__ import annotations

import asyncio
import json
import logging
import math
from datetime import datetime, timezone
from typing import Any

import aiohttp
from aiohttp import web
from asyncua import Server, ua
from asyncua.common.callback import CallbackType

from .config import Config
from .model import (
    ARM4DOF,
    REPLY_EMPTY,
    REPLY_OPERATION_DONE,
    REPLY_OPERATION_RESULT,
    SORTING,
    CommandDef,
    commands_for,
    nodes_for,
)

log = logging.getLogger("arm_gateway")

ROOT_FOLDER = "ArmGateway"
INT32_MAX = 2**31 - 1

# operation_result text -> result flag node
OPERATION_RESULT_FLAGS = {"success": "Done", "invalid": "Invalid", "rejected": "Rejected"}


class EspError(Exception):
    """The ESP32 could not be reached, timed out or gave an unusable answer."""


class BadReport(Exception):
    """A /scada request body that does not match the interface."""


def _is_number(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


class Gateway:
    def __init__(self, config: Config) -> None:
        self.cfg = config
        self.server: Server | None = None
        self.namespace_index: int | None = None
        self._node_ids: dict[str, ua.NodeId] = {}
        self._node_types: dict[str, ua.VariantType] = {}
        self._commands = commands_for(config.profile)
        self._execute_nodes: dict[str, CommandDef] = {c.path("Execute"): c for c in self._commands}
        self._busy: set[str] = set()
        self._tasks: set[asyncio.Task] = set()
        self._session: aiohttp.ClientSession | None = None
        self._http_runner: web.AppRunner | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._esp_online: bool | None = None
        self._heartbeat = 0
        self._result_seq = 0
        self._other_alarms: set[str] = set()

    # ------------------------------------------------------------------ life cycle

    async def start(self) -> None:
        self._loop = asyncio.get_running_loop()
        try:
            await self._start_opcua()
            # force_close: one connection per request. The ESP32 web server
            # does not keep connections alive reliably, and a stale pooled
            # connection would show up as a false CommFault.
            self._session = aiohttp.ClientSession(connector=aiohttp.TCPConnector(force_close=True))
            await self._start_http()
        except BaseException:
            await self.stop()
            raise
        self._spawn(self._poll_loop())
        self._spawn(self._heartbeat_loop())
        log.info(
            "gateway running: profile=%s, OPC UA %s (namespace index %d), HTTP %s:%d, ESP32 %s",
            self.cfg.profile,
            self.cfg.opcua_endpoint,
            self.namespace_index,
            self.cfg.http_host,
            self.cfg.http_port,
            self.cfg.esp32_base_url,
        )

    async def stop(self) -> None:
        tasks = list(self._tasks)
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        self._tasks.clear()
        if self._http_runner:
            await self._http_runner.cleanup()
            self._http_runner = None
        if self._session:
            await self._session.close()
            self._session = None
        if self.server:
            await self.server.stop()
            self.server = None

    def _spawn(self, coro) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    # ------------------------------------------------------------------ OPC UA

    async def _start_opcua(self) -> None:
        server = Server()
        await server.init()
        server.set_endpoint(self.cfg.opcua_endpoint)
        server.set_server_name("Arm Gateway")
        server.set_security_policy([ua.SecurityPolicyType.NoSecurity])
        idx = await server.register_namespace(self.cfg.opcua_namespace_uri)
        self.namespace_index = idx

        root = await server.nodes.objects.add_folder(
            ua.NodeId(ROOT_FOLDER, idx), ua.QualifiedName(ROOT_FOLDER, idx)
        )
        folders = {"": root}
        for node in nodes_for(self.cfg.profile, self.cfg.alarms):
            parts = node.path.split("/")
            # Folders use the path prefix as their string node ID, e.g. "Cmd/SetArmPosition".
            for depth in range(1, len(parts)):
                prefix = "/".join(parts[:depth])
                if prefix not in folders:
                    parent = folders["/".join(parts[: depth - 1])]
                    folders[prefix] = await parent.add_folder(
                        ua.NodeId(prefix, idx), ua.QualifiedName(parts[depth - 1], idx)
                    )
            node_id = ua.NodeId(node.path, idx)
            var = await folders["/".join(parts[:-1])].add_variable(
                node_id,
                ua.QualifiedName(parts[-1], idx),
                ua.Variant(node.initial, node.vtype),
                varianttype=node.vtype,
                datatype=ua.NodeId(int(node.vtype), 0),
            )
            if node.writable:
                await var.set_writable()
            self._node_ids[node.path] = node_id
            self._node_types[node.path] = node.vtype

        server.subscribe_server_callback(CallbackType.PostWrite, self._on_post_write)
        await server.start()
        self.server = server

    async def set_node(self, path: str, value: Any) -> None:
        now = datetime.now(timezone.utc)
        await self.server.write_attribute_value(
            self._node_ids[path],
            ua.DataValue(
                ua.Variant(value, self._node_types[path]),
                SourceTimestamp=now,
                ServerTimestamp=now,
            ),
        )

    async def get_node(self, path: str) -> Any:
        return await self.server.get_node(self._node_ids[path]).read_value()

    def _on_post_write(self, event, _dispatcher) -> None:
        """Runs after every client write. Starts a command on Execute = true."""
        try:
            writes = event.request_params.NodesToWrite
            results = event.response_params or []
            for write, status in zip(writes, results):
                if write.AttributeId != ua.AttributeIds.Value or not status.is_good():
                    continue
                if write.NodeId.NamespaceIndex != self.namespace_index:
                    continue
                cmd = self._execute_nodes.get(write.NodeId.Identifier)
                if cmd is None or write.Value.Value.Value is not True:
                    continue
                self._loop.call_soon_threadsafe(self._start_command, cmd)
        except Exception:
            log.exception("error in OPC UA write callback")

    # ------------------------------------------------------------------ commands

    def _start_command(self, cmd: CommandDef) -> None:
        if cmd.name in self._busy:
            # Guide 7.4: a write to Execute while Busy is ignored.
            log.info("%s: Execute ignored, command is busy", cmd.name)
            return
        self._busy.add(cmd.name)
        self._spawn(self._run_command(cmd))

    async def _run_command(self, cmd: CommandDef) -> None:
        try:
            for flag in cmd.flags:
                await self.set_node(cmd.path(flag), False)
            await self.set_node(cmd.path("Busy"), True)
            flag = await self._execute(cmd)
            await self.set_node(cmd.path(flag), True)
            log.info("%s: %s", cmd.name, flag)
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("%s: internal error", cmd.name)
        finally:
            self._busy.discard(cmd.name)
            if self.server is not None:
                try:
                    await self.set_node(cmd.path("Busy"), False)
                    await self.set_node(cmd.path("Execute"), False)
                except Exception:
                    log.exception("%s: could not reset Busy/Execute", cmd.name)

    async def _execute(self, cmd: CommandDef) -> str:
        """Sends the request and returns the name of the one result flag to set."""
        if cmd.body_from_json_node:
            text = await self.get_node(cmd.path(cmd.params[0].node))
            try:
                body = json.loads(text or "")
            except ValueError:
                body = None
            if not isinstance(body, dict):
                # Nothing is sent: the config was not saved.
                log.warning("%s: %s is not a JSON object", cmd.name, cmd.params[0].node)
                return "Failed"
        else:
            body = {}
            for p in cmd.params:
                value = await self.get_node(cmd.path(p.node))
                if isinstance(value, float) and not math.isfinite(value):
                    # NaN and infinity cannot be sent as JSON.
                    log.warning("%s: %s is not a finite number", cmd.name, p.node)
                    return "Invalid" if "Invalid" in cmd.flags else "CommFault"
                body[p.field] = value

        timeout = self.cfg.motion_timeout_s if cmd.motion else self.cfg.request_timeout_s
        log.info("%s: POST %s %s", cmd.name, cmd.endpoint, json.dumps(body))
        try:
            reply = await self._post(cmd.endpoint, body, timeout)
        except EspError as e:
            log.warning("%s: %s", cmd.name, e)
            return "CommFault"

        if cmd.reply == REPLY_EMPTY:
            return "Done"
        if cmd.reply == REPLY_OPERATION_DONE:
            done = reply.get("operation_done")
            if isinstance(done, bool):
                return "Done" if done else "Failed"
        elif cmd.reply == REPLY_OPERATION_RESULT:
            flag = OPERATION_RESULT_FLAGS.get(reply.get("operation_result"))
            if flag in cmd.flags:
                return flag
        log.warning("%s: unexpected answer %s", cmd.name, reply)
        return "CommFault"

    # ------------------------------------------------------------------ ESP32 client

    async def _post(self, endpoint: str, body: dict, timeout_s: float) -> dict:
        url = self.cfg.esp32_base_url + endpoint
        try:
            async with self._session.post(
                url, json=body, timeout=aiohttp.ClientTimeout(total=timeout_s)
            ) as resp:
                text = await resp.text()
                if resp.status != 200:
                    raise EspError(f"{endpoint}: HTTP {resp.status} {text[:200]}")
        except asyncio.TimeoutError:
            raise EspError(f"{endpoint}: no answer within {timeout_s:g} s") from None
        except aiohttp.ClientError as e:
            raise EspError(f"{endpoint}: {e}") from None
        try:
            data = json.loads(text) if text.strip() else {}
        except ValueError:
            raise EspError(f"{endpoint}: answer is not JSON: {text[:200]}") from None
        if not isinstance(data, dict):
            raise EspError(f"{endpoint}: answer is not a JSON object")
        return data

    async def _read_position(self) -> None:
        data = await self._post("/controller/get-current-arm-position", {}, self.cfg.request_timeout_s)
        if not all(_is_number(data.get(k)) for k in ("x", "y", "z")):
            raise EspError(f"get-current-arm-position: unexpected answer {data}")
        for axis in ("x", "y", "z"):
            await self.set_node(f"Arm/{axis.upper()}", float(data[axis]))

    async def _read_joints(self) -> None:
        data = await self._post("/controller/get-current-joint-position", {}, self.cfg.request_timeout_s)
        angles = data.get("angles")
        if not isinstance(angles, list) or len(angles) < 4 or not all(_is_number(a) for a in angles):
            raise EspError(f"get-current-joint-position: unexpected answer {data}")
        for i in range(4):  # a fifth value (gripper) would be ignored
            await self.set_node(f"Arm/Joint{i + 1}", float(angles[i]))

    async def _read_distance(self) -> None:
        data = await self._post("/controller/get-obstacle-distance", {}, self.cfg.request_timeout_s)
        distance = data.get("distance")
        if not _is_number(distance) or not -1 <= distance <= INT32_MAX:
            raise EspError(f"get-obstacle-distance: unexpected answer {data}")
        await self.set_node("Arm/ObstacleDistance", int(round(distance)))

    async def poll_once(self) -> None:
        reads = [self._read_position(), self._read_joints()]
        if self.cfg.profile == ARM4DOF:
            reads.append(self._read_distance())
        results = await asyncio.gather(*reads, return_exceptions=True)
        errors = [r for r in results if isinstance(r, BaseException)]
        for e in errors:
            if not isinstance(e, EspError):
                raise e
        online = not errors
        if online != self._esp_online:
            if online:
                log.info("ESP32 online")
            else:
                log.warning("ESP32 offline: %s", errors[0])
            self._esp_online = online
        # On failure the Arm/ nodes keep their last values.
        await self.set_node("Comm/EspOnline", online)

    async def _poll_loop(self) -> None:
        loop = asyncio.get_running_loop()
        next_time = loop.time()
        while True:
            try:
                await self.poll_once()
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("periodic read failed")
            next_time += self.cfg.poll_interval_s
            delay = next_time - loop.time()
            if delay < 0:  # the read took longer than the interval
                next_time, delay = loop.time(), 0
            await asyncio.sleep(delay)

    async def _heartbeat_loop(self) -> None:
        while True:
            await asyncio.sleep(self.cfg.heartbeat_interval_s)
            self._heartbeat = 0 if self._heartbeat >= INT32_MAX else self._heartbeat + 1
            try:
                await self.set_node("Comm/Heartbeat", self._heartbeat)
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("heartbeat write failed")

    # ------------------------------------------------------------------ /scada HTTP server

    async def _start_http(self) -> None:
        app = web.Application(middlewares=[self._json_errors])
        app.router.add_post("/scada/report-current-status", self._handle(self._report_status))
        app.router.add_post("/scada/report-alarm", self._handle(self._report_alarm))
        if self.cfg.profile == SORTING:
            app.router.add_post(
                "/scada/report-sorting-result", self._handle(self._report_sorting_result)
            )
        self._http_runner = web.AppRunner(app, access_log=None)
        await self._http_runner.setup()
        await web.TCPSite(self._http_runner, self.cfg.http_host, self.cfg.http_port).start()

    @web.middleware
    async def _json_errors(self, request: web.Request, handler):
        """Every error (unknown path, wrong method, crash) gets a JSON body."""
        try:
            return await handler(request)
        except web.HTTPException as e:
            return web.json_response({"error": e.reason}, status=e.status)
        except Exception:
            log.exception("error handling %s", request.path)
            return web.json_response({"error": "internal error"}, status=500)

    def _handle(self, update):
        async def handler(request: web.Request) -> web.Response:
            raw = await request.read()
            try:
                try:
                    body = json.loads(raw.decode("utf-8"))
                except ValueError:
                    raise BadReport("malformed JSON") from None
                if not isinstance(body, dict):
                    raise BadReport("body must be a JSON object")
                await update(body)
            except BadReport as e:
                log.warning("%s rejected (%s): %r", request.path, e, raw[:200])
                return web.json_response({"error": str(e)}, status=400)
            log.info("%s %s", request.path, json.dumps(body))
            await self.set_node("Comm/LastReportTime", datetime.now(timezone.utc))
            return web.json_response({})

        return handler

    @staticmethod
    def _field(body: dict, name: str, kind: type):
        value = body.get(name)
        if not isinstance(value, kind):
            raise BadReport(f"{name} ({'boolean' if kind is bool else 'string'}) required")
        return value

    async def _report_status(self, body: dict) -> None:
        # Any string is accepted: the ESP32 retries a refused report for ever,
        # so a status this gateway does not know must not block its queue.
        await self.set_node("Status/Current", self._field(body, "status", str))

    async def _report_alarm(self, body: dict) -> None:
        message = self._field(body, "alarm_message", str)
        active = self._field(body, "alarm_status", bool)
        node = self.cfg.alarms.get(message)
        if node is not None:
            await self.set_node(node, active)
            return
        if active:
            self._other_alarms.add(message)
            await self.set_node("Alarms/OtherMessage", message)
        else:
            self._other_alarms.discard(message)
        await self.set_node("Alarms/OtherActive", bool(self._other_alarms))

    async def _report_sorting_result(self, body: dict) -> None:
        fields = {
            "Result/ReadingResult": self._field(body, "reading_result", str),
            "Result/Origin": self._field(body, "origin", str),
            "Result/Destination": self._field(body, "destination", str),
            "Result/PartId": self._field(body, "part_id", str),
            "Result/Repository": self._field(body, "repository", str),
            "Result/SortingResult": self._field(body, "sorting_result", bool),
        }
        for path, value in fields.items():
            await self.set_node(path, value)
        # Seq is written last, so a script triggered by it sees the new fields.
        self._result_seq = 1 if self._result_seq >= INT32_MAX else self._result_seq + 1
        await self.set_node("Result/Seq", self._result_seq)
