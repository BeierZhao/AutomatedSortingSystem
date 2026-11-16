#include "Settings.h"

#include <Preferences.h>

#if __has_include("NetConfig.h")
#include "NetConfig.h"
#else
#include "NetConfig.example.h"
#endif

namespace {
const char *const kNamespace = "armcfg";
DeviceSettings g_active;

DeviceSettings read() {
    DeviceSettings s;
    Preferences prefs;
    // Read-write open, so the namespace is created on the first boot.
    prefs.begin(kNamespace, false);
    s.wifiSsid = prefs.getString("ssid", NET_WIFI_SSID);
    s.wifiPassword = prefs.getString("pass", NET_WIFI_PASSWORD);
    s.scadaHost = prefs.getString("gwhost", NET_SCADA_HOST);
    s.scadaPort = prefs.getUShort("gwport", NET_SCADA_PORT);
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
    } else {
        error = "unknown key (use SSID, PASS, GWHOST, GWPORT)";
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
