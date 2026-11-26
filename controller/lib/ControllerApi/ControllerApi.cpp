#include "ControllerApi.h"

#include <WiFi.h>
#include <math.h>

using namespace arm;

namespace {

const size_t kMaxBodyBytes = 1024;

// One decimal is plenty for mm and degrees, and keeps float noise such as
// 149.99998 out of the JSON.
double round1(float v) { return round((double)v * 10.0) / 10.0; }

void sendJson(AsyncWebServerRequest *request, int code, const JsonDocument &doc) {
    String body;
    serializeJson(doc, body);
    Serial.printf("[%lu] HTTP -> %s %d %s\n", millis(), request->url().c_str(), code,
                  body.c_str());
    request->send(code, "application/json", body);
}

void sendError(AsyncWebServerRequest *request, int code, const char *message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJson(request, code, doc);
}

// Collects the request body into request->_tempObject (freed by the server
// when the request is destroyed).
void collectBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                 size_t total) {
    if (total > kMaxBodyBytes) return;
    if (index == 0 && !request->_tempObject) {
        request->_tempObject = calloc(total + 1, 1);
    }
    if (request->_tempObject && index + len <= total) {
        memcpy((char *)request->_tempObject + index, data, len);
    }
}

}  // namespace

void wifiBegin(const NetworkConfig &net) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    if (net.useStaticIp) WiFi.config(net.ip, net.gateway, net.subnet);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
        if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            Serial.printf("[%lu] WiFi connected, IP %s\n", millis(),
                          WiFi.localIP().toString().c_str());
        } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            Serial.printf("[%lu] WiFi disconnected\n", millis());
        }
    });
    WiFi.begin(net.ssid, net.password);
}

// Registers POST `path`. The handler runs once the whole body has arrived
// and has parsed as a JSON object. An empty body counts as {}.
void ControllerApi::post(const char *path, JsonHandler handler) {
    server_.on(
        path, HTTP_POST,
        [handler](AsyncWebServerRequest *request) {
            if (request->contentLength() > kMaxBodyBytes) {
                sendError(request, 413, "body too large");
                return;
            }
            const char *text = (const char *)request->_tempObject;
            Serial.printf("[%lu] HTTP <- %s %s\n", millis(), request->url().c_str(),
                          text ? text : "");
            JsonDocument doc;
            if (text && *text) {
                if (deserializeJson(doc, text) || !doc.is<JsonObjectConst>()) {
                    sendError(request, 400, "malformed JSON");
                    return;
                }
            } else {
                doc.to<JsonObject>();
            }
            handler(request, doc.as<JsonObjectConst>());
        },
        nullptr, collectBody);
}

void ControllerApi::sendOperationResult(AsyncWebServerRequest *request, OpResult r) {
    JsonDocument doc;
    doc["operation_result"] = opResultText(r);
    sendJson(request, 200, doc);
}

// Runs a motion command and answers when its result is known: at once for
// "invalid" and "rejected", otherwise when the motion task reports the end
// of the move. The request is paused meanwhile. If the client disconnects,
// the weak pointer expires, the motion carries on and no response is sent.
void ControllerApi::deferOperationResult(
    AsyncWebServerRequest *request, const char *name,
    const std::function<OpResult(ArmMachine::DoneFn)> &start) {
    AsyncWebServerRequestPtr paused = request->pause();

    const OpResult r = start([this, paused, name](OpResult result) {
        if (auto req = paused.lock()) {
            sendOperationResult(req.get(), result);
        } else {
            Serial.printf("[%lu] HTTP %s: client gone, result \"%s\" dropped\n", millis(),
                          name, opResultText(result));
        }
    });

    if (r != OpResult::kPending) {
        if (auto req = paused.lock()) sendOperationResult(req.get(), r);
    }
}

void ControllerApi::addReadEndpoint(const char *path, ReadFn fn) {
    post(path, [fn](AsyncWebServerRequest *request, JsonObjectConst) {
        JsonDocument doc;
        fn(doc.to<JsonObject>());
        sendJson(request, 200, doc);
    });
}

void ControllerApi::begin() {
    // 4.2 set arm position
    post("/controller/set-arm-position",
         [this](AsyncWebServerRequest *request, JsonObjectConst body) {
             if (!body["x"].is<float>() || !body["y"].is<float>() || !body["z"].is<float>()) {
                 sendError(request, 400, "x, y, z (numbers) required");
                 return;
             }
             const float x = body["x"], y = body["y"], z = body["z"];
             deferOperationResult(request, "set-arm-position",
                                  [this, x, y, z](ArmMachine::DoneFn done) {
                                      return machine_.setArmPosition(x, y, z, done);
                                  });
         });

    // 4.3 set joint position
    post("/controller/set-joint-position",
         [this](AsyncWebServerRequest *request, JsonObjectConst body) {
             if (!body["joint_number"].is<int>() || !body["angle"].is<float>()) {
                 sendError(request, 400, "joint_number (integer), angle (number) required");
                 return;
             }
             const int joint = body["joint_number"];
             const float angle = body["angle"];
             deferOperationResult(request, "set-joint-position",
                                  [this, joint, angle](ArmMachine::DoneFn done) {
                                      return machine_.setJointPosition(joint, angle, done);
                                  });
         });

    // 4.5 set running permit
    post("/controller/set-running-permit",
         [this](AsyncWebServerRequest *request, JsonObjectConst body) {
             if (!body["permit"].is<bool>()) {
                 sendError(request, 400, "permit (boolean) required");
                 return;
             }
             machine_.setRunningPermit(body["permit"].as<bool>());
             JsonDocument doc;
             doc.to<JsonObject>();
             sendJson(request, 200, doc);
         });

    // 4.6 get current arm position
    addReadEndpoint("/controller/get-current-arm-position", [this](JsonObject out) {
        const Pose p = machine_.currentPose();
        out["x"] = round1(p.x);
        out["y"] = round1(p.y);
        out["z"] = round1(p.z);
    });

    // 4.7 get current joint position
    addReadEndpoint("/controller/get-current-joint-position", [this](JsonObject out) {
        const JointAngles q = machine_.currentJoints();
        JsonArray angles = out["angles"].to<JsonArray>();
        for (int j = 0; j < kJointCount; j++) angles.add(round1(q.q[j]));
    });

    server_.onNotFound([](AsyncWebServerRequest *request) {
        sendError(request, 404, "unknown endpoint");
    });
    server_.begin();
}
