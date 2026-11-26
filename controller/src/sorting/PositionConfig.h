// Position config of interface 4.1 update position config: the world-frame
// coordinates of each repository and of the part dock, plus the
// destination-to-repository map. Kept in RAM and in the ESP32's flash (NVS),
// so it survives a reboot.
//
// Body (docs/development_guide.md, section 5.1):
//   {"repositories": {"Repo1": {"x": 50, "y": 50, "z": 0}, ...},
//    "destinations": {"Hamilton": "Repo1", "none": "Repo4", ...},
//    "part_dock": {"x": 10, "y": 10, "z": 10}}
//
// All methods are safe to call from any task.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <utility>
#include <vector>

struct Point3 {
    float x, y, z;
};

class PositionConfig {
public:
    static const size_t kMaxRepositories = 16;
    static const size_t kMaxDestinations = 64;
    static const size_t kMaxNameLength = 40;

    // Reads the stored config. Call once at the start of setup().
    void begin();

    // 4.1: checks the body and, if it is complete and consistent, replaces
    // the config in RAM and in flash. Returns `operation done`. On false,
    // `error` says why and the previous config stays in use.
    bool update(JsonObjectConst body, String &error);

    // True once a config has been stored (now or at an earlier boot).
    bool valid();

    // ---- Lookups for the sequence control ----
    bool partDock(Point3 &out);
    // The repository for a destination text as read by the vision system
    // (case-sensitive, so the spelling must match the OCR output). False if
    // the destination is not mapped.
    bool repositoryFor(const String &destination, String &repository, Point3 &pos);

    // Counts for the serial panel's INFO line.
    void summary(JsonObject out);

private:
    struct Repository {
        String name;
        Point3 pos;
    };
    struct Data {
        std::vector<Repository> repositories;
        std::vector<std::pair<String, String>> destinations;  // destination -> repository
        Point3 partDock = {0, 0, 0};
        bool valid = false;
    };
    struct Lock;

    static bool parse(JsonObjectConst body, Data &out, String &error);
    static void toJson(const Data &data, JsonDocument &doc);

    SemaphoreHandle_t mutex_ = nullptr;
    Data data_;
};
