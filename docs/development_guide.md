# Development Guide: Shared Arm Controller, 4-DOF Arm Project and OPC UA Gateway

Status: draft for team review. The ESP32 firmware in `controller/` is partly implemented (see `controller/README.md`) and has not been tested on hardware yet. The gateway (`ignition_bridge/`) and the Ignition project are not started.

This guide covers three things:

1. The ESP32-S3 firmware for the robot arm, written once and used by two projects.
2. The Python gateway that sits between the ESP32 (HTTP) and Ignition (OPC UA).
3. The OPC UA IO list for the Ignition developer (section 7).

It builds on the Interface Specification ([README.md](../README.md)) and on [project_context.md](../project_context.md). Where this guide differs from `project_context.md`, the difference is listed in section 11.

## 1. The two projects and what they share

| | 4-DOF arm project | Automated Sorting System |
| --- | --- | --- |
| Arm | Same parallelogram arm, 4 servos (base, shoulder, elbow, wrist rotation) | Same arm, plus a gripper servo on the tool head |
| Extra hardware | Ultrasonic sensor on the wrist, joystick, buzzer | Conveyor stepper, Android phone (vision) |
| Obstacle avoidance | Yes | No |
| Spec interfaces used | 2.1, 2.2, 4.2, 4.3, 4.5, 4.6, 4.7, 4.8 | All except 4.8 |

Both projects use the same kinematics, servo driver, HTTP API layer, gateway and OPC UA node names. The 4-DOF project is a subset of the sorting system, so its Ignition tags and scripts carry over unchanged.

## 2. Architecture

```
                 HTTP + JSON                         OPC UA
  ESP32-S3  ───────────────────▶  Python gateway  ◀───────────▶  Ignition
  (server and client)  ◀────────  (HTTP server, HTTP client,     (OPC UA client,
                                   OPC UA server)                 no WebDev, no HTTP)
```

- **ESP32 to gateway**: the ESP32 pushes status, alarm and sorting-result reports to `/scada/...` on the gateway when they happen.
- **Gateway to ESP32**: the gateway forwards operator commands to `/controller/...` on the ESP32, and reads the arm position once per second.
- **Gateway to Ignition**: everything is exposed as OPC UA nodes. Ignition reads report nodes and writes command nodes. Ignition never speaks HTTP.

The gateway runs on the Ignition machine.

## 3. Arm parameters

These come from the existing test project (`ArmTest/src/main.cpp`) and must not change without re-measuring the arm.

| Parameter | Value |
| --- | --- |
| Link 1 (shoulder to elbow) | 140 mm |
| Link 2 (elbow to wrist) | 155 mm |
| Base direction | Inverted (`invertBase = true`) |
| Home pose (servo angles) | Base 90, shoulder 62, elbow 72 |

### Joints

`joint number` in interface 4.3 uses this numbering, and `angle` is the **physical** angle, not the raw servo angle.

| Joint | Name | Physical angle | Physical limits | Servo angle | Servo limits |
| --- | --- | --- | --- | --- | --- |
| 1 | Base | yaw, 0 = straight ahead | -90 to +90 | 90 - yaw | 0 to 180 |
| 2 | Shoulder | b | 25 to 110 | b - 5 | 20 to 105 |
| 3 | Elbow | a | 5 to 50 | a + 40 | 45 to 90 |
| 4 | Wrist rotation | 0 = pointing radially outward | To be measured | To be measured | To be measured |

The wrist rotates the tool about the vertical axis. It also aims the ultrasonic sensor. Rotating the wrist does not move the TCP.

### Kinematics

World frame: X forward, Y to the side, Z up, in mm.

```
R = L1*cos(b) + L2*cos(a) + tool.radial_offset
Z = L1*sin(b) - L2*sin(a) + tool.z_offset
X = R*cos(yaw)
Y = R*sin(yaw)
```

Inverse kinematics is the existing closed-form solution. Targets with X < 0, outside reach, or outside any joint limit give `"invalid"`.

### Tool heads

The parallelogram keeps the tool level, so a tool head is described by two offsets from the wrist to the TCP (radial and Z) plus any extra servo it carries. Each project has one tool configuration:

| Project | Tool | Extra servo |
| --- | --- | --- |
| 4-DOF arm | Ultrasonic sensor mount | None |
| Sorting | Gripper | Gripper open/close, driven by the sequence control only, with no external interface |

## 4. ESP32-S3 firmware

### 4.1 Layout

The repository has two top-level code folders: `controller/` for all ESP32 code, and `ignition_bridge/` for the Python gateway (section 6).

`controller/` is one PlatformIO project with two firmware environments and one PC test environment.

```
controller/
  platformio.ini          env:arm4dof and env:sorting (both esp32-s3), env:native (PC unit tests)
  include/                Pin assignment, network settings (NetConfig.h is git-ignored,
                          copy it from NetConfig.example.h)
  lib/
    ArmKinematics/        FK, IK and joint interpolation in plain C++, unit-tested on the PC
    ArmDriver/            Servo output, limits, 50 Hz motion task, stop,
                          machine state (permit, status, manual mode), serial console
    ToolHead/             TCP offsets and tool servo
    ControllerApi/        Wi-Fi and HTTP server for /controller/..., deferred responses
    ScadaClient/          Outbound queue for /scada/... reports, with retry
  src/arm4dof/            Obstacle monitor, joystick, buzzer
  src/sorting/            Conveyor, vision client, position config, sequence
  test/                   Unit tests, run with `pio test -e native`
ignition_bridge/          Python gateway: HTTP to OPC UA
```

Libraries: ESPAsyncWebServer, ArduinoJson, ESP32Servo. The sorting build adds AccelStepper.

Build from `controller/` with `pio run -e arm4dof` or `pio run -e sorting`. The PC tests need a host `g++` on the PATH.

### 4.2 Proposed pins (ESP32-S3-DevKitC-1)

| Function | GPIO | Notes |
| --- | --- | --- |
| Base servo | 11 | |
| Shoulder servo | 12 | |
| Elbow servo | 13 | |
| Wrist servo | 14 | |
| Joystick VRX | 1 | ADC1. ADC2 pins do not work while Wi-Fi is on |
| Joystick VRY | 2 | ADC1 |
| Joystick SW | 42 | Internal pull-up, active low |
| Ultrasonic TRIG | 15 | |
| Ultrasonic ECHO | 16 | Through a voltage divider, 5 V down to 3.3 V |
| Buzzer | 17 | |

Wiring rules:

- Power the servos from a separate 5 V supply and join its ground to the ESP32 ground. Do not power servos from the board.
- Power the joystick from **3.3 V**, not 5 V. On 5 V its outputs exceed what the ESP32-S3 pins tolerate.
- Do not use GPIO 0, 3, 45, 46 (strapping), 19, 20 (USB) or 35 to 37 (flash/PSRAM on some modules).

### 4.3 Tasks

| Task | Rate | Job |
| --- | --- | --- |
| Motion | 50 Hz | Steps each joint toward its target at a limited speed, writes servos, completes or aborts the pending command |
| Safety (arm4dof) | 20 Hz | Reads the ultrasonic sensor with a median filter, raises and clears the obstacle alarm |
| Joystick (arm4dof) | 20 Hz | Watches the SW button, and in manual mode converts stick deflection to small world-frame X-Y moves |
| SCADA client | Event-driven | Sends queued reports in order, retries with back-off |
| Web server | Async | Validates requests and passes them to the motion task through a queue |

Web handlers never move the arm and never block.

### 4.4 Behaviour

**Running permit and status.** After boot the machine is in `"stop"`. `set-running-permit` with `true` moves it to `"idle"`, and with `false` back to `"stop"`. The 4-DOF project reports `"idle"`, `"stop"` and `"manual"` (see "Manual mode" below). It has no value for "moving", because the specification has none. The sorting project has no joystick, so it reports every value except `"manual"`.

**Motion commands (4.2, 4.3).** One command runs at a time.

| Situation | `operation_result` | When the response is sent |
| --- | --- | --- |
| Target valid, motion finished | `"success"` | When the arm arrives |
| No IK solution or outside joint limits | `"invalid"` | Immediately |
| Another command is running, or the machine is in `"stop"` or `"manual"` | `"rejected"` | Immediately |
| Motion interrupted by an obstacle, by permit = false, or by the joystick taking over | `"rejected"` | At the moment of interruption |

**Obstacle (arm4dof only).** Proposed thresholds: stop below 100 mm, clear above 150 mm.

1. Distance falls below the stop threshold.
2. The arm freezes at its current interpolated position, and the buzzer sounds.
3. A pending motion request is answered with `"rejected"`.
4. The ESP32 pushes `report-alarm` with `alarm_message = "Obstacle detected"` and `alarm_status = true`, then `report-current-status` with `"stop"`.
5. When the distance rises above the clear threshold, the ESP32 pushes `report-alarm` with `alarm_status = false`.
6. The arm moves again only after the operator sends `set-running-permit` with `true`.

**Wrist during XYZ moves (arm4dof only).** The wrist turns so the sensor faces the horizontal direction of travel. A joint 4 command overrides this until the next XYZ move.

**Manual mode (arm4dof only).** The SW button on the joystick switches between manual mode and the automatic side (HMI and automatic program).

| Event | What happens |
| --- | --- |
| SW pressed, machine in `"idle"` or `"stop"` | Any running motion stops and its pending request is answered with `"rejected"`. The ESP32 pushes `report-current-status` with `"manual"`. This works whether or not the running permit is set |
| While in `"manual"` | The joystick alone moves the arm. HTTP motion commands are answered with `"rejected"`. `set-running-permit` has no effect |
| SW pressed, machine in `"manual"` | The arm stops, the running permit is cleared, and the ESP32 pushes `report-current-status` with `"stop"`. Control returns to the HMI and the automatic program, which need `set-running-permit` with `true` before the arm moves again |

In manual mode, stick deflection sets the speed of the TCP in world X and Y, with Z held, and every step passes through IK. The wrist turns the sensor toward the direction the stick is pushed.

The obstacle check stays active in manual mode. While an obstacle is too close, the alarm is raised, the buzzer sounds and the TCP does not move, but the wrist still turns. Pushing the stick away from the obstacle turns the sensor away, the alarm clears, and the arm can back off. The status stays `"manual"` throughout.

**Serial commands.** The serial commands of the test project (`XYZ`, `HOME`, `STATUS`, and the `#OK:` acknowledgement) are kept so the existing 3D GUI still works for commissioning.

## 5. HTTP API

All endpoints use `POST` with a JSON body, field names in `snake_case`, and HTTP 200 for every business result. An interface with no input takes `{}`, and one with no output returns `{}`.

### 5.1 ESP32 as server (called by the gateway)

| Endpoint | Request | Response | Project |
| --- | --- | --- | --- |
| `/controller/set-arm-position` | `{"x":150,"y":0,"z":50}` | `{"operation_result":"success"}` | Both |
| `/controller/set-joint-position` | `{"joint_number":4,"angle":30}` | `{"operation_result":"success"}` | Both |
| `/controller/set-running-permit` | `{"permit":true}` | `{}` | Both |
| `/controller/get-current-arm-position` | `{}` | `{"x":150,"y":0,"z":50}` | Both |
| `/controller/get-current-joint-position` | `{}` | `{"angles":[0,67,32,0]}` | Both |
| `/controller/get-obstacle-distance` | `{}` | `{"distance":183}` | 4-DOF |
| `/controller/move-conveyor` | `{"direction":true,"distance":100}` | `{"operation_result":"success"}` | Sorting |
| `/controller/update-position-config` | See below | `{"operation_done":true}` | Sorting |

The three read interfaces (4.6, 4.7, 4.8) answer immediately and are never rejected, including during motion and in manual mode. `angles` holds the physical angles from section 3, ordered by joint number. `distance` is the median-filtered reading, and `-1` means nothing is in range.

Proposed body for `update-position-config`:

```json
{
  "repositories": { "Repo1": {"x": 50, "y": 50, "z": 0}, "Repo2": {"x": 100, "y": 50, "z": 0} },
  "destinations": { "Hamilton": "Repo1", "Burlington": "Repo1", "Mississauga": "Repo2" },
  "part_dock": {"x": 10, "y": 10, "z": 10}
}
```

### 5.2 Gateway as server (called by the ESP32)

| Endpoint | Request | Response | Project |
| --- | --- | --- | --- |
| `/scada/report-current-status` | `{"status":"idle"}` | `{}` | Both |
| `/scada/report-alarm` | `{"alarm_message":"Obstacle detected","alarm_status":true}` | `{}` | Both |
| `/scada/report-sorting-result` | `{"reading_result":"success","origin":"Toronto","destination":"Hamilton","part_id":"10234","repository":"Repo1","sorting_result":true}` | `{}` | Sorting |

The gateway address and port are configurable in the firmware.

## 6. Python gateway

The gateway lives in `ignition_bridge/`. Proposed stack: Python 3.11 or later, `asyncua` for the OPC UA server, `aiohttp` for the HTTP server and client, all in one asyncio process.

| Setting | Proposed value |
| --- | --- |
| HTTP listen address | `0.0.0.0:8080` |
| OPC UA endpoint | `opc.tcp://127.0.0.1:4840/arm-gateway` |
| OPC UA security | None, anonymous. The endpoint listens on localhost only |
| OPC UA namespace URI | `urn:group12:arm-gateway` |
| Position, joint and distance read interval | 1 s |
| HTTP timeout, motion commands | 30 s |
| HTTP timeout, other calls | 2 s |
| Profile | `arm4dof` or `sorting`. The sorting profile adds the sorting-only nodes |

What the gateway does:

- **Reports.** On each `/scada/...` request it updates the matching nodes and returns `{}`.
- **Alarms.** A table in the config file maps each known `alarm_message` to a Boolean node. An unknown message goes to `Alarms/OtherMessage` and `Alarms/OtherActive`.
- **Position and distance.** Every second it calls `get-current-arm-position`, `get-current-joint-position` and, in the `arm4dof` profile, `get-obstacle-distance`, and writes the nodes under `Arm/`. Success or failure of these calls drives `Comm/EspOnline`.
- **Commands.** It watches each `Execute` node and runs the handshake in section 7.4.

Deployment: allow the HTTP port through the Windows firewall, and start the gateway automatically at boot.

## 7. OPC UA IO list (for the Ignition developer)

### 7.1 Connection

In the Ignition Gateway, add an OPC UA connection to `opc.tcp://127.0.0.1:4840/arm-gateway` with security policy None and anonymous login.

Node IDs are strings in the gateway's namespace: `ns=2;s=<path>`. For example, `ns=2;s=Arm/X`. The index 2 is what `asyncua` assigns to the first registered namespace. If browsing shows a different index, use that one.

"R" means Ignition only reads the node. "RW" means Ignition writes it.

### 7.2 Monitoring nodes

| Node path | Type | Access | Meaning | Project |
| --- | --- | --- | --- | --- |
| `Status/Current` | String | R | Current step: `idle`, `stop`, for 4-DOF also `manual`, and for sorting also `moving conveyor`, `read label`, `generate trajectory`, `pick up`, `place` | Both |
| `Arm/X` | Double | R | TCP X in mm, refreshed every second | Both |
| `Arm/Y` | Double | R | TCP Y in mm | Both |
| `Arm/Z` | Double | R | TCP Z in mm | Both |
| `Arm/Joint1` | Double | R | Base yaw in degrees, refreshed every second | Both |
| `Arm/Joint2` | Double | R | Shoulder angle b in degrees | Both |
| `Arm/Joint3` | Double | R | Elbow angle a in degrees | Both |
| `Arm/Joint4` | Double | R | Wrist rotation in degrees | Both |
| `Arm/ObstacleDistance` | Int32 | R | Ultrasonic distance in mm, refreshed every second. `-1` = nothing in range | 4-DOF |
| `Alarms/Obstacle` | Boolean | R | True while an obstacle is too close to the tool | 4-DOF |
| `Alarms/OtherActive` | Boolean | R | True while an alarm with an unmapped message is raised | Both |
| `Alarms/OtherMessage` | String | R | Text of the most recent unmapped alarm | Both |
| `Comm/EspOnline` | Boolean | R | True if the last periodic read from the ESP32 succeeded | Both |
| `Comm/LastReportTime` | DateTime | R | Time of the last report received from the ESP32 | Both |
| `Comm/Heartbeat` | Int32 | R | Increments every second while the gateway is running | Both |
| `Result/Seq` | Int32 | R | Increments by 1 for each sorting result received | Sorting |
| `Result/ReadingResult` | String | R | `success`, `info lost`, `none`, `fault` | Sorting |
| `Result/Origin` | String | R | Origin label text | Sorting |
| `Result/Destination` | String | R | Destination label text | Sorting |
| `Result/PartId` | String | R | Part ID | Sorting |
| `Result/Repository` | String | R | `Repo<N>` or `manual` | Sorting |
| `Result/SortingResult` | Boolean | R | True if the part was placed | Sorting |

The sorting project will add more mapped alarm nodes under `Alarms/` as its alarm messages are defined.

### 7.3 Command nodes

Every command has the same four or five status nodes. Their meaning is the same for all commands:

| Status node | Meaning |
| --- | --- |
| `Busy` | The command is being executed |
| `Done` | The command finished successfully |
| `Invalid` | The controller answered `"invalid"`: no IK solution, or outside joint limits |
| `Rejected` | The controller answered `"rejected"`: another command was running, the machine was in `stop` or `manual`, or the motion was interrupted |
| `CommFault` | The gateway could not reach the ESP32, or the request timed out |

| Node path | Type | Access | Meaning | Project |
| --- | --- | --- | --- | --- |
| `Cmd/SetArmPosition/X` | Double | RW | Target X in mm | Both |
| `Cmd/SetArmPosition/Y` | Double | RW | Target Y in mm | Both |
| `Cmd/SetArmPosition/Z` | Double | RW | Target Z in mm | Both |
| `Cmd/SetArmPosition/Execute` | Boolean | RW | Write true to start | Both |
| `Cmd/SetArmPosition/Busy` | Boolean | R | | Both |
| `Cmd/SetArmPosition/Done` | Boolean | R | | Both |
| `Cmd/SetArmPosition/Invalid` | Boolean | R | | Both |
| `Cmd/SetArmPosition/Rejected` | Boolean | R | | Both |
| `Cmd/SetArmPosition/CommFault` | Boolean | R | | Both |
| `Cmd/SetJointPosition/JointNumber` | Int32 | RW | 1 base, 2 shoulder, 3 elbow, 4 wrist | Both |
| `Cmd/SetJointPosition/Angle` | Double | RW | Physical angle in degrees | Both |
| `Cmd/SetJointPosition/Execute` | Boolean | RW | Write true to start | Both |
| `Cmd/SetJointPosition/Busy` | Boolean | R | | Both |
| `Cmd/SetJointPosition/Done` | Boolean | R | | Both |
| `Cmd/SetJointPosition/Invalid` | Boolean | R | | Both |
| `Cmd/SetJointPosition/Rejected` | Boolean | R | | Both |
| `Cmd/SetJointPosition/CommFault` | Boolean | R | | Both |
| `Cmd/SetRunningPermit/Permit` | Boolean | RW | True = start, false = stop | Both |
| `Cmd/SetRunningPermit/Execute` | Boolean | RW | Write true to send | Both |
| `Cmd/SetRunningPermit/Busy` | Boolean | R | | Both |
| `Cmd/SetRunningPermit/Done` | Boolean | R | | Both |
| `Cmd/SetRunningPermit/CommFault` | Boolean | R | | Both |
| `Cmd/MoveConveyor/Direction` | Boolean | RW | Conveyor direction | Sorting |
| `Cmd/MoveConveyor/Distance` | Int32 | RW | Distance in mm | Sorting |
| `Cmd/MoveConveyor/Execute` | Boolean | RW | Write true to start | Sorting |
| `Cmd/MoveConveyor/Busy` | Boolean | R | | Sorting |
| `Cmd/MoveConveyor/Done` | Boolean | R | | Sorting |
| `Cmd/MoveConveyor/Rejected` | Boolean | R | | Sorting |
| `Cmd/MoveConveyor/CommFault` | Boolean | R | | Sorting |
| `Cmd/UpdatePositionConfig/ConfigJson` | String | RW | The JSON body shown in section 5.1 | Sorting |
| `Cmd/UpdatePositionConfig/Execute` | Boolean | RW | Write true to send | Sorting |
| `Cmd/UpdatePositionConfig/Busy` | Boolean | R | | Sorting |
| `Cmd/UpdatePositionConfig/Done` | Boolean | R | | Sorting |
| `Cmd/UpdatePositionConfig/Failed` | Boolean | R | The controller answered `operation_done = false` | Sorting |
| `Cmd/UpdatePositionConfig/CommFault` | Boolean | R | | Sorting |

### 7.4 Command handshake

1. Ignition writes the parameter nodes.
2. Ignition writes `Execute = true`.
3. The gateway clears `Done` and all failure flags, sets `Busy = true`, and sends the HTTP request.
4. When the ESP32 answers (or the request fails), the gateway sets exactly one of `Done`, `Invalid`, `Rejected`, `Failed`, `CommFault`.
5. The gateway sets `Busy = false` and `Execute = false`.

Rules:

- The result flags stay as they are until the next `Execute` of the same command.
- A write to `Execute` while `Busy` is true is ignored.
- Motion commands can stay busy for many seconds. Do not put a short timeout on them in the HMI.
- Starting a second motion command while one is running is allowed at the OPC UA level. The ESP32 answers `"rejected"`, and the second command's `Rejected` flag turns on.

Example Ignition script for a "Go" button:

```python
base = "[default]ArmGateway/Cmd/SetArmPosition/"
system.tag.writeBlocking([base + "X", base + "Y", base + "Z"], [x, y, z])
system.tag.writeBlocking([base + "Execute"], [True])
```

The HMI then binds indicators to `Busy`, `Done`, `Invalid`, `Rejected` and `CommFault`.

### 7.5 What Ignition does with the nodes

| Requirement | How |
| --- | --- |
| Live status display | Bind to `Status/Current` and the nodes under `Arm/` |
| Manual mode indication | When `Status/Current` is `manual`, show that the joystick has control and disable the command buttons |
| Log arm positions to a database | Enable tag history on `Arm/X`, `Arm/Y`, `Arm/Z` and `Arm/Joint1` to `Arm/Joint4` |
| Record obstacle alarms as events | Configure an alarm on `Alarms/Obstacle` and store it with an Alarm Journal |
| Communication fault | Configure alarms on `Comm/EspOnline` = false and on a stale `Comm/Heartbeat` |
| Log sorting results | A tag change script on `Result/Seq` reads the other `Result/` nodes and inserts a database row |
| Remote monitoring and control | A Perspective project using the same tags |

Use `Result/Seq`, not the result fields themselves, to detect a new part: two parts in a row can have identical results.

## 8. Known limits of the current interfaces

- Joint angles and the obstacle distance reach the HMI once per second, so fast changes between two reads are not visible. The obstacle alarm itself is pushed immediately.
- The specification has no report ID, so a sorting result that the ESP32 retries can arrive twice and increment `Result/Seq` twice.

## 9. Implementation order

1. Firmware skeleton on the ESP32-S3: three servos moving, FK and IK ported with PC unit tests.
2. Interpolated motion, limits, stop, and the serial commands from the test project.
3. Wi-Fi and the `/controller/...` endpoints with deferred responses, tested with curl.
4. Wrist axis and tool head configuration.
5. Ultrasonic sensor, obstacle alarm, buzzer, joystick.
6. Gateway: HTTP server, OPC UA nodes, command handshake, tested against a stub ESP32.
7. Ignition: OPC UA connection, tags, history, alarms, Perspective views.
8. Sorting build: conveyor, vision client, position config, sequence control.

## 10. Testing with stubs

| Under test | Stub |
| --- | --- |
| ESP32 firmware | A laptop HTTP client for `/controller/...`, and a small HTTP server that prints `/scada/...` reports |
| Gateway | A stub ESP32 HTTP server with configurable delay and results, and an OPC UA client such as UaExpert |
| Ignition | The gateway running against the stub ESP32 |

## 11. Assumptions and differences to confirm with the team

| Item | This guide | Needs |
| --- | --- | --- |
| How Ignition receives reports | Through the Python gateway over OPC UA | `project_context.md` section 6.3 describes WebDev and must be updated |
| How Ignition sends commands | By writing OPC UA nodes, not `system.net.httpClient()` | Same section |
| Interrupted motion | Answered with `"rejected"` | Add a sentence to the specification, 4.2 to 4.4 |
| Motion command while the machine is in `"stop"` | Answered with `"rejected"` | Team confirmation |
| `joint number` and `angle` | Numbering and physical angles as in section 3 | Team confirmation |
| Interfaces 4.7 and 4.8 and status `"manual"` | Added to the specification for the 4-DOF project | Team review of the specification change |
| Manual mode ignores the running permit | The joystick can take over in `"stop"` and without a permit | Team confirmation |
| Obstacle in manual mode | TCP motion blocked, wrist still turns | Team confirmation |
| Position, joint and distance read every second by the gateway | Kept, as the only periodic calls | Team confirmation, since the design otherwise avoids polling |
| `update-position-config` body | As proposed in section 5.1 | Team confirmation |
| Wrist joint limits, tool offsets, obstacle thresholds | Not yet measured | Measurement on the arm |
