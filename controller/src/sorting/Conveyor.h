// Conveyor stepper for interface 4.4 move conveyor.
//
// Step pulses come from an esp_timer, so a move never blocks a task. One
// move runs at a time. It is an AuxAxis of the ArmMachine, which applies the
// permit / one-command-at-a-time rules and stops it together with the arm.
#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <functional>

#include "ArmMachine.h"

class Conveyor : public AuxAxis {
public:
    // completed = true when the full distance was moved, false when the
    // move was aborted by stop(). Runs in the conveyor task (completed) or
    // in the task that called stop(). Called exactly once per started move.
    using DoneFn = std::function<void(bool completed)>;

    // enablePin: active low, -1 if not wired. The driver is enabled only
    // while the belt moves.
    void begin(int stepPin, int dirPin, int enablePin, float stepsPerMm, float speedMmPerS,
               bool dirLevelForTrue);

    // Starts a move. Returns false (and does not call `done`) if a move is
    // already running. A distance of 0 completes at once.
    bool start(bool direction, uint32_t distanceMm, DoneFn done);

    bool busy() override;
    void stop() override;

private:
    struct Lock;
    static void onTimer(void *self);
    static void taskEntry(void *self);
    void taskLoop();
    void finish(bool completed);
    void setEnabled(bool on);

    int stepPin_ = -1;
    int dirPin_ = -1;
    int enablePin_ = -1;
    float stepsPerMm_ = 1.0f;
    uint64_t halfPeriodUs_ = 1000;
    bool dirLevelForTrue_ = true;

    esp_timer_handle_t timer_ = nullptr;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;

    // Written by start() while the timer is stopped, then only by onTimer().
    volatile uint32_t edgesLeft_ = 0;
    volatile bool stepLevel_ = false;

    bool pending_ = false;
    DoneFn done_;
};
