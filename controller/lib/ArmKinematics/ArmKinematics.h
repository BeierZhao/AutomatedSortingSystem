// Forward and inverse kinematics of the parallelogram arm.
// Plain C++, no Arduino dependency, unit-tested on the PC (test/).
//
// World frame: X forward, Y to the side, Z up, in mm.
//
//   R = L1*cos(b) + L2*cos(a) + tool.radialOffset
//   Z = L1*sin(b) - L2*sin(a) + tool.zOffset
//   X = R*cos(yaw)
//   Y = R*sin(yaw)
#pragma once

#include "ArmConfig.h"

namespace arm {

enum IkError {
    kIkOk = 0,
    kIkBehindBase,     // target X < 0
    kIkBaseLimit,      // yaw outside the base limits
    kIkTooFar,         // beyond L1 + L2
    kIkTooClose,       // inside |L1 - L2|
    kIkShoulderLimit,  // b outside its physical limits
    kIkElbowLimit,     // a outside its physical limits
    kIkServoLimit,     // a servo command outside its servo limits
    kIkWristLimit      // wrist angle outside its physical limits
};

struct Pose {
    float x, y, z;  // TCP, mm
    float r;        // horizontal distance of the TCP from the base axis, mm
};

// Values computed by the last inverseKinematics() call, for error messages.
struct IkDiag {
    float yaw;   // deg
    float b;     // deg
    float a;     // deg
    float dist;  // mm, shoulder to wrist point
};

const char *ikErrorText(IkError e);

float physToServo(const ArmConfig &cfg, int joint, float phys);
float servoToPhys(const ArmConfig &cfg, int joint, float servo);

// Checks one physical joint angle against the physical and servo limits.
// Returns kIkOk or the limit error of that joint.
IkError checkJoint(const ArmConfig &cfg, int joint, float phys);

// Checks all four joints, returns the first error.
IkError checkJoints(const ArmConfig &cfg, const JointAngles &q);

Pose forwardKinematics(const ArmConfig &cfg, const JointAngles &q);

// Solves joints 1 to 3 for the TCP target. The wrist does not move the TCP,
// so `wrist` is checked against its limits and copied to `out` unchanged.
// `out` is written only when the result is kIkOk.
IkError inverseKinematics(const ArmConfig &cfg, float x, float y, float z,
                          float wrist, JointAngles &out, IkDiag *diag = 0);

// Wraps an angle to (-180, 180].
float wrapDeg(float deg);

}  // namespace arm
