#include "ArmKinematics.h"

#include <math.h>

namespace arm {

namespace {

const float kPi = 3.14159265358979f;
const float kDegToRad = kPi / 180.0f;
const float kRadToDeg = 180.0f / kPi;

// Tolerance on limit checks, so a pose exactly on a limit survives a
// float FK -> IK round trip.
const float kLimitEps = 0.01f;  // deg
const float kReachEps = 0.01f;  // mm

float clampUnit(float v) {
    if (v < -1.0f) return -1.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

bool outside(float v, float lo, float hi) {
    return v < lo - kLimitEps || v > hi + kLimitEps;
}

IkError limitErrorOf(int joint) {
    switch (joint) {
        case kBase: return kIkBaseLimit;
        case kShoulder: return kIkShoulderLimit;
        case kElbow: return kIkElbowLimit;
        default: return kIkWristLimit;
    }
}

}  // namespace

const char *ikErrorText(IkError e) {
    switch (e) {
        case kIkOk: return "ok";
        case kIkBehindBase: return "target X < 0 (behind base)";
        case kIkBaseLimit: return "base yaw out of limits";
        case kIkTooFar: return "beyond max reach";
        case kIkTooClose: return "inside min reach";
        case kIkShoulderLimit: return "shoulder angle b out of limits";
        case kIkElbowLimit: return "elbow angle a out of limits";
        case kIkServoLimit: return "servo limit violated";
        case kIkWristLimit: return "wrist angle out of limits";
    }
    return "unknown";
}

float wrapDeg(float deg) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg <= -180.0f) deg += 360.0f;
    return deg;
}

float physToServo(const ArmConfig &cfg, int joint, float phys) {
    const JointConfig &j = cfg.joint[joint];
    return j.servoSign * phys + j.servoOffset;
}

float servoToPhys(const ArmConfig &cfg, int joint, float servo) {
    const JointConfig &j = cfg.joint[joint];
    return (servo - j.servoOffset) / j.servoSign;
}

IkError checkJoint(const ArmConfig &cfg, int joint, float phys) {
    if (joint < 0 || joint >= kJointCount) return kIkServoLimit;
    const JointConfig &j = cfg.joint[joint];
    if (!(phys == phys)) return limitErrorOf(joint);  // NaN
    if (outside(phys, j.physMin, j.physMax)) return limitErrorOf(joint);
    if (outside(physToServo(cfg, joint, phys), j.servoMin, j.servoMax)) {
        return kIkServoLimit;
    }
    return kIkOk;
}

IkError checkJoints(const ArmConfig &cfg, const JointAngles &q) {
    for (int i = 0; i < kJointCount; i++) {
        IkError e = checkJoint(cfg, i, q.q[i]);
        if (e != kIkOk) return e;
    }
    return kIkOk;
}

Pose forwardKinematics(const ArmConfig &cfg, const JointAngles &q) {
    const float yaw = q.q[kBase] * kDegToRad;
    const float b = q.q[kShoulder] * kDegToRad;
    const float a = q.q[kElbow] * kDegToRad;

    Pose p;
    p.r = cfg.l1 * cosf(b) + cfg.l2 * cosf(a) + cfg.tool.radialOffset;
    p.z = cfg.l1 * sinf(b) - cfg.l2 * sinf(a) + cfg.tool.zOffset;
    p.x = p.r * cosf(yaw);
    p.y = p.r * sinf(yaw);
    return p;
}

IkError inverseKinematics(const ArmConfig &cfg, float x, float y, float z,
                          float wrist, JointAngles &out, IkDiag *diag) {
    IkDiag d = {0.0f, 0.0f, 0.0f, 0.0f};
    IkError result = kIkOk;
    JointAngles q;
    q.q[kBase] = q.q[kShoulder] = q.q[kElbow] = 0.0f;
    q.q[kWrist] = wrist;

    do {
        if (!(x == x) || !(y == y) || !(z == z)) {  // NaN
            result = kIkTooFar;
            break;
        }
        if (x < -0.01f) {
            result = kIkBehindBase;
            break;
        }

        const float rTcp = sqrtf(x * x + y * y);
        const float yaw = atan2f(y, x) * kRadToDeg;
        d.yaw = yaw;
        if (outside(yaw, cfg.joint[kBase].physMin, cfg.joint[kBase].physMax)) {
            result = kIkBaseLimit;
            break;
        }

        // Wrist point: remove the tool offsets from the TCP target.
        const float r = rTcp - cfg.tool.radialOffset;
        const float zw = z - cfg.tool.zOffset;

        const float l1 = cfg.l1;
        const float l2 = cfg.l2;
        const float dist2 = r * r + zw * zw;
        const float dist = sqrtf(dist2);
        d.dist = dist;

        if (dist > l1 + l2 + kReachEps) {
            result = kIkTooFar;
            break;
        }
        if (dist < fabsf(l1 - l2) - kReachEps || dist < 1e-3f) {
            result = kIkTooClose;
            break;
        }

        // Angle between the two links (law of cosines).
        const float cosX = clampUnit((l1 * l1 + l2 * l2 - dist2) / (2.0f * l1 * l2));
        const float xDeg = acosf(cosX) * kRadToDeg;

        // Shoulder angle b = elevation of the wrist point + inner angle at
        // the shoulder.
        const float gamma = atan2f(zw, r);
        const float cosPsi = clampUnit((l1 * l1 + dist2 - l2 * l2) / (2.0f * l1 * dist));
        const float b = (gamma + acosf(cosPsi)) * kRadToDeg;
        const float a = 180.0f - xDeg - b;
        d.b = b;
        d.a = a;

        if (outside(b, cfg.joint[kShoulder].physMin, cfg.joint[kShoulder].physMax)) {
            result = kIkShoulderLimit;
            break;
        }
        if (outside(a, cfg.joint[kElbow].physMin, cfg.joint[kElbow].physMax)) {
            result = kIkElbowLimit;
            break;
        }

        q.q[kBase] = yaw;
        q.q[kShoulder] = b;
        q.q[kElbow] = a;

        for (int i = kBase; i <= kElbow; i++) {
            const JointConfig &j = cfg.joint[i];
            if (outside(physToServo(cfg, i, q.q[i]), j.servoMin, j.servoMax)) {
                result = kIkServoLimit;
            }
        }
        if (result != kIkOk) break;

        result = checkJoint(cfg, kWrist, wrist);
    } while (false);

    if (diag) *diag = d;
    if (result == kIkOk) out = q;
    return result;
}

}  // namespace arm
