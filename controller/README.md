# controller: ESP32-S3 arm firmware

One PlatformIO project for both projects described in
[docs/development_guide.md](../docs/development_guide.md).

| Environment | What it builds |
| --- | --- |
| `arm4dof` | 4-DOF arm: shared arm controller, ultrasonic obstacle stop, joystick manual mode |
| `sorting` | Automated Sorting System. **Partial**: shared arm controller, conveyor (4.4), position config (4.1), sorting result report (2.3). No vision client or sequence yet |
| `native` | PC unit tests for the kinematics and the joint interpolator |

## Build and test

```
pio run -e arm4dof
pio run -e sorting
pio test -e native
```

`pio test -e native` needs a host `g++` on the PATH. On Windows without one, PlatformIO's own package works:

```
pio pkg install -g -t platformio/toolchain-gccmingw32
```

then add `%USERPROFILE%\.platformio\packages\toolchain-gccmingw32\bin` to the PATH.

## Configuration

| What | Where |
| --- | --- |
| Wi-Fi name and password, gateway address | At run time with the serial panel (below), stored in the ESP32's flash. `include/NetConfig.h` holds the defaults used until something is stored |
| Static IP, HTTP port, and the defaults above | `include/NetConfig.h`. Copy it from `include/NetConfig.example.h`. It is git-ignored |
| Pins (servos, joystick, ultrasonic, conveyor STEP/DIR/EN) | `include/BoardPins.h` |
| Link lengths, joint limits, servo mapping, speeds, home pose | `lib/ArmKinematics/ArmConfig.h` |
| Tool TCP offsets | `lib/ToolHead/ToolHead.h` |
| Obstacle thresholds, joystick | `src/arm4dof/Arm4dofConfig.h` |
| Conveyor steps per mm, speed, direction, largest distance | `src/sorting/SortingConfig.h` |

Not measured yet, still at the proposed defaults: wrist servo direction, offset and limits, tool offsets, obstacle thresholds, joint speeds, conveyor steps per mm and speed.

## Status

Everything below has been verified **by build and PC unit tests only**. Nothing has run on an ESP32-S3 or on the arm yet.

| Part | State |
| --- | --- |
| FK, IK, limits, interpolation (`ArmKinematics`) | Done, 31 unit tests |
| Servo output, 50 Hz motion task, stop (`ArmDriver`) | Written, builds |
| Permit, status, manual mode, result rules (`ArmMachine`) | Written, builds |
| Serial console compatible with the ArmTest 3D GUI | Written, builds |
| Wi-Fi and `/controller/...` endpoints 4.2, 4.3, 4.5, 4.6, 4.7 | Written, builds |
| `/scada/report-current-status`, `/scada/report-alarm` queue | Written, builds |
| arm4dof: obstacle monitor, joystick, endpoint 4.8 | Written, builds |
| sorting: conveyor (4.4) on a step/dir driver, endpoint `/controller/move-conveyor` | Written, builds |
| sorting: position config (4.1) in flash, endpoint `/controller/update-position-config` | Written, builds |
| sorting: `/scada/report-sorting-result` (2.3) in the report queue, `SRTEST` test command | Written, builds. Nothing sends real results until the sequence exists |
| sorting: gripper, vision client, sequence | Not started |

## Serial console

115200 baud. The texts the ArmTest GUI parses (`[Nano OK]`, `[Nano IK FAIL]`, `#OK:b,s,e`, the `STATUS` block) are unchanged.

`XYZ x y z`, `G x y z`, `HOME`, `STATUS`, `BOUNDS`, `STOP`, `PERMIT 0|1`, `STEP n`, `B|S|E|W <servo angle>`, `B+ B- S+ S- E+ E- W+ W-`, `HELP`.

Differences from ArmTest: motion is interpolated, so `STATUS` shows the commanded target. `INVBASE`, `PIN`, `TEST` and `RAW` are gone (these are compile-time settings now).

## Calibration sketches

`test/calib_*` are three standalone sketches for checking hardware. They use nothing from `lib/` and no external library, only constants at the top of each file and serial output of the raw and the processed values.

| Environment | What it does |
| --- | --- |
| `calib_joystick` | Prints raw ADC counts, calibrated millivolts, the normalised value and the value after deadband for both axes, plus the SW button |
| `calib_ultrasonic` | Prints the echo time, the raw distance, the corrected distance (`SCALE`, `OFFSET_MM`) and the median |
| `calib_servo` | Drives the four servo pins with raw LEDC PWM (no ESP32Servo). Serial commands: `u <n> <us>`, `a <n> <deg>`, `s <n>` sweep, `x <n>` off, `p` print (`n` = 1 to 4, 0 = all) |

```
pio run -e calib_servo -t upload
pio device monitor
```

The constants found this way are not copied into the firmware automatically: put them into `src/arm4dof/Arm4dofConfig.h`, `lib/ArmKinematics/ArmConfig.h` and `ArmHardware` (pulse range) by hand.

## Serial panel

`tools/serial_hmi/` is a small browser panel (Chrome or Edge, Web Serial) based on the ArmTest GUI, without the 3D view. It shows the machine status, permit, TCP position, joint angles, obstacle distance, joystick input (raw and processed) and Wi-Fi state, offers permit, home, stop, XYZ and joint jog, and stores the Wi-Fi and gateway settings on the board.

```
python tools/serial_hmi/run_hmi.py          (or double-click run_hmi.bat)
python tools/serial_hmi/run_hmi.py --demo   simulated data, no board needed
```

Plug the board in through its UART USB port and close any other serial monitor first. The panel uses these extra console commands (`src/common/HmiCommands.cpp`):

| Command | Effect |
| --- | --- |
| `INFO` | One line `#INFO:{json}` with the live state. The panel polls it twice a second |
| `J <joint> <angle>` | Jog joint 1 to 4 to a physical angle |
| `SET SSID\|PASS\|GWHOST\|GWPORT <value>` | Store a network setting. The value is the rest of the line |
| `SET STPATH\|ALPATH\|SRPATH <path>` | Store the path of the status (2.1), alarm (2.2) or sorting result (2.3) report on the SCADA receiver, for example `/system/webdev/SortingDemo/report_status`. Starts with `/`, at most 100 characters, no spaces. Without a path, the stored one is forgotten and the `NetConfig.h` default is used. `SRPATH` is used by the sorting build only |
| `SRTEST` | Sorting build only: queue one sorting result report with fixed example values (part `10234`, `Repo3`) to check the receiver and the path in use |
| `FACTORY` | Forget the stored settings, back to `NetConfig.h` |
| `REBOOT` | Restart, which applies stored settings |

The Wi-Fi password is stored in the ESP32's flash unencrypted and is never printed back.

The **SCADA endpoints** card sets the report paths, so the board can report to a different receiver (the Python gateway, an Ignition WebDev project, ...) without a rebuild. It shows the full URLs built from the gateway host and port of the Network card. **Default** forgets the stored paths. Like the other settings, they are used from the next boot. With the sorting build the card also has the **sorting result path** and a **Send test result** button (`SRTEST`). The test report is recorded by the receiver like a real part. Reports are retried until the receiver accepts them, so a wrong path holds up the later reports until the path is fixed and the board rebooted.

`python tools/serial_hmi/run_hmi.py --demo sorting` shows the panel as for the sorting build.

## HTTP panel

`tools/http_hmi/` tests the board over Wi-Fi, without the gateway or Ignition. It is one Python script (standard library only) that serves a browser panel and does two jobs:

- **Receives the `/scada/...` reports** (2.1 status, 2.2 alarm, 2.3 sorting result) in place of the gateway, with the same validation (HTTP 400 and `{"error": ...}` for a bad body). The panel shows the current status, the active alarms and the last sorting results, and logs every report.
- **Forwards the panel's `/controller/...` calls** to the ESP32, so the browser needs nothing from the board. It polls position, joint angles and obstacle distance, and offers permit, set arm position, set joint position, move conveyor, update position config and a raw request box with the full answer and round trip time.

```
python tools/http_hmi/run_http_hmi.py --esp32 http://192.168.1.50   (or double-click run_http_hmi.bat)
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--esp32` | `http://192.168.1.50` | ESP32 base URL. Can also be changed in the panel |
| `--port` | `8080` | Port of the panel and of the `/scada` reports. Same as the gateway, so the board's settings need not change, but the gateway must not run at the same time |
| `--host` | `0.0.0.0` | Listen address. Must be reachable from the ESP32 |
| `--no-browser`, `--verbose` | | Do not open the browser; log every HTTP request |

At start it prints this PC's IP addresses. Set the board's gateway host to one of them, the gateway port to `--port` and the report paths to `/scada/report-current-status`, `/scada/report-alarm` and (sorting build) `/scada/report-sorting-result` (with the serial panel, or `NET_SCADA_HOST` / `NET_SCADA_PORT` / `NET_SCADA_STATUS_PATH` / `NET_SCADA_ALARM_PATH` / `NET_SCADA_RESULT_PATH`), and allow the port through the Windows firewall (see `ignition_bridge/README.md`). Motion commands wait up to 30 s for the answer; send a second one while the first runs to see `rejected`.

Without a board, run it against the gateway's stub ESP32:

```
ignition_bridge\.venv\Scripts\arm-gateway-stub --port 8081 --gateway-url http://127.0.0.1:8080
python tools/http_hmi/run_http_hmi.py --esp32 http://127.0.0.1:8081
```

The forwarding endpoint only accepts `/controller/...` paths, but anyone on the network who can reach the port can use it to move the arm. Use it on the lab network only.

## Assumptions made where the specification and the guide are silent

Raise these for team review.

1. A command that is both blocked (machine in `stop` or `manual`, or another command running) and invalid is answered `"rejected"`: the machine state is checked first.
2. A `joint_number` outside 1 to 4 is answered `"invalid"`.
3. Malformed JSON, or a missing or wrongly typed field, is answered with HTTP 400 and `{"error": "..."}`. An unknown path gives 404. An empty body counts as `{}`.
4. If the HTTP client disconnects during a motion, the motion carries on and its result is dropped. Until it ends, other motion commands are `"rejected"`.
5. While the obstacle alarm is active, `set-arm-position` and `set-joint-position` for joints 1 to 3 are `"rejected"` even after a new running permit. Joint 4 (wrist) is still accepted, so the sensor can be turned away to clear the alarm. This mirrors the manual-mode rule.
6. On an obstacle stop, `report-current-status` with `"stop"` is sent even if the machine was already in `"stop"`.
7. Wrist during an XYZ move: the wrist turns to `heading - yaw`, clamped to its limits, and joints 1 to 3 wait until it is within 10 degrees. A purely vertical move leaves the wrist alone. In manual mode the TCP likewise waits until the wrist is within 15 degrees of the stick direction.
8. Serial jog commands do not need the running permit (commissioning without the gateway). They are refused in manual mode, while an HTTP motion command is running, and for joints 1 to 3 while the obstacle alarm is active.
9. XYZ moves are interpolated in joint space, so the TCP path between two points is not a straight line.
10. Positions and angles are returned rounded to 0.1.
11. The servo pulse range is 544 to 2400 microseconds for 0 to 180 degrees, the Arduino Nano `Servo` default that the ArmTest calibration was done with.
12. The obstacle distance is the median of five readings at 20 Hz, so the alarm reacts after about 150 ms.
13. The joystick centre is read at power-up, so the stick must be released then.
14. Sorting: only one motion command runs at a time across the arm and the conveyor. `move-conveyor` follows the rules of 4.2 and 4.3: `"rejected"` without the running permit or while any motion runs, and a running move is stopped and answered `"rejected"` by `set-running-permit` `false` or the serial `STOP`.
15. `move-conveyor`: `direction` `true` drives the DIR pin to `kConveyorDirLevelForTrue` (`SortingConfig.h`), to be defined as the feed direction when the conveyor is built. A `distance` below 0 or above 5000 mm is answered with HTTP 400, because 4.4 has no `"invalid"` result. A distance of 0 is `"success"` without moving. The belt runs at a constant speed without a ramp. A manual `move-conveyor` does not change the reported status.
16. `update-position-config`: malformed JSON is HTTP 400. A body that is valid JSON but incomplete or inconsistent is answered `{"operation_done": false}` and the reason is printed on the serial log: `repositories` needs 1 to 16 entries named `Repo<N>` with numeric `x`, `y`, `z`; `destinations` (may be empty, up to 64) must map each name to one of those repositories; `part_dock` needs numeric `x`, `y`, `z`. Reachability by the arm is not checked. On `false` the previous config stays in use. The config is kept in flash and loaded at boot. Destination names are case-sensitive and must match the OCR text.
17. `report-sorting-result` reports are never dropped from the queue and are retried until accepted, but they are not sent again after a reconnect (they are events, not states), so a result can reach the receiver twice only if its first answer was lost.

## Trying it without the gateway

```
curl -X POST http://<esp32-ip>/controller/set-running-permit -H "Content-Type: application/json" -d "{\"permit\":true}"
curl -X POST http://<esp32-ip>/controller/set-arm-position -H "Content-Type: application/json" -d "{\"x\":180,\"y\":40,\"z\":50}"
curl -X POST http://<esp32-ip>/controller/get-current-joint-position -d "{}"
```

Sorting build:

```
curl -X POST http://<esp32-ip>/controller/move-conveyor -H "Content-Type: application/json" -d "{\"direction\":true,\"distance\":100}"
curl -X POST http://<esp32-ip>/controller/update-position-config -H "Content-Type: application/json" -d "{\"repositories\":{\"Repo1\":{\"x\":50,\"y\":50,\"z\":0}},\"destinations\":{\"Hamilton\":\"Repo1\"},\"part_dock\":{\"x\":10,\"y\":10,\"z\":10}}"
```

The status, alarm and sorting result reports go to `http://<gateway host>:<gateway port><path>`, set with the serial panel (defaults: `NET_SCADA_HOST`, `NET_SCADA_PORT`, `NET_SCADA_STATUS_PATH`, `NET_SCADA_ALARM_PATH`, `NET_SCADA_RESULT_PATH`). Without the gateway, the HTTP panel above receives and shows them.
