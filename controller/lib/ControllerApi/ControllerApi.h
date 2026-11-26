// Wi-Fi and the HTTP server for the /controller/... endpoints.
//
// All endpoints are POST with a JSON body and answer HTTP 200 for every
// business result (docs/development_guide.md, section 5.1). The motion
// endpoints (4.2, 4.3) answer only when the motion has finished or was
// interrupted. Handlers never move the arm and never block.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <functional>

#include "ArmMachine.h"

struct NetworkConfig {
    const char *ssid;
    const char *password;
    // Fixed address (project_context.md, section 2). Leave useStaticIp false
    // to rely on a DHCP reservation instead.
    bool useStaticIp;
    IPAddress ip;
    IPAddress gateway;
    IPAddress subnet;
};

// Starts the station connection. Returns at once. The connection is made,
// and re-made after a drop, in the background.
void wifiBegin(const NetworkConfig &net);

class ControllerApi {
public:
    // Fills the response of a read endpoint.
    using ReadFn = std::function<void(JsonObject out)>;

    ControllerApi(ArmMachine &machine, uint16_t port = 80)
        : machine_(machine), server_(port) {}

    // Registers the endpoints shared by both projects (4.2, 4.3, 4.5, 4.6,
    // 4.7) and starts the server. Call after wifiBegin() and after any
    // addReadEndpoint() or addEndpoint().
    void begin();

    // Adds a project-specific endpoint that takes {} and answers at once,
    // for example /controller/get-obstacle-distance.
    void addReadEndpoint(const char *path, ReadFn fn);

    // Adds a project-specific POST endpoint. The handler runs once the
    // whole body has arrived and parsed as a JSON object (an empty body
    // counts as {}), and must answer with one of the helpers below.
    using JsonHandler = std::function<void(AsyncWebServerRequest *, JsonObjectConst body)>;
    void addEndpoint(const char *path, JsonHandler handler) { post(path, std::move(handler)); }

    // ---- Helpers for project-specific endpoints ----
    static void sendJson(AsyncWebServerRequest *request, int code, const JsonDocument &doc);
    // HTTP `code` with {"error": message}.
    static void sendError(AsyncWebServerRequest *request, int code, const char *message);
    // Runs a motion command and answers {"operation_result": ...} when its
    // result is known: at once unless `start` returns kPending, otherwise
    // when `start`'s done callback runs.
    void deferOperationResult(AsyncWebServerRequest *request, const char *name,
                              const std::function<OpResult(ArmMachine::DoneFn)> &start);

private:
    void post(const char *path, JsonHandler handler);
    static void sendOperationResult(AsyncWebServerRequest *request, OpResult r);

    ArmMachine &machine_;
    AsyncWebServer server_;
};
