#include "PositionConfig.h"

#include <Preferences.h>
#include <math.h>

struct PositionConfig::Lock {
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    SemaphoreHandle_t m_;
};

namespace {
const char *const kNamespace = "sortcfg";
const char *const kKey = "poscfg";

// {"x": n, "y": n, "z": n} with finite numbers.
bool readPoint(JsonVariantConst v, Point3 &out) {
    if (!v.is<JsonObjectConst>()) return false;
    if (!v["x"].is<float>() || !v["y"].is<float>() || !v["z"].is<float>()) return false;
    out = {v["x"].as<float>(), v["y"].as<float>(), v["z"].as<float>()};
    return isfinite(out.x) && isfinite(out.y) && isfinite(out.z);
}

// "Repo<N>" with N a positive integer without leading zeros (spec 2.3).
bool isRepositoryName(const char *name) {
    if (strncmp(name, "Repo", 4) != 0) return false;
    const char *n = name + 4;
    if (*n < '1' || *n > '9') return false;
    for (; *n; n++) {
        if (*n < '0' || *n > '9') return false;
    }
    return strlen(name) <= 12;
}
}  // namespace

bool PositionConfig::parse(JsonObjectConst body, Data &out, String &error) {
    out = Data();

    JsonObjectConst repos = body["repositories"];
    if (repos.isNull() || repos.size() == 0) {
        error = "repositories: object with at least one entry required";
        return false;
    }
    if (repos.size() > kMaxRepositories) {
        error = "repositories: at most " + String(kMaxRepositories) + " entries";
        return false;
    }
    for (JsonPairConst kv : repos) {
        const char *name = kv.key().c_str();
        if (!isRepositoryName(name)) {
            error = String("repositories: name \"") + name + "\" is not Repo<N>";
            return false;
        }
        Repository r;
        r.name = name;
        if (!readPoint(kv.value(), r.pos)) {
            error = String("repositories.") + name + ": x, y, z (numbers) required";
            return false;
        }
        out.repositories.push_back(r);
    }

    JsonVariantConst destVar = body["destinations"];
    if (!destVar.is<JsonObjectConst>()) {
        error = "destinations: object required";
        return false;
    }
    JsonObjectConst dests = destVar.as<JsonObjectConst>();
    if (dests.size() > kMaxDestinations) {
        error = "destinations: at most " + String(kMaxDestinations) + " entries";
        return false;
    }
    for (JsonPairConst kv : dests) {
        const char *dest = kv.key().c_str();
        const size_t len = strlen(dest);
        if (len == 0 || len > kMaxNameLength) {
            error = "destinations: names must be 1 to " + String(kMaxNameLength) + " characters";
            return false;
        }
        if (!kv.value().is<const char *>()) {
            error = String("destinations.") + dest + ": repository name (string) required";
            return false;
        }
        const char *repo = kv.value().as<const char *>();
        bool known = false;
        for (const Repository &r : out.repositories) known = known || r.name == repo;
        if (!known) {
            error = String("destinations.") + dest + ": \"" + repo + "\" is not in repositories";
            return false;
        }
        out.destinations.emplace_back(String(dest), String(repo));
    }

    if (!readPoint(body["part_dock"], out.partDock)) {
        error = "part_dock: x, y, z (numbers) required";
        return false;
    }
    out.valid = true;
    return true;
}

void PositionConfig::toJson(const Data &data, JsonDocument &doc) {
    JsonObject root = doc.to<JsonObject>();
    JsonObject repos = root["repositories"].to<JsonObject>();
    for (const Repository &r : data.repositories) {
        JsonObject p = repos[r.name].to<JsonObject>();
        p["x"] = r.pos.x;
        p["y"] = r.pos.y;
        p["z"] = r.pos.z;
    }
    JsonObject dests = root["destinations"].to<JsonObject>();
    for (const auto &d : data.destinations) dests[d.first] = d.second;
    JsonObject dock = root["part_dock"].to<JsonObject>();
    dock["x"] = data.partDock.x;
    dock["y"] = data.partDock.y;
    dock["z"] = data.partDock.z;
}

void PositionConfig::begin() {
    mutex_ = xSemaphoreCreateMutex();

    Preferences prefs;
    prefs.begin(kNamespace, false);
    String text;
    if (prefs.isKey(kKey)) {
        const size_t len = prefs.getBytesLength(kKey);
        std::vector<char> buf(len + 1, '\0');
        prefs.getBytes(kKey, buf.data(), len);
        text = buf.data();
    }
    prefs.end();

    if (text.isEmpty()) {
        Serial.println(F("Position config: none stored yet"));
        return;
    }
    JsonDocument doc;
    Data data;
    String error;
    if (deserializeJson(doc, text) || !doc.is<JsonObjectConst>() ||
        !parse(doc.as<JsonObjectConst>(), data, error)) {
        Serial.printf("Position config: stored copy unreadable (%s), ignored\n", error.c_str());
        return;
    }
    Lock lock(mutex_);
    data_ = std::move(data);
    Serial.printf("Position config: %u repositories, %u destinations\n",
                  (unsigned)data_.repositories.size(), (unsigned)data_.destinations.size());
}

bool PositionConfig::update(JsonObjectConst body, String &error) {
    Data data;
    if (!parse(body, data, error)) return false;

    // Stored in the canonical form, so unknown fields are not kept.
    JsonDocument doc;
    toJson(data, doc);
    String text;
    serializeJson(doc, text);

    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "flash not available";
        return false;
    }
    const size_t written = prefs.putBytes(kKey, text.c_str(), text.length());
    prefs.end();
    if (written != text.length()) {
        error = "writing to flash failed";
        return false;
    }

    Lock lock(mutex_);
    data_ = std::move(data);
    return true;
}

bool PositionConfig::valid() {
    Lock lock(mutex_);
    return data_.valid;
}

bool PositionConfig::partDock(Point3 &out) {
    Lock lock(mutex_);
    if (!data_.valid) return false;
    out = data_.partDock;
    return true;
}

bool PositionConfig::repositoryFor(const String &destination, String &repository, Point3 &pos) {
    Lock lock(mutex_);
    for (const auto &d : data_.destinations) {
        if (d.first != destination) continue;
        for (const Repository &r : data_.repositories) {
            if (r.name == d.second) {
                repository = r.name;
                pos = r.pos;
                return true;
            }
        }
    }
    return false;
}

void PositionConfig::summary(JsonObject out) {
    Lock lock(mutex_);
    out["valid"] = data_.valid;
    out["repositories"] = data_.repositories.size();
    out["destinations"] = data_.destinations.size();
}
