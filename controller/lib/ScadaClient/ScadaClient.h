// Outbound queue for the /scada/... reports (interfaces 2.1 and 2.2).
//
// Reports are queued by any task and sent in order by one dedicated task,
// with retry and back-off. After the gateway was unreachable, the current
// status and every active alarm are sent again so the HMI resynchronises.
// When the queue is full the oldest status report is dropped. Alarm reports
// are never dropped.
#pragma once

#include <Arduino.h>
#include <deque>
#include <vector>

class ScadaClient {
public:
    // Starts the sender task. `host` must stay valid (use a string literal
    // or a constant).
    void begin(const char *host, uint16_t port);

    // 2.1 report current status. Never blocks.
    void reportStatus(const char *status);
    // 2.2 report alarm. Never blocks.
    void reportAlarm(const char *message, bool active);

    static const size_t kMaxQueue = 32;

private:
    struct Event {
        uint32_t seq;
        bool isAlarm;
        String text;  // status, or alarm message
        bool active;  // alarm status
    };

    static void taskEntry(void *self);
    void taskLoop();
    void enqueueLocked(bool isAlarm, const String &text, bool active);
    bool send(const Event &e);

    const char *host_ = nullptr;
    uint16_t port_ = 0;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::deque<Event> queue_;
    uint32_t nextSeq_ = 0;
    String currentStatus_;
    std::vector<String> activeAlarms_;
};
