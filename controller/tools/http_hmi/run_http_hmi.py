"""HTTP test panel for the ESP32 arm controller, without Ignition or the gateway.

One process, Python standard library only:

- serves the panel (index.html, app.js) to the browser,
- receives the /scada/... reports the ESP32 pushes (guide section 5.2), the
  same way the gateway does, and keeps them for the panel,
- forwards the panel's /controller/... calls to the ESP32 (guide section 5.1),
  so the browser needs no CORS support from the board.

    python run_http_hmi.py                          ESP32 at the default address
    python run_http_hmi.py --esp32 http://192.168.1.50
    python run_http_hmi.py --port 8080 --no-browser

Point the firmware's gateway setting (NET_SCADA_HOST / NET_SCADA_PORT, or
"Gateway host/port" in the serial panel) at this PC and --port. Do not run
the real gateway on the same port at the same time.
"""
from __future__ import annotations

import argparse
import collections
import json
import math
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
import webbrowser
from datetime import datetime
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DEFAULT_PORT = 8080
DEFAULT_ESP32 = "http://192.168.1.50"
MAX_BODY = 64 * 1024
MAX_EVENTS = 500
HERE = os.path.dirname(os.path.abspath(__file__))


class BadReport(Exception):
    pass


def _field(body: dict, name: str, kind: type):
    value = body.get(name)
    if not isinstance(value, kind) or (kind is str and isinstance(value, bool)):
        raise BadReport(f"{name} ({'boolean' if kind is bool else 'string'}) required")
    return value


class State:
    """What the ESP32 has reported, plus an event log the panel polls."""

    def __init__(self, esp32_url: str) -> None:
        self.lock = threading.Lock()
        self.esp32_url = esp32_url.rstrip("/")
        self.status: str | None = None
        self.status_time: str | None = None
        self.alarms: dict[str, str] = {}  # active alarm message -> time raised
        self.results: collections.deque = collections.deque(maxlen=50)
        self.events: collections.deque = collections.deque(maxlen=MAX_EVENTS)
        self.seq = 0
        self.last_report: str | None = None
        self.last_report_from: str | None = None
        self.report_count = 0

    def add_event(self, kind: str, text: str, data=None) -> None:
        """Caller holds the lock."""
        self.seq += 1
        self.events.append(
            {"seq": self.seq, "time": _now(), "kind": kind, "text": text, "data": data}
        )

    def handle_report(self, path: str, body: dict, client: str) -> None:
        with self.lock:
            if path == "/scada/report-current-status":
                status = _field(body, "status", str)
                self.status, self.status_time = status, _now()
                self.add_event("status", f"status = {status}", body)
            elif path == "/scada/report-alarm":
                message = _field(body, "alarm_message", str)
                active = _field(body, "alarm_status", bool)
                if active:
                    self.alarms.setdefault(message, _now())
                else:
                    self.alarms.pop(message, None)
                self.add_event(
                    "alarm-on" if active else "alarm-off",
                    f"alarm {'RAISED' if active else 'cleared'}: {message}",
                    body,
                )
            elif path == "/scada/report-sorting-result":
                result = {
                    "reading_result": _field(body, "reading_result", str),
                    "origin": _field(body, "origin", str),
                    "destination": _field(body, "destination", str),
                    "part_id": _field(body, "part_id", str),
                    "repository": _field(body, "repository", str),
                    "sorting_result": _field(body, "sorting_result", bool),
                    "time": _now(),
                }
                self.results.appendleft(result)
                self.add_event(
                    "result",
                    f"sorting result: part {result['part_id']} -> {result['repository']} "
                    f"({'ok' if result['sorting_result'] else 'FAILED'})",
                    body,
                )
            else:
                raise KeyError(path)
            self.report_count += 1
            self.last_report = _now()
            self.last_report_from = client

    def snapshot(self, since: int) -> dict:
        with self.lock:
            return {
                "esp32_url": self.esp32_url,
                "status": self.status,
                "status_time": self.status_time,
                "alarms": [{"message": m, "since": t} for m, t in self.alarms.items()],
                "results": list(self.results),
                "last_report": self.last_report,
                "last_report_from": self.last_report_from,
                "report_count": self.report_count,
                "seq": self.seq,
                # since > seq means this server restarted: send everything.
                "events": [e for e in self.events if e["seq"] > since or since > self.seq],
            }


def _now() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def forward_to_esp32(base_url: str, endpoint: str, body: dict, timeout: float) -> dict:
    """POSTs to the ESP32 and describes the outcome for the panel."""
    url = base_url + endpoint
    data = json.dumps(body).encode()
    req = urllib.request.Request(
        url, data=data, method="POST", headers={"Content-Type": "application/json"}
    )
    t0 = time.monotonic()
    out: dict = {"url": url, "request": body}
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            out["http_status"] = resp.status
            text = resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        out["http_status"] = e.code
        text = e.read().decode("utf-8", "replace")
    except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError, OSError) as e:
        reason = getattr(e, "reason", e)
        if isinstance(reason, (socket.timeout, TimeoutError)):
            out["error"] = f"no answer within {timeout:g} s"
        else:
            out["error"] = str(reason)
        out["elapsed_ms"] = round((time.monotonic() - t0) * 1000)
        return out
    out["elapsed_ms"] = round((time.monotonic() - t0) * 1000)
    out["raw"] = text
    try:
        out["response"] = json.loads(text) if text.strip() else {}
    except ValueError:
        out["error"] = "answer is not JSON"
    return out


def make_handler(state: State, verbose: bool):
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=HERE, **kwargs)

        # ---------------------------------------------------------------- helpers

        def log_message(self, format, *args):
            if verbose:
                super().log_message(format, *args)

        def send_json(self, code: int, obj) -> None:
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def read_json(self):
            length = int(self.headers.get("Content-Length") or 0)
            if length > MAX_BODY:
                raise BadReport("body too large")
            raw = self.rfile.read(length) if length else b""
            try:
                body = json.loads(raw.decode("utf-8")) if raw.strip() else {}
            except ValueError:
                raise BadReport(f"malformed JSON: {raw[:200]!r}") from None
            if not isinstance(body, dict):
                raise BadReport("body must be a JSON object")
            return body

        # ---------------------------------------------------------------- routes

        def end_headers(self):
            # Static files: always fetch the latest version while editing.
            if not self.path.startswith("/api/"):
                self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def do_GET(self):
            url = urlparse(self.path)
            if url.path == "/api/state":
                try:
                    since = int(parse_qs(url.query).get("since", ["0"])[0])
                except ValueError:
                    since = 0
                self.send_json(200, state.snapshot(since))
                return
            if url.path in ("/", ""):
                self.path = "/index.html"
            elif url.path.startswith("/scada/"):
                self.send_json(405, {"error": "method not allowed"})
                return
            super().do_GET()

        def do_POST(self):
            path = urlparse(self.path).path
            try:
                if path.startswith("/scada/"):
                    self.handle_scada(path)
                elif path == "/api/esp32":
                    self.handle_forward()
                elif path == "/api/config":
                    self.handle_config()
                elif path == "/api/clear":
                    with state.lock:
                        state.results.clear()
                        state.alarms.clear()
                        state.add_event("system", "reports cleared from the panel")
                    self.send_json(200, {})
                else:
                    self.send_json(404, {"error": "unknown endpoint"})
            except BadReport as e:
                self.send_json(400, {"error": str(e)})

        def handle_scada(self, path: str) -> None:
            client = self.client_address[0]
            try:
                body = self.read_json()
                state.handle_report(path, body, client)
            except KeyError:
                self.send_json(404, {"error": "unknown endpoint"})
                return
            except BadReport as e:
                print(f"[{_now()}] {client} {path} REJECTED: {e}", flush=True)
                with state.lock:
                    state.add_event("bad", f"{path} rejected: {e}")
                raise
            print(f"[{_now()}] {client} {path} {json.dumps(body)}", flush=True)
            self.send_json(200, {})

        def handle_forward(self) -> None:
            req = self.read_json()
            endpoint = req.get("endpoint")
            body = req.get("body", {})
            timeout = req.get("timeout", 5)
            if not isinstance(endpoint, str) or not endpoint.startswith("/controller/"):
                raise BadReport("endpoint must start with /controller/")
            if not isinstance(body, dict):
                raise BadReport("body must be a JSON object")
            if (
                isinstance(timeout, bool)
                or not isinstance(timeout, (int, float))
                or not math.isfinite(timeout)
                or not 0.1 <= timeout <= 120
            ):
                raise BadReport("timeout must be 0.1 to 120 s")
            with state.lock:
                base = state.esp32_url
            result = forward_to_esp32(base, endpoint, body, float(timeout))
            if not req.get("quiet"):
                outcome = result.get("error") or result.get("raw", "")
                print(
                    f"[{_now()}] -> {endpoint} {json.dumps(body)} : "
                    f"{result.get('http_status', '-')} {outcome} ({result['elapsed_ms']} ms)",
                    flush=True,
                )
            self.send_json(200, result)

        def handle_config(self) -> None:
            body = self.read_json()
            url = body.get("esp32_url")
            if not isinstance(url, str) or not url.startswith(("http://", "https://")):
                raise BadReport("esp32_url must start with http://")
            with state.lock:
                state.esp32_url = url.strip().rstrip("/")
                state.add_event("system", f"ESP32 address set to {state.esp32_url}")
            print(f"[{_now()}] ESP32 address set to {url}", flush=True)
            self.send_json(200, {"esp32_url": url})

    return Handler


def local_ips() -> list[str]:
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    try:
        # The address of the interface that would reach the LAN.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.168.1.1", 1))
            ips.add(s.getsockname()[0])
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith("127."))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"port for the panel and the /scada reports (default {DEFAULT_PORT})")
    ap.add_argument("--host", default="0.0.0.0",
                    help="listen address (default 0.0.0.0, needed for the ESP32 to reach it)")
    ap.add_argument("--esp32", default=DEFAULT_ESP32,
                    help=f"ESP32 base URL (default {DEFAULT_ESP32}); can be changed in the panel")
    ap.add_argument("--no-browser", action="store_true", help="do not open the browser")
    ap.add_argument("--verbose", action="store_true", help="log every HTTP request")
    args = ap.parse_args()

    state = State(args.esp32)
    with state.lock:
        state.add_event("system", "panel server started")
    try:
        httpd = ThreadingHTTPServer((args.host, args.port), make_handler(state, args.verbose))
    except OSError as e:
        print(f"[!] Cannot listen on {args.host}:{args.port}: {e}")
        print("    Is the gateway or another panel already running? Try --port 8082.")
        sys.exit(1)
    httpd.daemon_threads = True

    url = f"http://localhost:{args.port}/"
    print("=" * 64)
    print(" Arm Controller HTTP Panel")
    print("=" * 64)
    print(f"[*] Panel:          {url}")
    print(f"[*] ESP32 (target): {args.esp32}")
    print(f"[*] /scada reports: POST to this PC, port {args.port}")
    for ip in local_ips():
        print(f"      e.g. gateway host {ip}, port {args.port}")
    print(" Allow the port through the Windows firewall so the ESP32 can reach it.")
    print(" Ctrl + C stops the server.")
    print("=" * 64, flush=True)

    if not args.no_browser:
        threading.Timer(1.0, webbrowser.open, args=(url,)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[*] Server stopped.")
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
