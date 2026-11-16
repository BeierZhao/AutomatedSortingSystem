#include "ObstacleMonitor.h"

#include <algorithm>
#include <limits.h>

#include "Arm4dofConfig.h"

using namespace arm4dof;

namespace {
const uint32_t kPeriodMs = 50;     // 20 Hz
const uint32_t kEchoWaitMs = 30;   // covers kObstacleMaxRangeMm (about 23 ms)
const float kMmPerUs = 0.343f / 2.0f;  // speed of sound, out and back
}  // namespace

void ObstacleMonitor::begin(int trigPin, int echoPin, AlarmFn onAlarm) {
    trigPin_ = trigPin;
    echoPin_ = echoPin;
    onAlarm_ = std::move(onAlarm);
    for (int i = 0; i < kWindow; i++) samples_[i] = -1;

    pinMode(trigPin_, OUTPUT);
    digitalWrite(trigPin_, LOW);
    pinMode(echoPin_, INPUT);
    // The echo pulse is timed by interrupt, so the task never busy-waits.
    attachInterruptArg(echoPin_, echoIsr, this, CHANGE);

    xTaskCreatePinnedToCore(taskEntry, "obstacle", 4096, this, 4, nullptr, 1);
}

void IRAM_ATTR ObstacleMonitor::echoIsr(void *arg) {
    ObstacleMonitor *self = static_cast<ObstacleMonitor *>(arg);
    const uint32_t now = micros();
    if (digitalRead(self->echoPin_)) {
        self->riseUs_ = now;
    } else {
        self->widthUs_ = now - self->riseUs_;
        self->gotEcho_ = true;
    }
}

// One measurement, in mm, or -1 if no echo came back within range.
int ObstacleMonitor::measureOnce() {
    gotEcho_ = false;
    digitalWrite(trigPin_, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin_, LOW);
    vTaskDelay(pdMS_TO_TICKS(kEchoWaitMs));

    if (!gotEcho_) return -1;
    const int mm = (int)(widthUs_ * kMmPerUs);
    return mm > kObstacleMaxRangeMm ? -1 : mm;
}

void ObstacleMonitor::taskEntry(void *self) {
    static_cast<ObstacleMonitor *>(self)->taskLoop();
}

void ObstacleMonitor::taskLoop() {
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        samples_[next_] = measureOnce();
        next_ = (next_ + 1) % kWindow;

        // Median of the last five readings. "Nothing in range" sorts as far.
        int sorted[kWindow];
        for (int i = 0; i < kWindow; i++) {
            sorted[i] = samples_[i] < 0 ? INT_MAX : samples_[i];
        }
        std::sort(sorted, sorted + kWindow);
        const int median = sorted[kWindow / 2];
        distance_ = median == INT_MAX ? -1 : median;

        const bool wasActive = alarm_;
        if (!wasActive && median < kObstacleStopMm) {
            alarm_ = true;
        } else if (wasActive && median > kObstacleClearMm) {
            alarm_ = false;
        }
        if (alarm_ != wasActive && onAlarm_) onAlarm_(alarm_);

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kPeriodMs));
    }
}
