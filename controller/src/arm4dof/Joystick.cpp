#include "Joystick.h"

#include <math.h>

#include "Arm4dofConfig.h"

using namespace arm;
using namespace arm4dof;

namespace {
const uint32_t kPeriodMs = 50;  // 20 Hz
const int kAdcMax = 4095;       // 12-bit
}  // namespace

void Joystick::begin(int vrxPin, int vryPin, int swPin) {
    vrxPin_ = vrxPin;
    vryPin_ = vryPin;
    swPin_ = swPin;
    pinMode(swPin_, INPUT_PULLUP);

    long sumX = 0, sumY = 0;
    for (int i = 0; i < 16; i++) {
        sumX += analogRead(vrxPin_);
        sumY += analogRead(vryPin_);
        delay(2);
    }
    centreX_ = (int)(sumX / 16);
    centreY_ = (int)(sumY / 16);

    xTaskCreatePinnedToCore(taskEntry, "joystick", 4096, this, 3, nullptr, 1);
}

// Deflection from -1 to +1, with a deadband around the centre.
float Joystick::normalise(int raw, int centre) const {
    const float span = raw >= centre ? (float)(kAdcMax - centre) : (float)centre;
    float v = span > 0.0f ? (raw - centre) / span : 0.0f;
    v = constrain(v, -1.0f, 1.0f);
    if (fabsf(v) < kJoystickDeadband) return 0.0f;
    // Rescale so the output starts from 0 at the edge of the deadband.
    const float mag = (fabsf(v) - kJoystickDeadband) / (1.0f - kJoystickDeadband);
    return v > 0.0f ? mag : -mag;
}

void Joystick::manualStep(float vx, float vy, float dt) {
    if (vx == 0.0f && vy == 0.0f) return;

    const ArmConfig &cfg = machine_.config();
    const JointAngles now = machine_.currentJoints();

    // The wrist turns the sensor toward the direction the stick is pushed.
    const JointConfig &wc = cfg.joint[kWrist];
    const float heading = atan2f(vy, vx) * 180.0f / (float)M_PI;
    const float wrist = constrain(wrapDeg(heading - now.q[kBase]), wc.physMin, wc.physMax);
    const bool aligned = fabsf(wrist - now.q[kWrist]) <= kManualWristAlignTolDeg;

    if (!machine_.tcpBlocked() && aligned) {
        const float nx = x_ + vx * kManualMaxSpeedMmPerS * dt;
        const float ny = y_ + vy * kManualMaxSpeedMmPerS * dt;
        JointAngles target;
        if (inverseKinematics(cfg, nx, ny, z_, wrist, target) == kIkOk &&
            machine_.manualStreamTo(target)) {
            x_ = nx;
            y_ = ny;
            return;
        }
    }

    // Obstacle too close, wrist not yet aligned, or the edge of the
    // workspace: only the wrist turns. The TCP target restarts from where
    // the arm actually is.
    JointAngles target = now;
    target.q[kWrist] = wrist;
    machine_.manualStreamTo(target);
    const Pose p = forwardKinematics(cfg, now);
    x_ = p.x;
    y_ = p.y;
}

void Joystick::taskEntry(void *self) { static_cast<Joystick *>(self)->taskLoop(); }

void Joystick::taskLoop() {
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kPeriodMs));

        // SW button: two equal samples in a row (100 ms) debounce it.
        const bool sw = digitalRead(swPin_) == LOW;
        if (sw == swLast_ && sw != swStable_) {
            swStable_ = sw;
            if (sw) {
                machine_.toggleManual();
                if (machine_.manual()) {
                    const Pose p = machine_.currentPose();
                    x_ = p.x;
                    y_ = p.y;
                    z_ = p.z;
                }
            }
        }
        swLast_ = sw;

        Reading r;
        r.rawX = analogRead(vrxPin_);
        r.rawY = analogRead(vryPin_);
        r.centreX = centreX_;
        r.centreY = centreY_;
        r.x = kJoystickXSign * normalise(r.rawX, centreX_);
        r.y = kJoystickYSign * normalise(r.rawY, centreY_);
        r.pressed = swStable_;
        reading_ = r;

        if (machine_.manual()) manualStep(r.x, r.y, kPeriodMs / 1000.0f);
    }
}
