// Tunable constants of the 4-DOF arm project.
// Values marked NOT MEASURED are the proposals of docs/development_guide.md
// and must be tuned on the real arm.
#pragma once

namespace arm4dof {

// ---- Obstacle (NOT MEASURED) ----
const int kObstacleStopMm = 100;   // alarm raised below this distance
const int kObstacleClearMm = 150;  // alarm cleared above this distance
const int kObstacleMaxRangeMm = 4000;  // longer echoes count as "nothing in range"
const char *const kObstacleAlarmMessage = "Obstacle detected";

// ---- Buzzer ----
// Assumption: active buzzer module that sounds while its input is HIGH.
const bool kBuzzerActiveHigh = true;

// ---- Joystick ----
const float kManualMaxSpeedMmPerS = 40.0f;  // TCP speed at full deflection
const float kJoystickDeadband = 0.15f;      // fraction of full deflection
// Stick axis to world axis. Flip a sign if the arm moves the wrong way.
const float kJoystickXSign = 1.0f;  // VRX -> world X (forward)
const float kJoystickYSign = 1.0f;  // VRY -> world Y (side)
// In manual mode the TCP waits until the wrist (sensor) is this close to
// the direction the stick is pushed.
const float kManualWristAlignTolDeg = 15.0f;

}  // namespace arm4dof
