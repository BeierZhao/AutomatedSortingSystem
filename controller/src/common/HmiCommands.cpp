#include "HmiCommands.h"

#include <WiFi.h>
#include <math.h>

#include "Settings.h"

using namespace arm;

namespace {

ArmDriver *g_driver = nullptr;
ArmMachine *g_machine = nullptr;
const char *g_appName = "";
hmi::InfoExtraFn g_extra;

double round1(float v) { return round((double)v * 10.0) / 10.0; }

void printInfo(Stream &io) {
    const ArmConfig &cfg = g_machine->config();
    const JointAngles now = g_driver->current();
    const JointAngles target = g_driver->target();
    const Pose p = forwardKinematics(cfg, now);

    JsonDocument doc;
    JsonObject info = doc.to<JsonObject>();
    info["app"] = g_appName;
    info["status"] = g_machine->status();
    info["permit"] = g_machine->permit();
    info["manual"] = g_machine->manual();
    info["blocked"] = g_machine->tcpBlocked();
    info["moving"] = g_driver->isMoving();

    JsonArray joints = info["joints"].to<JsonArray>();
    JsonArray targets = info["target"].to<JsonArray>();
    JsonArray limits = info["limits"].to<JsonArray>();
    for (int j = 0; j < kJointCount; j++) {
        joints.add(round1(now.q[j]));
        targets.add(round1(target.q[j]));
        JsonArray lim = limits.add<JsonArray>();
        lim.add(cfg.joint[j].physMin);
        lim.add(cfg.joint[j].physMax);
    }
    info["x"] = round1(p.x);
    info["y"] = round1(p.y);
    info["z"] = round1(p.z);

    const bool connected = WiFi.status() == WL_CONNECTED;
    JsonObject wifi = info["wifi"].to<JsonObject>();
    wifi["connected"] = connected;
    wifi["ssid"] = settings::active().wifiSsid;
    wifi["ip"] = connected ? WiFi.localIP().toString() : String("");
    wifi["rssi"] = connected ? WiFi.RSSI() : 0;
    wifi["gwhost"] = settings::active().scadaHost;
    wifi["gwport"] = settings::active().scadaPort;
    wifi["stpath"] = settings::active().scadaStatusPath;
    wifi["alpath"] = settings::active().scadaAlarmPath;

    // Stored values = what the next boot will use. The password itself is
    // never printed.
    const DeviceSettings stored = settings::stored();
    const DeviceSettings &active = settings::active();
    JsonObject cfgOut = info["cfg"].to<JsonObject>();
    cfgOut["ssid"] = stored.wifiSsid;
    cfgOut["pass_set"] = stored.wifiPassword.length() > 0;
    cfgOut["gwhost"] = stored.scadaHost;
    cfgOut["gwport"] = stored.scadaPort;
    cfgOut["stpath"] = stored.scadaStatusPath;
    cfgOut["alpath"] = stored.scadaAlarmPath;
    cfgOut["pending"] = stored.wifiSsid != active.wifiSsid ||
                        stored.wifiPassword != active.wifiPassword ||
                        stored.scadaHost != active.scadaHost ||
                        stored.scadaPort != active.scadaPort ||
                        stored.scadaStatusPath != active.scadaStatusPath ||
                        stored.scadaAlarmPath != active.scadaAlarmPath;

    if (g_extra) g_extra(info);

    String line;
    serializeJson(doc, line);
    io.print(F("#INFO:"));
    io.println(line);
}

void jogJoint(char *args, Stream &io) {
    char *end = nullptr;
    const long jointNumber = strtol(args, &end, 10);
    if (end == args || jointNumber < 1 || jointNumber > kJointCount) {
        io.println(F("#J:ERR usage: J <joint 1-4> <angle>"));
        return;
    }
    const float angle = atof(end);
    JointAngles target = g_driver->target();
    target.q[jointNumber - 1] = angle;
    switch (g_machine->consoleJogTo(target)) {
        case OpResult::kSuccess:
            io.printf("#J:OK %ld %.1f\n", jointNumber, angle);
            break;
        case OpResult::kInvalid:
            io.println(F("#J:ERR outside joint limits"));
            break;
        default:
            io.println(F("#J:ERR rejected (manual mode, obstacle, or a remote command is running)"));
            break;
    }
}

// Returns true if the line was one of the HMI commands. `line` still has
// its original case, so SSIDs and passwords are not altered.
bool handle(char *line, Stream &io) {
    if (strcasecmp(line, "INFO") == 0) {
        printInfo(io);
        return true;
    }
    if (strncasecmp(line, "J ", 2) == 0) {
        jogJoint(line + 2, io);
        return true;
    }
    if (strncasecmp(line, "SET ", 4) == 0) {
        char *key = line + 4;
        while (*key == ' ') key++;
        char *value = strchr(key, ' ');
        if (value) {
            *value++ = '\0';  // the value is the rest of the line, spaces included
        } else {
            value = key + strlen(key);  // empty value
        }
        const char *error = settings::set(key, value);
        if (error) {
            io.printf("#CFG:ERR %s\n", error);
        } else {
            io.printf("#CFG:OK %s stored, REBOOT to apply\n", key);
        }
        return true;
    }
    if (strcasecmp(line, "FACTORY") == 0) {
        settings::clear();
        io.println(F("#CFG:OK stored settings cleared, REBOOT to apply"));
        return true;
    }
    if (strcasecmp(line, "REBOOT") == 0) {
        io.println(F("#CFG:OK rebooting"));
        io.flush();
        g_machine->consoleStop();
        delay(200);
        ESP.restart();
        return true;
    }
    return false;
}

}  // namespace

namespace hmi {

void begin(SerialConsole &console, ArmDriver &driver, ArmMachine &machine,
           const char *appName, InfoExtraFn extra) {
    g_driver = &driver;
    g_machine = &machine;
    g_appName = appName;
    g_extra = std::move(extra);
    console.setExtension(handle);
}

}  // namespace hmi
