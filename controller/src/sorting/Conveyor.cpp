#include "Conveyor.h"

#include <math.h>

struct Conveyor::Lock {
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    SemaphoreHandle_t m_;
};

namespace {
// Guards edgesLeft_ / stepLevel_ between the timer callback and stop().
portMUX_TYPE g_stepMux = portMUX_INITIALIZER_UNLOCKED;
// esp_timer does not go below 50 us per period.
const uint64_t kMinHalfPeriodUs = 50;
}  // namespace

void Conveyor::begin(int stepPin, int dirPin, int enablePin, float stepsPerMm,
                     float speedMmPerS, bool dirLevelForTrue) {
    stepPin_ = stepPin;
    dirPin_ = dirPin;
    enablePin_ = enablePin;
    stepsPerMm_ = stepsPerMm;
    dirLevelForTrue_ = dirLevelForTrue;
    const double stepsPerS = (double)speedMmPerS * stepsPerMm;
    halfPeriodUs_ = (uint64_t)(500000.0 / stepsPerS);
    if (halfPeriodUs_ < kMinHalfPeriodUs) halfPeriodUs_ = kMinHalfPeriodUs;

    pinMode(stepPin_, OUTPUT);
    digitalWrite(stepPin_, LOW);
    pinMode(dirPin_, OUTPUT);
    if (enablePin_ >= 0) pinMode(enablePin_, OUTPUT);
    setEnabled(false);

    mutex_ = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(taskEntry, "conveyor", 4096, this, 3, &task_, 1);

    esp_timer_create_args_t args = {};
    args.callback = onTimer;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "conveyor";
    esp_timer_create(&args, &timer_);
}

void Conveyor::setEnabled(bool on) {
    if (enablePin_ >= 0) digitalWrite(enablePin_, on ? LOW : HIGH);
}

bool Conveyor::start(bool direction, uint32_t distanceMm, DoneFn done) {
    Lock lock(mutex_);
    if (pending_) return false;
    const uint32_t steps = (uint32_t)lround((double)distanceMm * stepsPerMm_);
    pending_ = true;
    done_ = std::move(done);
    Serial.printf("[%lu] Conveyor: %s %lu mm (%lu steps)\n", millis(),
                  direction ? "forward" : "backward", (unsigned long)distanceMm,
                  (unsigned long)steps);

    if (steps == 0) {
        xTaskNotifyGive(task_);  // completes in the conveyor task
        return true;
    }
    digitalWrite(dirPin_, direction == dirLevelForTrue_ ? HIGH : LOW);
    setEnabled(true);
    portENTER_CRITICAL(&g_stepMux);
    edgesLeft_ = steps * 2;
    stepLevel_ = false;
    portEXIT_CRITICAL(&g_stepMux);
    // The first edge comes one half period later, well after the DIR and
    // EN setup times of common drivers.
    esp_timer_start_periodic(timer_, halfPeriodUs_);
    return true;
}

bool Conveyor::busy() {
    Lock lock(mutex_);
    return pending_;
}

void Conveyor::stop() {
    esp_timer_stop(timer_);  // error if not running, which is fine
    portENTER_CRITICAL(&g_stepMux);
    edgesLeft_ = 0;
    stepLevel_ = false;
    portEXIT_CRITICAL(&g_stepMux);
    digitalWrite(stepPin_, LOW);
    finish(false);
}

void Conveyor::onTimer(void *arg) {
    Conveyor *self = static_cast<Conveyor *>(arg);
    bool last = false;
    portENTER_CRITICAL(&g_stepMux);
    if (self->edgesLeft_ > 0) {
        self->stepLevel_ = !self->stepLevel_;
        gpio_set_level((gpio_num_t)self->stepPin_, self->stepLevel_ ? 1 : 0);
        last = --self->edgesLeft_ == 0;
    }
    portEXIT_CRITICAL(&g_stepMux);
    if (last) {
        esp_timer_stop(self->timer_);
        xTaskNotifyGive(self->task_);
    }
}

void Conveyor::taskEntry(void *self) { static_cast<Conveyor *>(self)->taskLoop(); }

void Conveyor::taskLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // A notification left over from a move that stop() aborted must not
        // complete the next move: that one still has edges to go.
        portENTER_CRITICAL(&g_stepMux);
        const bool arrived = edgesLeft_ == 0;
        portEXIT_CRITICAL(&g_stepMux);
        if (arrived) finish(true);
    }
}

void Conveyor::finish(bool completed) {
    DoneFn done;
    {
        Lock lock(mutex_);
        if (!pending_) return;
        pending_ = false;
        done = std::move(done_);
        done_ = nullptr;
        setEnabled(false);
    }
    Serial.printf("[%lu] Conveyor: %s\n", millis(), completed ? "arrived" : "stopped");
    if (done) done(completed);
}
