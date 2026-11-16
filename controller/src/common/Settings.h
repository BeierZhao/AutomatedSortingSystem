// Network settings that can be changed without rebuilding.
//
// They are stored in the ESP32's flash (NVS) by the serial HMI panel. A
// value that was never stored falls back to include/NetConfig.h. Changes
// take effect at the next boot.
#pragma once

#include <Arduino.h>

struct DeviceSettings {
    String wifiSsid;
    String wifiPassword;
    String scadaHost;
    uint16_t scadaPort;
};

namespace settings {

// Reads the stored settings. Call once at the start of setup().
void load();

// The settings this boot is running with. Never changes after load().
const DeviceSettings &active();

// What is stored now, which is what the next boot will use.
DeviceSettings stored();

// Stores one value. Keys: SSID, PASS, GWHOST, GWPORT (case-insensitive).
// Returns nullptr on success, or an error text.
const char *set(const char *key, const char *value);

// Forgets all stored values, so the next boot uses NetConfig.h again.
void clear();

}  // namespace settings
