#include "Settings.h"

#include <Preferences.h>

#if __has_include("NetConfig.h")
#include "NetConfig.h"
#else
#include "NetConfig.example.h"
#endif

// For NetConfig.h files made before the report paths existed.
#ifndef NET_SCADA_STATUS_PATH
#define NET_SCADA_STATUS_PATH "/scada/report-current-status"
#endif
#ifndef NET_SCADA_ALARM_PATH
#define NET_SCADA_ALARM_PATH "/scada/report-alarm"
#endif

namespace {
const char *const kNamespace = "armcfg";
// The serial console takes lines of up to 127 characters, "SET STPATH " included.
const size_t kMaxPathLength = 100;
DeviceSettings g_active;

// A report path: starts with '/', printable ASCII, no spaces.
const char *checkPath(const char *value) {
    const size_t len = strlen(value);
    if (value[0] != '/') return "path must start with /";
    if (len > kMaxPathLength) return "path must be at most 100 characters";
    for (const char *c = value; *c; c++) {
        if (*c <= ' ' || *c > '~') return "path must not contain spaces or special characters";
    }
    return nullptr;
}

// Stores a path, or forgets it when `value` is empty.
const char *setPath(Preferences &prefs, const char *nvsKey, const char *value) {
    if (*value == '\0') {
        if (prefs.isKey(nvsKey)) prefs.remove(nvsKey);
        return nullptr;
    }
    const char *error = checkPath(value);
    if (!error) prefs.putString(nvsKey, value);
    return error;
}

DeviceSettings read() {
    DeviceSettings s;
    Preferences prefs;
    // Read-write open, so the namespace is created on the first boot.
    prefs.begin(kNamespace, false);
    s.wifiSsid = prefs.getString("ssid", NET_WIFI_SSID);
    s.wifiPassword = prefs.getString("pass", NET_WIFI_PASSWORD);
    s.scadaHost = prefs.getString("gwhost", NET_SCADA_HOST);
    s.scadaPort = prefs.getUShort("gwport", NET_SCADA_PORT);
    s.scadaStatusPath = prefs.getString("stpath", NET_SCADA_STATUS_PATH);
    s.scadaAlarmPath = prefs.getString("alpath", NET_SCADA_ALARM_PATH);
    prefs.end();
    return s;
}
}  // namespace

namespace settings {

void load() { g_active = read(); }

const DeviceSettings &active() { return g_active; }

DeviceSettings stored() { return read(); }

const char *set(const char *key, const char *value) {
    const size_t len = strlen(value);
    Preferences prefs;
    const char *error = nullptr;
    prefs.begin(kNamespace, false);
    if (strcasecmp(key, "SSID") == 0) {
        if (len < 1 || len > 32) {
            error = "SSID must be 1 to 32 characters";
        } else {
            prefs.putString("ssid", value);
        }
    } else if (strcasecmp(key, "PASS") == 0) {
        // Empty = open network. WPA2 needs 8 to 63 characters.
        if (len != 0 && (len < 8 || len > 63)) {
            error = "password must be empty or 8 to 63 characters";
        } else {
            prefs.putString("pass", value);
        }
    } else if (strcasecmp(key, "GWHOST") == 0) {
        if (len < 1 || len > 63) {
            error = "gateway host must be 1 to 63 characters";
        } else {
            prefs.putString("gwhost", value);
        }
    } else if (strcasecmp(key, "GWPORT") == 0) {
        const long port = atol(value);
        if (port < 1 || port > 65535) {
            error = "gateway port must be 1 to 65535";
        } else {
            prefs.putUShort("gwport", (uint16_t)port);
        }
    } else if (strcasecmp(key, "STPATH") == 0) {
        error = setPath(prefs, "stpath", value);
    } else if (strcasecmp(key, "ALPATH") == 0) {
        error = setPath(prefs, "alpath", value);
    } else {
        error = "unknown key (use SSID, PASS, GWHOST, GWPORT, STPATH, ALPATH)";
    }
    prefs.end();
    return error;
}

void clear() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.clear();
    prefs.end();
}

}  // namespace settings
