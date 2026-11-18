# ignition_bridge: HTTP to OPC UA gateway

The gateway sits between the ESP32 arm controller and Ignition. Ignition never speaks HTTP.

```
              HTTP + JSON                        OPC UA
  ESP32  ──── /scada/... reports ────▶  gateway  ◀──────────▶  Ignition
         ◀─── /controller/... calls ──            (server)      (client)
```

It is one asyncio process that is:

- an HTTP server for the reports the ESP32 pushes (`/scada/report-current-status`, `/scada/report-alarm`, `/scada/report-sorting-result`),
- an HTTP client that forwards operator commands to `/controller/...` and reads position, joint angles and obstacle distance once per second,
- an OPC UA server exposing all of it as nodes.

The design, the HTTP API and the full node list are in [docs/development_guide.md](../docs/development_guide.md), sections 5 to 7. Section 7 is the IO list for the Ignition developer, and `tests/test_io_list.py` checks the running server against it.

Status: tested against the stub ESP32 in this folder only. It has not yet run against the real ESP32 or a real Ignition gateway.

## Contents

| Path | What |
| --- | --- |
| `arm_gateway/gateway.py` | The gateway: OPC UA server, `/scada` server, periodic read, command handshake |
| `arm_gateway/model.py` | The node list and the commands, as data |
| `arm_gateway/config.py` | Settings and their defaults |
| `arm_gateway/stub_esp32.py` | Stub ESP32 `/controller` server for testing |
| `config.example.toml` | Example settings. Copy to `config.toml` |
| `tests/` | pytest suite: gateway + stub ESP32 + an `asyncua` client in the role of Ignition |

## Install (Windows)

Needs Python 3.11 or later (`python --version`). In PowerShell, from this folder:

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -e ".[dev]"
```

This installs `asyncua`, `aiohttp`, the test tools, and two commands in `.venv\Scripts`: `arm-gateway` and `arm-gateway-stub`. Leave out `[dev]` on a machine that only runs the gateway.

## Configure

```powershell
Copy-Item config.example.toml config.toml
```

Then edit `config.toml` (it is git-ignored):

| Setting | Default | Meaning |
| --- | --- | --- |
| `profile` | `arm4dof` | `arm4dof` or `sorting`. The sorting profile adds the conveyor, position-config and `Result/` nodes, and drops `Arm/ObstacleDistance` |
| `[esp32] base_url` | `http://192.168.1.50:80` | Address of the ESP32. The port is `NET_CONTROLLER_HTTP_PORT` in the firmware |
| `[esp32] poll_interval_s` | `1.0` | Interval of the position, joint and distance reads |
| `[esp32] motion_timeout_s` | `30.0` | Timeout of `set-arm-position`, `set-joint-position` and `move-conveyor`, which answer only when the motion ends |
| `[esp32] request_timeout_s` | `2.0` | Timeout of every other call |
| `[http] host`, `port` | `0.0.0.0`, `8080` | Where the gateway listens for `/scada/...` reports |
| `[opcua] endpoint` | `opc.tcp://127.0.0.1:4840/arm-gateway` | OPC UA endpoint. Localhost only, because it has no security |
| `[opcua] namespace_uri` | `urn:group12:arm-gateway` | Namespace of the nodes |
| `[logging] level`, `file` | `INFO`, none | Log level, and an optional rotating log file |
| `[alarms]` | `"Obstacle detected" = "Alarms/Obstacle"` | Alarm table, see below |

On the ESP32 side, the firmware must push its reports to this machine: set `NET_SCADA_HOST` to this machine's IP address and `NET_SCADA_PORT` to the `[http] port` (in `controller/include/NetConfig.h`, or with the serial HMI panel). Give this machine and the ESP32 fixed addresses.

### Alarm table

Each line of `[alarms]` maps one `alarm_message` text to a Boolean node under `Alarms/`. The gateway creates a node for every entry and sets it to the `alarm_status` of each matching report. The text must match exactly, including case.

A message that is not in the table is an unmapped alarm: its text goes to `Alarms/OtherMessage`, and `Alarms/OtherActive` is true while at least one unmapped alarm is raised.

If the file has no `[alarms]` table, the `arm4dof` profile uses the single entry above and the `sorting` profile uses an empty table. To add an alarm, add a line, restart the gateway, and add the new node to the IO list in the guide.

## Run

```powershell
.venv\Scripts\arm-gateway --config config.toml
```

Stop it with Ctrl+C. At start it logs the OPC UA endpoint and the namespace index.

### Firewall

The ESP32 connects to the HTTP port from the network, so allow it in (PowerShell as Administrator):

```powershell
New-NetFirewallRule -DisplayName "Arm gateway HTTP 8080" -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow
```

The OPC UA port 4840 needs no rule, because Ignition connects through localhost. Neither port collides with Ignition's defaults (web 8088, its own OPC UA server 62541).

### Start at boot

Register a scheduled task that runs as SYSTEM at startup and restarts on failure (PowerShell as Administrator, with `$dir` set to this folder):

```powershell
$dir = "C:\path\to\AutomatedSortingSystem\ignition_bridge"
$action = New-ScheduledTaskAction -Execute "$dir\.venv\Scripts\arm-gateway.exe" -Argument "--config `"$dir\config.toml`"" -WorkingDirectory $dir
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName "ArmGateway" -Action $action -Trigger $trigger -Settings $settings -User "SYSTEM" -RunLevel Highest
```

A task has no console, so set `[logging] file` in `config.toml` to see what it does. Start it by hand with `Start-ScheduledTask ArmGateway`, and remove it with `Unregister-ScheduledTask ArmGateway`. If the gateway starts after Ignition, Ignition reconnects by itself.

## Connecting Ignition

1. In the Ignition Gateway web page, add an OPC UA connection (Config, OPC Client, OPC Connections) with endpoint URL `opc.tcp://127.0.0.1:4840/arm-gateway`.
2. Choose security policy **None** and message security mode **None**, with anonymous login (no user name). It is the only endpoint the gateway offers.
3. The connection should show "Connected". In the Designer's OPC browser the nodes are under `Objects/ArmGateway/`.

Node IDs are strings: `ns=2;s=<path>`, for example `ns=2;s=Arm/X` or `ns=2;s=Cmd/SetArmPosition/Execute`. Use that as the OPC Item Path of each tag.

**Namespace index caveat.** The `2` is the position of `urn:group12:arm-gateway` in the server's namespace array, and the tests confirm that the current `asyncua` version gives 2. It is not guaranteed by OPC UA. The gateway logs the index at start, and the browser shows it. If it is ever different, use that index in every item path.

Things to know when building the tags:

- Before the first report arrives, `Status/Current` is an empty string and `Comm/LastReportTime` is `1601-01-01 00:00:00 UTC`, the OPC UA minimum date, which stands for "no report yet". The ESP32 sends its status at boot and when it changes, so after a gateway restart the status stays empty until the next change.
- When the ESP32 is offline, the `Arm/` nodes keep their last values. Use `Comm/EspOnline` to mark them stale.
- Commands follow the handshake in guide section 7.4: write the parameters, write `Execute = true`, then watch `Busy` and the result flags. The flags stay until the next `Execute` of the same command.
- `CommFault` covers a connection error, a timeout, an HTTP status other than 200, and an answer the gateway does not understand.
- `Cmd/UpdatePositionConfig/Failed` is also set, without sending anything, when `ConfigJson` is not a JSON object.

## Testing without the ESP32

### Stub ESP32

`arm-gateway-stub` answers every `/controller/...` endpoint. It is not an arm simulation: a successful motion just waits and then reports the target as the new position.

```powershell
.venv\Scripts\arm-gateway-stub --port 8081 --motion-delay 2 --gateway-url http://127.0.0.1:8080
```

Point the gateway at it with `base_url = "http://127.0.0.1:8081"`.

| Option | Meaning |
| --- | --- |
| `--motion-delay` | Seconds a successful motion takes |
| `--result` | `auto` (default), `success`, `invalid` or `rejected` for all motion commands. `auto` answers `rejected` without a running permit or while another motion runs, `invalid` for a target out of reach, otherwise `success` |
| `--gateway-url` | If set, the stub reports `stop` at start and `idle` or `stop` when the running permit changes |

Its behaviour can be changed while it runs:

```powershell
curl.exe -X POST http://127.0.0.1:8081/stub/config -H "Content-Type: application/json" -d '{\"results\":{\"set-arm-position\":\"invalid\"},\"motion_delay\":5,\"distance\":80}'
```

Keys: `results` (per endpoint `set-arm-position`, `set-joint-position`, `move-conveyor`), `motion_delay`, `read_delay`, `operation_done`, `http_status` (force an HTTP error, `null` to clear), `distance`.

Reports can be sent to the gateway by hand, in the role of the ESP32:

```powershell
curl.exe -X POST http://127.0.0.1:8080/scada/report-alarm -H "Content-Type: application/json" -d '{\"alarm_message\":\"Obstacle detected\",\"alarm_status\":true}'
```

Any OPC UA client such as UaExpert can then be used in place of Ignition.

### Automated tests

```powershell
.venv\Scripts\python -m pytest
```

Each test starts the gateway and the stub on free localhost ports and drives them with an `asyncua` client and an HTTP client. The suite takes a few minutes, because every test starts its own OPC UA server. It covers each report endpoint and malformed bodies, the alarm table including unmapped messages, every command with success, invalid, rejected, failed and comm fault (ESP32 down, timeout, non-200), the `Busy` and latching rules, `Result/Seq`, `Comm/EspOnline`, `Comm/Heartbeat`, and the address space against the IO list in the guide.

## Behaviour notes

- `/scada` requests with malformed JSON, a missing field or a wrongly typed field get HTTP 400 and `{"error": "..."}`. An unknown path gets 404 and a wrong method 405, also with a JSON error body. Unknown fields are ignored.
- Any string is accepted as `status`, because the firmware retries a refused report for ever and would block its report queue.
- `/scada/report-sorting-result` exists only in the `sorting` profile.
- `Comm/EspOnline` is true only if every read of the last cycle succeeded.
- `Result/Seq` is written after the other `Result/` nodes, so a tag change script on it reads the new values.
- A `Double` parameter that is NaN or infinite is answered with `Invalid` without sending anything.
