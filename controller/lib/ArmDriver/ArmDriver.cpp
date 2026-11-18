#include "ArmDriver.h"

#include "ArmKinematics.h"

using namespace arm;

namespace {
const uint32_t kPwmHz = 50;   // 20 ms frame
const uint8_t kPwmBits = 14;  // the ESP32-S3 LEDC maximum
const float kFrameUs = 1000000.0f / kPwmHz;

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

    // LEDC channel j drives joint j, the same setup as test/calib_servo.
    for (int j = 0; j < kJointCount; j++) {
        attached_[j] = ledcSetup(j, kPwmHz, kPwmBits) > 0;
        if (attached_[j]) ledcAttachPin(hw_.pin[j], j);
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
        writePulseUs(j, us);
    }
}

void ArmDriver::writePulseUs(int joint, float us) {
    if (!attached_[joint]) return;
    const uint32_t duty = (uint32_t)lroundf(us / kFrameUs * (float)(1u << kPwmBits));
    // Write only on change: the motion task runs once per PWM period and
    // most ticks leave the pulse unchanged.
    if (duty == lastDuty_[joint]) return;
    lastDuty_[joint] = duty;
    ledcWrite(joint, duty);
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
