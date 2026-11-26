// Settings of the sorting build that are not network settings.
// NOT MEASURED YET: set these from the real conveyor.
#pragma once

#include <stdint.h>

namespace sorting {

// Conveyor stepper. Steps per mm of belt travel, microstepping included:
// (motor steps per turn * microsteps) / (pi * drive roller diameter).
// 200 * 16 / (pi * 20 mm) = 50.9.
const float kConveyorStepsPerMm = 50.9f;
// Belt speed. Constant, no ramp: keep it below the speed at which the motor
// starts without acceleration.
const float kConveyorSpeedMmPerS = 40.0f;
// Level of the DIR pin for `direction` = true (interface 4.4). Flip it if
// the belt runs the wrong way.
const bool kConveyorDirLevelForTrue = true;
// Largest distance accepted by move-conveyor, in mm.
const int32_t kConveyorMaxDistanceMm = 5000;

}  // namespace sorting
