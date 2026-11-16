// Servo output and the 50 Hz motion task.
//
// The driver knows nothing about permits, modes or HTTP. It moves the four
// joints toward a target at a limited speed, one command at a time, and
// tells the caller when that command completed or was aborted.
#pragma once

#include <Arduino.h>
#include <ESP32Servo.h>
#include <functional>

#include "ArmConfig.h"
#include "JointInterpolator.h"

struct ArmHardware {
    int pin[arm::kJointCount];
    // Pulse widths for servo angle 0 and 180. The defaults are those of the
    // Arduino Servo library on the Nano, which the ArmTest calibration
    // (offsets, limits, home pose) was done with.
    int pulseMinUs = 544;
    int pulseMaxUs = 2400;
};

class ArmDriver {
public:
    // Called exactly once per accepted command: completed = true when the arm
    // arrived, false when the command was aborted by stop().
    // Runs in the motion task (completed) or in the task that called stop().
    using DoneFn = std::function<void(bool completed)>;

    static const uint32_t kPeriodMs = 20;  // 50 Hz

    // Attaches the servos, drives them to the home pose and starts the
    // motion task.
    void begin(const arm::ArmConfig &cfg, const ArmHardware &hw);

    // Starts a coordinated move. Returns false (and does not call `done`)
    // if a command is already pending or the target is outside the limits.
    // See JointInterpolator::moveTo for wristHeadingDeg.
    bool startMove(const arm::JointAngles &target, DoneFn done,
                   const float *wristHeadingDeg = nullptr);

    // Streaming target for jogging (serial console, joystick). No completion
    // callback. Returns false if a command is pending or the target is
    // outside the limits.
    bool streamTo(const arm::JointAngles &target);

    // Freezes the arm at its current interpolated position. A pending
    // command is aborted and its callback runs before stop() returns.
    void stop();

    bool commandPending();
    bool isMoving();
    arm::JointAngles current();
    arm::JointAngles target();
    bool attached(int joint) { return servo_[joint].attached(); }
    const ArmHardware &hardware() const { return hw_; }

private:
    static void taskEntry(void *self);
    void taskLoop();
    void writeServos(const arm::JointAngles &q);

    arm::ArmConfig cfg_;
    ArmHardware hw_;
    Servo servo_[arm::kJointCount];
    arm::JointInterpolator interp_;
    SemaphoreHandle_t mutex_ = nullptr;
    DoneFn done_;
    bool pending_ = false;
    static const uint8_t kMinCommandTicks = 2;
    uint8_t ticksSinceStart_ = 0;
};
