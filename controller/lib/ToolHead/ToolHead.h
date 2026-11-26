// Tool head presets: TCP offsets from the wrist point, per project.
// Plain C++. The gripper servo of the sorting build will be added here.
#pragma once

#include "ArmConfig.h"

namespace arm {

// 4-DOF arm: ultrasonic sensor mount.
// NOT MEASURED YET: offsets from the wrist point to the TCP, in mm.
const float kUltrasonicToolRadialOffsetMm = 0.0f;
const float kUltrasonicToolZOffsetMm = 0.0f;

// Sorting system: gripper.
// NOT MEASURED YET.
const float kGripperToolRadialOffsetMm = 0.0f;
const float kGripperToolZOffsetMm = 0.0f;

inline ToolConfig ultrasonicMountTool() {
    ToolConfig t = {kUltrasonicToolRadialOffsetMm, kUltrasonicToolZOffsetMm};
    return t;
}

inline ToolConfig gripperTool() {
    ToolConfig t = {kGripperToolRadialOffsetMm, kGripperToolZOffsetMm};
    return t;
}

}  // namespace arm
