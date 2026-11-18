// Arm parameters and limits. Plain C++, no Arduino dependency.
//
// Values come from the ArmTest project (see docs/development_guide.md,
// section 3) and must not change without re-measuring the arm.
#pragma once

namespace arm {

// Joint index = "joint number" of interface 4.3 minus 1.
enum Joint { kBase = 0, kShoulder = 1, kElbow = 2, kWrist = 3, kJointCount = 4 };

// One joint. All angles in degrees.
//
// "Physical" angle is what the kinematics and the HTTP interfaces use.
// "Servo" angle is the 0..180 command of the hobby servo:
//     servo = servoSign * physical + servoOffset
struct JointConfig {
    float physMin;
    float physMax;
    float servoSign;    // +1 or -1
    float servoOffset;  // servo angle at physical angle 0
    float servoMin;
    float servoMax;
    float maxSpeed;     // deg/s, speed limit used by the motion task
};

// Offsets from the wrist point to the TCP. The parallelogram keeps the tool
// level, so two numbers describe a tool head.
struct ToolConfig {
    float radialOffset;  // mm, along the horizontal arm direction
    float zOffset;       // mm, up
};

// Physical joint angles in degrees.
//   base     = yaw, 0 = straight ahead (+X), positive toward +Y
//   shoulder = b
//   elbow    = a
//   wrist    = rotation about the vertical axis, 0 = pointing radially outward
struct JointAngles {
    float q[kJointCount];
};

struct ArmConfig {
    float l1;  // mm, shoulder to elbow
    float l2;  // mm, elbow to wrist
    JointConfig joint[kJointCount];
    ToolConfig tool;
    JointAngles home;
};

// Measured values for the shared arm, with no tool offsets.
inline ArmConfig defaultArmConfig() {
    ArmConfig c;
    c.l1 = 140.0f;
    c.l2 = 155.0f;

    // Base: invertBase = true in ArmTest, so servo = 90 - yaw.
    c.joint[kBase] = {-70.0f, 70.0f, -1.0f, 90.0f, 20.0f, 160.0f, 60.0f};
    // Shoulder: b = servo + 5.
    c.joint[kShoulder] = {25.0f, 110.0f, 1.0f, -5.0f, 20.0f, 105.0f, 45.0f};
    // Elbow: a = servo - 40.
    c.joint[kElbow] = {5.0f, 50.0f, 1.0f, 40.0f, 45.0f, 90.0f, 45.0f};
    // Wrist: NOT MEASURED YET. Proposed default: servo centre = radially
    // outward, +/-90 degrees of travel. Re-measure sign, offset and limits.
    c.joint[kWrist] = {-70.0f, 70.0f, 1.0f, 90.0f, 20.0f, 170.0f, 120.0f};

    // Tool offsets: NOT MEASURED YET, see lib/ToolHead.
    c.tool.radialOffset = 0.0f;
    c.tool.zOffset = 0.0f;

    // Home pose: servo 90 / 62 / 72 in ArmTest.
    c.home.q[kBase] = 0.0f;
    c.home.q[kShoulder] = 67.0f;
    c.home.q[kElbow] = 32.0f;
    c.home.q[kWrist] = 0.0f;
    return c;
}

}  // namespace arm
