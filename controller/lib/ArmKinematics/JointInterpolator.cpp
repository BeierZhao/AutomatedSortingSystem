#include "JointInterpolator.h"

#include <math.h>

#include "ArmKinematics.h"

namespace arm {

namespace {
const float kArriveEps = 1e-3f;  // deg
const float kWristAlignTolDeg = 10.0f;
}

JointInterpolator::JointInterpolator()
    : trackHeading_(false), heading_(0.0f), wristMin_(0.0f), wristMax_(0.0f) {
    for (int i = 0; i < kJointCount; i++) {
        cur_.q[i] = 0.0f;
        tgt_.q[i] = 0.0f;
        speed_[i] = 0.0f;
    }
}

void JointInterpolator::reset(const JointAngles &current) {
    cur_ = current;
    tgt_ = current;
    trackHeading_ = false;
}

void JointInterpolator::moveTo(const ArmConfig &cfg, const JointAngles &target,
                               const float *wristHeadingDeg) {
    const int coordinated = wristHeadingDeg ? kWrist : kJointCount;

    // Duration is set by the joint that needs the longest at its max speed.
    float duration = 0.0f;
    for (int i = 0; i < coordinated; i++) {
        const float t = fabsf(target.q[i] - cur_.q[i]) / cfg.joint[i].maxSpeed;
        if (t > duration) duration = t;
    }
    for (int i = 0; i < coordinated; i++) {
        tgt_.q[i] = target.q[i];
        speed_[i] = duration > 0.0f
                        ? fabsf(target.q[i] - cur_.q[i]) / duration
                        : cfg.joint[i].maxSpeed;
    }

    trackHeading_ = wristHeadingDeg != 0;
    if (trackHeading_) {
        heading_ = *wristHeadingDeg;
        wristMin_ = cfg.joint[kWrist].physMin;
        wristMax_ = cfg.joint[kWrist].physMax;
        speed_[kWrist] = cfg.joint[kWrist].maxSpeed;
        updateWristTarget();
    }
}

void JointInterpolator::streamTo(const ArmConfig &cfg, const JointAngles &target) {
    tgt_ = target;
    trackHeading_ = false;
    for (int i = 0; i < kJointCount; i++) speed_[i] = cfg.joint[i].maxSpeed;
}

void JointInterpolator::halt() {
    tgt_ = cur_;
    trackHeading_ = false;
}

void JointInterpolator::updateWristTarget() {
    float w = wrapDeg(heading_ - cur_.q[kBase]);
    if (w < wristMin_) w = wristMin_;
    if (w > wristMax_) w = wristMax_;
    tgt_.q[kWrist] = w;
}

bool JointInterpolator::step(float dt) {
    // While the wrist is still turning toward the heading, joints 1 to 3
    // wait, so the tool faces the direction of travel before the TCP moves.
    bool holdArm = false;
    if (trackHeading_) {
        updateWristTarget();
        holdArm = fabsf(tgt_.q[kWrist] - cur_.q[kWrist]) > kWristAlignTolDeg;
    }
    for (int i = 0; i < kJointCount; i++) {
        if (holdArm && i != kWrist) continue;
        // The base has just moved, so aim the wrist again before it steps.
        if (i == kWrist && trackHeading_) updateWristTarget();
        const float remaining = tgt_.q[i] - cur_.q[i];
        const float maxStep = speed_[i] * dt;
        if (fabsf(remaining) <= maxStep + kArriveEps) {
            cur_.q[i] = tgt_.q[i];
        } else {
            cur_.q[i] += remaining > 0.0f ? maxStep : -maxStep;
        }
    }
    return arrived();
}

bool JointInterpolator::arrived() const {
    for (int i = 0; i < kJointCount; i++) {
        if (fabsf(tgt_.q[i] - cur_.q[i]) > kArriveEps) return false;
    }
    return true;
}

}  // namespace arm
