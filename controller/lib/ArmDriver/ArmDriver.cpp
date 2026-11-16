#include "ArmDriver.h"

#include "ArmKinematics.h"

using namespace arm;

namespace {
struct Lock {
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    SemaphoreHandle_t m_;
};
}  // namespace

void ArmDriver::begin(const ArmConfig &cfg, const ArmHardware &hw) {
    cfg_ = cfg;
    hw_ = hw;
    mutex_ = xSemaphoreCreateMutex();

    // The servos have no feedback, so the arm jumps to the home pose at
    // power-up, as it did with ArmTest.
    interp_.reset(cfg_.home);

    for (int t = 0; t < 4; t++) ESP32PWM::allocateTimer(t);
    for (int j = 0; j < kJointCount; j++) {
        servo_[j].setPeriodHertz(50);
        servo_[j].attach(hw_.pin[j], hw_.pulseMinUs, hw_.pulseMaxUs);
    }
    writeServos(cfg_.home);

    xTaskCreatePinnedToCore(taskEntry, "motion", 4096, this, 5, nullptr, 1);
}

void ArmDriver::writeServos(const JointAngles &q) {
    for (int j = 0; j < kJointCount; j++) {
        // Per-joint limits are enforced here as the last line of defence.
        const JointConfig &jc = cfg_.joint[j];
        float phys = constrain(q.q[j], jc.physMin, jc.physMax);
        float servo = constrain(physToServo(cfg_, j, phys), jc.servoMin, jc.servoMax);
        const float us = hw_.pulseMinUs + servo / 180.0f * (hw_.pulseMaxUs - hw_.pulseMinUs);
        servo_[j].writeMicroseconds((int)lroundf(us));
    }
}

bool ArmDriver::startMove(const JointAngles &target, DoneFn done,
                          const float *wristHeadingDeg) {
    JointAngles checked = target;
    if (wristHeadingDeg) checked.q[kWrist] = 0.0f;  // ignored by the move
    for (int j = 0; j < kJointCount; j++) {
        if (j == kWrist && wristHeadingDeg) continue;
        if (checkJoint(cfg_, j, checked.q[j]) != kIkOk) return false;
    }

    Lock lock(mutex_);
    if (pending_) return false;
    interp_.moveTo(cfg_, target, wristHeadingDeg);
    done_ = std::move(done);
    pending_ = true;
    ticksSinceStart_ = 0;
    return true;
}

bool ArmDriver::streamTo(const JointAngles &target) {
    if (checkJoints(cfg_, target) != kIkOk) return false;
    Lock lock(mutex_);
    if (pending_) return false;
    interp_.streamTo(cfg_, target);
    return true;
}

void ArmDriver::stop() {
    DoneFn done;
    {
        Lock lock(mutex_);
        interp_.halt();
        if (pending_) {
            pending_ = false;
            done = std::move(done_);
            done_ = nullptr;
        }
    }
    if (done) done(false);
}

bool ArmDriver::commandPending() {
    Lock lock(mutex_);
    return pending_;
}

bool ArmDriver::isMoving() {
    Lock lock(mutex_);
    return !interp_.arrived();
}

JointAngles ArmDriver::current() {
    Lock lock(mutex_);
    return interp_.current();
}

JointAngles ArmDriver::target() {
    Lock lock(mutex_);
    return interp_.target();
}

void ArmDriver::taskEntry(void *self) { static_cast<ArmDriver *>(self)->taskLoop(); }

void ArmDriver::taskLoop() {
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kPeriodMs));

        DoneFn done;
        JointAngles q;
        {
            Lock lock(mutex_);
            const bool arrived = interp_.step(kPeriodMs / 1000.0f);
            q = interp_.current();
            if (pending_ && ticksSinceStart_ < kMinCommandTicks) ticksSinceStart_++;
            // A command never completes in under two ticks, so the caller of
            // startMove() has returned before its callback can run here.
            if (arrived && pending_ && ticksSinceStart_ >= kMinCommandTicks) {
                pending_ = false;
                done = std::move(done_);
                done_ = nullptr;
            }
        }
        writeServos(q);
        if (done) done(true);
    }
}
