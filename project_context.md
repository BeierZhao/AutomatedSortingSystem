# Automated Sorting System: Project Context

This document gives teammates and AI agents the context needed to implement the system. It does not define endpoint paths, request and response schemas, or error codes. Those are in the separate API interface specification, which is the authoritative source for all inter-subsystem messages. Where this document shows example paths, they are placeholders until the API specification confirms them.

## 1. Project Overview

This is a course project (McMaster University, PROCTECH 4SS3, Group 12) to design and implement an automated sorting system for industrial applications. Parts travel along a conveyor belt. The system must determine each part's position and label, decide where each part belongs, and move it to the correct destination without human intervention.

## 2. Hardware and Software Stack

| Subsystem | Hardware | Software |
|---|---|---|
| Controller | ESP32 (Wi-Fi version) | Arduino framework, ESPAsyncWebServer, ArduinoJson |
| Conveyor | Stepper motor and driver | Motor control on the ESP32 |
| Robotic arm | Four-bar parallelogram arm driven by a servo motor | Servo control on the ESP32 |
| Detection and recognition | Android smartphone (high-definition camera, good performance, low power, portable, Wi-Fi) | Android app using OkHttp or HttpURLConnection |
| SCADA / HMI | Operator workstation on the plant network | Ignition, using the built-in system.net.httpClient() |

The Ignition, ESP32, and Android components all communicate over the same Wi-Fi and plant network.

## 3. Subsystems and Responsibilities

Position sensing detects the position of each part as it moves along the conveyor. In this design, the conveyor's stepper motor step count provides the belt position, so the controller knows where each part is relative to the arm.

Machine vision runs on the Android phone. It scans each part's label, extracts the label data, and sends the result to the controller.

The controller runs on the ESP32. It is the central decision maker. It receives detection data from the phone, determines each part's destination, drives the conveyor stepper, commands the servo-driven arm, and serves status to Ignition.

The actuation subsystem is the servo-driven parallelogram arm. It moves parts to their designated locations and reports completion or failure to the controller.

The SCADA HMI runs in Ignition. It displays real-time system status, part positions, sorting results, and alarms, and it stores sorting data for later analysis.

## 5. Communication Topology

All communication uses HTTP with JSON bodies. The ESP32 acts as the HTTP server. The Android phone and Ignition act as HTTP clients.

## 6. Communication Conventions

Use UTF-8 encoded JSON with the `Content-Type: application/json` header. Use ISO 8601 timestamps with time zone information. Use standard HTTP status codes, and return a JSON error body on failure. Identifiers should be stable strings, and a part's identifier should not change during its time on the conveyor. Consumers must tolerate unknown fields in responses so schemas can be extended without breaking other subsystems.

## 7. Implementation Notes by Platform

### 7.1 ESP32 (Arduino framework)

Use the ESPAsyncWebServer library, in its asynchronous form, because it handles requests in the background and does not block motor control. Use ArduinoJson to parse and build all JSON. Note that ESPAsyncWebServer does not parse JSON request bodies by itself. Register POST routes with a body handler (the onRequestBody callback) and parse the accumulated body with ArduinoJson once it is complete, or use the AsyncCallbackJsonWebHandler helper if available in your version.

For the conveyor stepper, a non-blocking stepping library such as AccelStepper is a suitable choice, and for the servo, the ESP32Servo library is commonly used. These are suggestions and should be confirmed against the hardware drivers the team selects.

### 7.2 Android phone

Use OkHttp or HttpURLConnection to send POST requests with a JSON body to the ESP32. Network calls must run on a background thread, since Android throws an exception if network I/O runs on the main thread. Use a coroutine, an executor, or OkHttp's asynchronous enqueue method. Keep the camera and recognition pipeline separate from the network thread so that a slow or failed request does not stall recognition. Handle failures by retrying a limited number of times and logging the failure, rather than retrying without bound.

### 7.3 Ignition (Gateway Timer Script)

Ignition uses the built-in system.net.httpClient() and needs no additional modules. Create a Gateway Timer Script. In each execution, send a GET request to the ESP32 status endpoint, decode the JSON response with system.util.jsonDecode(), and write the values to tags using system.tag.writeBlocking() or the equivalent for your project. Set a short timeout on the HTTP client so a missing controller does not stall the timer. If the request fails, set a communication-fault tag so the HMI can show that the controller is unreachable.

## 9. Requirements Summary

The system must transport parts along the conveyor, identify each part's position and label, and sort parts to the correct destination without human intervention. Operators must see real-time system status, part positions, sorting results, and alarms through Ignition. All sorting data must be logged and retained for later analysis. The system must support remote monitoring from within the plant network.

## 10. Guidance for Implementers (Human or AI)

Keep each subsystem decoupled. Each should depend only on the API contract, not on the internal implementation of another subsystem. Use the API specification as the single source of truth for message formats, and do not invent fields or endpoints that are not in it. If a required behavior is not specified, state the assumption explicitly and raise it for team review rather than silently choosing one. Build and test each platform independently with mock data before integration: the ESP32 can be tested with a simple HTTP client on a laptop, the phone with a stub server, and Ignition with a stub endpoint. Log every received and sent message with its timestamp and part identifier to support debugging and the data analysis requirement.

