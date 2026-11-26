#include "ScadaClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

namespace {
const uint32_t kConnectTimeoutMs = 1500;
const uint32_t kResponseTimeoutMs = 2000;
const uint32_t kBackoffMinMs = 500;
const uint32_t kBackoffMaxMs = 5000;

struct Lock {
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    SemaphoreHandle_t m_;
};
}  // namespace

void ScadaClient::begin(const char *host, uint16_t port, const char *statusPath,
                        const char *alarmPath) {
    host_ = host;
    port_ = port;
    statusPath_ = statusPath;
    alarmPath_ = alarmPath;
    mutex_ = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(taskEntry, "scada", 8192, this, 2, &task_, 0);
}

void ScadaClient::enqueueLocked(bool isAlarm, const String &text, bool active) {
    if (queue_.size() >= kMaxQueue) {
        // Drop the oldest status report. Alarms are never dropped.
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (!it->isAlarm) {
                queue_.erase(it);
                break;
            }
        }
    }
    queue_.push_back(Event{nextSeq_++, isAlarm, text, active});
}

void ScadaClient::reportStatus(const char *status) {
    {
        Lock lock(mutex_);
        currentStatus_ = status;
        enqueueLocked(false, currentStatus_, false);
    }
    if (task_) xTaskNotifyGive(task_);
}

void ScadaClient::reportAlarm(const char *message, bool active) {
    {
        Lock lock(mutex_);
        const String text(message);
        auto it = activeAlarms_.begin();
        while (it != activeAlarms_.end() && *it != text) ++it;
        if (active && it == activeAlarms_.end()) activeAlarms_.push_back(text);
        if (!active && it != activeAlarms_.end()) activeAlarms_.erase(it);
        enqueueLocked(true, text, active);
    }
    if (task_) xTaskNotifyGive(task_);
}

bool ScadaClient::send(const Event &e) {
    JsonDocument doc;
    const char *path;
    if (e.isAlarm) {
        path = alarmPath_;
        doc["alarm_message"] = e.text;
        doc["alarm_status"] = e.active;
    } else {
        path = statusPath_;
        doc["status"] = e.text;
    }
    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kResponseTimeoutMs);
    if (!http.begin(host_, port_, path)) return false;
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(body);
    http.end();

    Serial.printf("[%lu] SCADA -> %s %s : %d\n", millis(), path, body.c_str(), code);
    return code >= 200 && code < 300;
}

void ScadaClient::taskEntry(void *self) { static_cast<ScadaClient *>(self)->taskLoop(); }

void ScadaClient::taskLoop() {
    uint32_t backoffMs = kBackoffMinMs;
    bool wasOffline = false;

    for (;;) {
        Event head;
        bool have = false;
        {
            Lock lock(mutex_);
            if (!queue_.empty()) {
                head = queue_.front();
                have = true;
            }
        }
        if (!have) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const bool ok = WiFi.status() == WL_CONNECTED && send(head);
        if (!ok) {
            wasOffline = true;
            vTaskDelay(pdMS_TO_TICKS(backoffMs));
            backoffMs = min(backoffMs * 2, kBackoffMaxMs);
            continue;
        }

        backoffMs = kBackoffMinMs;
        Lock lock(mutex_);
        // The head may have been dropped meanwhile to make room.
        if (!queue_.empty() && queue_.front().seq == head.seq) queue_.pop_front();
        if (wasOffline) {
            // Back online: resend the current state after what is queued.
            wasOffline = false;
            if (currentStatus_.length()) enqueueLocked(false, currentStatus_, false);
            for (const String &alarm : activeAlarms_) enqueueLocked(true, alarm, true);
        }
    }
}
