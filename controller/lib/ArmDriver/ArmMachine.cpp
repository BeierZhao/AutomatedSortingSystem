#include "ArmMachine.h"

#include <math.h>

using namespace arm;

struct ArmMachine::Lock {
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    SemaphoreHandle_t m_;
};

const char *opResultText(OpResult r) {
    switch (r) {
        case OpResult::kSuccess: return "success";
        case OpResult::kInvalid: return "invalid";
        default: return "rejected";
    }
}

void ArmMachine::begin(const ArmConfig &cfg, bool wristFollowsTravel) {
    cfg_ = cfg;
    wristFollowsTravel_ = wristFollowsTravel;
    mutex_ = xSemaphoreCreateMutex();
}

void ArmMachine::setStatus(const char *s, bool forceReport) {
    const bool changed = status_ != s;
    status_ = s;
    if ((changed || forceReport) && statusFn_) statusFn_(s);
}

bool ArmMachine::movesTcp(const JointAngles &from, const JointAngles &to) const {
    for (int j = kBase; j <= kElbow; j++) {
        if (fabsf(from.q[j] - to.q[j]) > 0.01f) return true;
    }
    return false;
}

// ---------------- Remote commands ----------------

OpResult ArmMachine::setArmPosition(float x, float y, float z, DoneFn done) {
    Lock lock(mutex_);
    if (manual_ || !permit_ || tcpBlocked_ || driver_.commandPending()) {
        return OpResult::kRejected;
    }

    const JointAngles now = driver_.current();
    JointAngles target;
    if (inverseKinematics(cfg_, x, y, z, now.q[kWrist], target) != kIkOk) {
        return OpResult::kInvalid;
    }

    // Wrist: face the horizontal direction of travel. A purely vertical
    // move has no such direction, so the wrist stays where it is.
    float heading = 0.0f;
    const float *headingPtr = nullptr;
    if (wristFollowsTravel_) {
        const Pose from = forwardKinematics(cfg_, now);
        const float dx = x - from.x;
        const float dy = y - from.y;
        if (dx * dx + dy * dy > 1.0f) {
            heading = atan2f(dy, dx) * 180.0f / (float)M_PI;
            headingPtr = &heading;
        }
    }

    const bool started = driver_.startMove(
        target,
        [done](bool completed) {
            if (done) done(completed ? OpResult::kSuccess : OpResult::kRejected);
        },
        headingPtr);
    return started ? OpResult::kPending : OpResult::kRejected;
}

OpResult ArmMachine::setJointPosition(int jointNumber, float angle, DoneFn done) {
    Lock lock(mutex_);
    if (manual_ || !permit_ || driver_.commandPending()) return OpResult::kRejected;

    const int joint = jointNumber - 1;
    // Assumption: a joint number that does not exist is "invalid".
    if (joint < 0 || joint >= kJointCount) return OpResult::kInvalid;
    if (checkJoint(cfg_, joint, angle) != kIkOk) return OpResult::kInvalid;
    if (tcpBlocked_ && joint != kWrist) return OpResult::kRejected;

    JointAngles target = driver_.current();
    target.q[joint] = angle;
    const bool started = driver_.startMove(target, [done](bool completed) {
        if (done) done(completed ? OpResult::kSuccess : OpResult::kRejected);
    });
    return started ? OpResult::kPending : OpResult::kRejected;
}

void ArmMachine::setRunningPermit(bool permit) {
    Lock lock(mutex_);
    if (manual_) return;  // no effect in manual mode
    permit_ = permit;
    if (permit) {
        setStatus(status_text::kIdle);
    } else {
        driver_.stop();  // a running command is answered "rejected"
        setStatus(status_text::kStop);
    }
}

// ---------------- Read-back ----------------

Pose ArmMachine::currentPose() { return forwardKinematics(cfg_, driver_.current()); }

JointAngles ArmMachine::currentJoints() { return driver_.current(); }

const char *ArmMachine::status() {
    Lock lock(mutex_);
    return status_;
}

bool ArmMachine::permit() {
    Lock lock(mutex_);
    return permit_;
}

bool ArmMachine::manual() {
    Lock lock(mutex_);
    return manual_;
}

bool ArmMachine::tcpBlocked() {
    Lock lock(mutex_);
    return tcpBlocked_;
}

// ---------------- Local inputs ----------------

void ArmMachine::toggleManual() {
    Lock lock(mutex_);
    driver_.stop();
    if (!manual_) {
        manual_ = true;
        setStatus(status_text::kManual);
    } else {
        manual_ = false;
        permit_ = false;
        setStatus(status_text::kStop);
    }
}

bool ArmMachine::manualStreamTo(const JointAngles &target) {
    Lock lock(mutex_);
    if (!manual_) return false;
    if (tcpBlocked_ && movesTcp(driver_.current(), target)) return false;
    return driver_.streamTo(target);
}

void ArmMachine::obstacleStop() {
    Lock lock(mutex_);
    driver_.stop();
    if (manual_) return;  // status stays "manual"
    permit_ = false;
    setStatus(status_text::kStop, true);
}

void ArmMachine::setTcpBlocked(bool blocked) {
    Lock lock(mutex_);
    tcpBlocked_ = blocked;
}

// ---------------- Commissioning console ----------------

OpResult ArmMachine::consoleJogTo(const JointAngles &target) {
    Lock lock(mutex_);
    if (checkJoints(cfg_, target) != kIkOk) return OpResult::kInvalid;
    if (manual_ || driver_.commandPending()) return OpResult::kRejected;
    if (tcpBlocked_ && movesTcp(driver_.current(), target)) return OpResult::kRejected;
    return driver_.streamTo(target) ? OpResult::kSuccess : OpResult::kRejected;
}

void ArmMachine::consoleStop() {
    Lock lock(mutex_);
    driver_.stop();
}
