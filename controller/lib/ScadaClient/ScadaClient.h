// Outbound queue for the /scada/... reports (interfaces 2.1, 2.2 and 2.3).
//
// Reports are queued by any task and sent in order by one dedicated task,
// with retry and back-off. After the gateway was unreachable, the current
// status and every active alarm are sent again so the HMI resynchronises.
// When the queue is full the oldest status report is dropped. Alarm reports
// and sorting results are never dropped. Sorting results are events, so
// they are not sent again after a reconnect (only retried until accepted).
#pragma once

#include <Arduino.h>
#include <deque>
#include <vector>

// Inputs of 2.3 report sorting result. The first four are passed through
// from 3.1 read part label.
struct SortingResult {
    String readingResult;  // "success", "info lost", "none", "fault"
    String origin;
    String destination;
    String partId;
    String repository;     // "Repo<N>" or "manual"
    bool sortingResult;    // true = placed
};

class ScadaClient {
public:
    // Starts the sender task. `host` and the paths must stay valid (use
    // string literals or constants). The paths are where the status, alarm
    // and sorting result reports are POSTed on the receiver.
    void begin(const char *host, uint16_t port,
               const char *statusPath = "/scada/report-current-status",
               const char *alarmPath = "/scada/report-alarm",
               const char *resultPath = "/scada/report-sorting-result");

    // 2.1 report current status. Never blocks.
    void reportStatus(const char *status);
    // 2.2 report alarm. Never blocks.
    void reportAlarm(const char *message, bool active);
    // 2.3 report sorting result. Never blocks.
    void reportSortingResult(const SortingResult &result);

    static const size_t kMaxQueue = 32;

private:
    enum class Kind : uint8_t { kStatus, kAlarm, kResult };
    struct Event {
        uint32_t seq;
        Kind kind;
        String text;  // status, alarm message, or the JSON body of a result
        bool active;  // alarm status
    };

    static void taskEntry(void *self);
    void taskLoop();
    void enqueueLocked(Kind kind, const String &text, bool active);
    void wake();
    bool send(const Event &e);

    const char *host_ = nullptr;
    uint16_t port_ = 0;
    const char *statusPath_ = nullptr;
    const char *alarmPath_ = nullptr;
    const char *resultPath_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::deque<Event> queue_;
    uint32_t nextSeq_ = 0;
    String currentStatus_;
    std::vector<String> activeAlarms_;
};
