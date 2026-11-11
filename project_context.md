# Automated Sorting System: Project Context

This document gives teammates and AI agents the context needed to implement the system. It does **not** repeat the contents of the Interface Specification (interface names, parameters, enum values, example config). That specification is the single source of truth for what each interface does. This document covers what the specification leaves open: who calls whom, how interfaces map onto HTTP + JSON, and platform-specific implementation notes.

## 1. Project Overview

This is a course project (McMaster University, PROCTECH 4SS3, Group 12) to build an automated sorting system. Parts arrive on a conveyor, a vision system reads each part's labels, and a robotic arm picks each part up and places it in the correct repository. The operator watches the process through a SCADA HMI. The subsystems communicate over HTTP with JSON bodies.

## 2. Hardware and Software Stack

| Subsystem | Hardware | Software |
|---|---|---|
| Controller | ESP32 (Wi-Fi version) | Arduino framework, ESPAsyncWebServer, ArduinoJson |
| Conveyor | Stepper motor and driver | Driven by the ESP32 |
| Robotic arm | Four-axis parallelogram arm, servo-driven | Driven by the ESP32, with inverse and forward kinematics running on the ESP32 |
| Vision | Android smartphone (HD camera, Wi-Fi) | Android app with OCR, barcode scanning, and an embedded HTTP server |
| SCADA / HMI | Operator workstation | Ignition, with the WebDev module for receiving HTTP requests |

All devices are on the same Wi-Fi / plant network and need fixed IP addresses (DHCP reservation or static configuration).

## 3. Who Is the Server and Who Is the Client

The Interface Specification groups interfaces by the subsystem that **implements** them. The implementing subsystem is the HTTP server for those interfaces, and the other party is the client. There is **no polling** in this design: SCADA information is pushed by the controller when events happen.

| Interfaces (spec section) | HTTP server | HTTP client | Notes |
|---|---|---|---|
| SCADA/HMI (2.1 to 2.3) | Ignition | ESP32 | Event-driven reports pushed by the controller when status changes, an alarm changes, or a part is finished. |
| Vision (3.1 to 3.2) | Android phone | ESP32 | Called by the controller's sequence control to read labels and, optionally, locate the part. |
| Controller (4.1 to 4.6) | ESP32 | Ignition | Used by the operator side for configuration download, start/stop, and manual commissioning (arm, joint, conveyor commands and position readback). |

The ESP32 is therefore both an HTTP server (to Ignition) and an HTTP client (to Ignition's WebDev endpoints and to the phone). The phone and Ignition never call each other.

Assumption to confirm: when the sequence control on the ESP32 itself needs to move the arm or the conveyor (the "pick up", "place", and "moving conveyor" steps), it should call the same internal functions directly rather than sending HTTP requests to itself. The HTTP interfaces 4.2 to 4.4 exist for external callers such as Ignition.

## 4. Sequence Control

The sequence control runs on the ESP32. The step names reported through interface 2.1 are the authoritative description of the cycle. At a high level, the controller starts a cycle only when running is permitted, brings a part in by the conveyor, calls the vision subsystem, uses the position config to determine the target, moves the arm to pick and place the part, and reports the outcome. At each step change it pushes a status report to Ignition, and any fault is pushed as an alarm report.

## 5. HTTP + JSON Conventions (Proposed Mapping)

The Interface Specification is protocol-neutral. The mapping below is a **proposal** so that all three platforms use the same rules. The team should confirm or change it, and then freeze it.

- Each interface is one endpoint on its server. Use `POST` with a JSON body for every interface, including interfaces that take no input (send an empty object `{}`) and interfaces that return nothing (respond with an empty object `{}`). One method everywhere keeps the three codebases simple.
- Endpoint path convention: `/<subsystem>/<interface-name-in-kebab-case>`, for example the interface "report alarm" under SCADA becomes `/scada/report-alarm`.
- JSON field names: the parameter names from the specification in `snake_case`, with the exact parameter names and string enum values from the specification.
- Use UTF-8 and the `Content-Type: application/json` header.
- Use standard HTTP status codes for transport and protocol problems (malformed JSON, unknown path, server error). Business results that the specification defines as return values (such as the operation result strings) go in the JSON body with HTTP 200.
- Receivers must ignore unknown fields so schemas can be extended.
- Spec conventions on units still apply: millimetres, degrees, and case-sensitive enum strings.

## 6. Implementation Notes by Platform

### 6.1 ESP32 (Arduino framework)

**As server.** Use ESPAsyncWebServer with ArduinoJson. The library does not parse JSON bodies for you: accumulate the body in the body callback, or use the `AsyncCallbackJsonWebHandler` helper if your library version includes it, then parse with ArduinoJson.

Web handlers must be short and must never run motion or call `delay()`. Validate the input, hand work to the motion/sequence code through a FreeRTOS queue, and return.

**Long-running interfaces.** Interfaces 4.2, 4.3, and 4.4 return "success" only after the arm or conveyor has actually reached its target, so the response is deferred until the motion finishes. With the asynchronous server, keep the request object, let the motion task complete the movement, and then send the response from that completion path. Handle the case where the client disconnects in the meantime. The "rejected" result in the specification means a command is already running, so the controller should execute one motion command at a time and answer immediately with "rejected" otherwise. Callers must use generous HTTP timeouts for these three interfaces.

**As client.** Calls to Ignition (2.x) and to the phone (3.x) use HTTPClient or an asynchronous HTTP client library. A synchronous HTTPClient call blocks until the response arrives, so run all outbound calls in a dedicated FreeRTOS task, never in the web server callbacks or the motor control loop. Set short connect timeouts for Ignition reports, and a longer, bounded timeout for the vision call because OCR may take time.

**Outbound event queue for SCADA reports.** Because reports are pushed and not polled, a lost request means the HMI would show stale data. Keep a bounded outbound queue, send events in order, and retry on failure with a limited back-off. If Ignition was unreachable for a while, on reconnection resend the current status and all active alarm states so the HMI resynchronizes. A queue full condition should not stall the sequence: drop the oldest status updates first, and never drop alarms or sorting results.

**Motion.** A non-blocking stepper library such as AccelStepper is suitable for the conveyor, and ESP32Servo for the servos. These are suggestions to confirm against the chosen drivers. The kinematics (inverse for interface 4.2, forward for interface 4.6) run on the ESP32 and the arm's joint limits are needed to implement the "invalid" results.

### 6.2 Android phone (Vision)

The phone must run an **HTTP server inside the app**. Suitable options are NanoHTTPD (small and simple) or Ktor Server. The server exposes the vision interfaces and responds to requests from the ESP32 only.

Run the server and the camera pipeline in a **foreground service** with a persistent notification, and keep the screen awake or hold the appropriate wake lock while the system runs. Otherwise Android may throttle or kill the app in the background, which would make the controller's calls fail. Disable battery optimization for the app.

Run the camera, OCR, and barcode pipeline off the HTTP handler thread. The handler for "read part label" should trigger a capture, wait for the recognition result (with its own internal timeout), and return the result. Return the specification's fault values when the camera or recognition fails, instead of leaving the request hanging.

The phone's IP address must be stable, so set a DHCP reservation, and make the address configurable in the ESP32 firmware.

### 6.3 Ignition (SCADA / HMI)

**Receiving reports (server role).** Ignition receives the three SCADA interfaces through the **WebDev module**, which is a separate module and must be installed on the Gateway. Check that your Ignition edition and license provide WebDev before starting. Create one WebDev resource per SCADA interface. In each `doPost` handler, parse the JSON body, write the values to tags (status, alarm, sorting result), and return an empty JSON object. The exact structure of the request object and the way the body is exposed differ between Ignition versions, so verify them in the Designer's WebDev documentation for your version. Keep handlers fast and write to tags and the database only.

Alarm reports are state-based (raised or reset), so a repeated report of the same state is harmless. Sorting results are events, so a retry from the controller may arrive twice (see section 8).

**Calling the controller (client role).** Use `system.net.httpClient()`, which needs no extra module. Call it from the HMI actions (buttons for start/stop, config download, manual commands) and set timeouts appropriate to the interface, with long timeouts for the motion interfaces. Use the response results to update the HMI.

**Fallback if WebDev is not available.** The pure-HTTP workaround is to have the ESP32 hold a numbered event buffer and have Ignition fetch from it with a Gateway Timer Script. This is polling, which the team has judged unsuitable for these interfaces, so use it only if WebDev cannot be obtained. The alternative is to use MQTT through a broker, which would change the agreed communication method.

## 7. Guidance for Implementers (Human or AI)

Keep each subsystem decoupled and dependent only on the specification and the HTTP mapping in section 5. Do not invent interfaces, fields, or enum values that are not in the specification. If a behavior is not specified, state the assumption explicitly and raise it for team review. Build and test each platform independently with stubs before integration: test the ESP32 server with a laptop HTTP client, the phone's server with the same, and Ignition's WebDev endpoints with a laptop sending sample reports. Provide a stub vision server and a stub SCADA server that the ESP32 firmware can be tested against. Log each sent and received message with a timestamp and the part ID where available.

