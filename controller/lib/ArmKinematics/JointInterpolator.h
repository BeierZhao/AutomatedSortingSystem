// Speed-limited joint-space interpolation. Plain C++, unit-tested on the PC.
// The motion task of ArmDriver calls step() at 50 Hz and writes the result
// to the servos.
#pragma once

#include "ArmConfig.h"

namespace arm {

class JointInterpolator {
public:
    JointInterpolator();

    // Sets the current position without moving (also clears the target).
    void reset(const JointAngles &current);

    // Coordinated move: all joints start and arrive together, and no joint
    // exceeds its maxSpeed.
    //
    // If wristHeadingDeg is not null, the wrist is not part of the
    // coordinated move. Instead it turns at its own maxSpeed so the tool
    // faces that world-frame heading (0 = +X, 90 = +Y) while the base turns:
    //     wrist = heading - yaw, clamped to the wrist limits.
    // Joints 1 to 3 wait until the wrist is within 10 degrees of that angle.
    // target.q[kWrist] is ignored in that case.
    void moveTo(const ArmConfig &cfg, const JointAngles &target,
                const float *wristHeadingDeg = 0);

    // Streaming target for jogging: every joint heads for the target at its
    // own maxSpeed. Can be called again at any time to retarget.
    void streamTo(const ArmConfig &cfg, const JointAngles &target);

    // Freezes at the current interpolated position.
    void halt();

    // Advances by dt seconds. Returns true when every joint is on target.
    bool step(float dt);

    bool arrived() const;
    const JointAngles &current() const { return cur_; }
    const JointAngles &target() const { return tgt_; }

private:
    void updateWristTarget();

    JointAngles cur_;
    JointAngles tgt_;
    float speed_[kJointCount];  // deg/s for this move
    bool trackHeading_;
    float heading_;
    float wristMin_;
    float wristMax_;
};

}  // namespace arm
