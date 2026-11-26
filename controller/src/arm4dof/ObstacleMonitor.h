// Ultrasonic obstacle monitor (HC-SR04 style sensor on the wrist).
// A 20 Hz task measures the distance, median-filters it and raises or
// clears the obstacle alarm with hysteresis.
#pragma once

#include <Arduino.h>
#include <functional>

class ObstacleMonitor {
public:
    // Called from the monitor task when the alarm is raised or cleared.
    using AlarmFn = std::function<void(bool active)>;

    void begin(int trigPin, int echoPin, AlarmFn onAlarm);

    // Median-filtered distance in mm, -1 = nothing in range (interface 4.8).
    int distanceMm() const { return distance_; }
    bool alarmActive() const { return alarm_; }

private:
    static void taskEntry(void *self);
    static void IRAM_ATTR echoIsr(void *self);
    void taskLoop();
    int measureOnce();

    static const int kWindow = 5;

    int trigPin_ = -1;
    int echoPin_ = -1;
    AlarmFn onAlarm_;
    volatile uint32_t riseUs_ = 0;
    volatile uint32_t widthUs_ = 0;
    volatile bool gotEcho_ = false;
    int samples_[kWindow];
    int next_ = 0;
    volatile int distance_ = -1;
    volatile bool alarm_ = false;
};
