// Machine state on top of ArmDriver: running permit, status, manual mode,
// and the result rules of interfaces 4.2 / 4.3 / 4.5
// (docs/development_guide.md, section 4.4).
//
// All methods are safe to call from any task. None of them blocks on motion.
#pragma once

#include <Arduino.h>
#include <functional>

#include "ArmDriver.h"
#include "ArmKinematics.h"

// `operation result` of interfaces 4.2 and 4.3, plus kPending for "accepted,
// the result follows through the callback".
enum class OpResult { kSuccess, kInvalid, kRejected, kPending };

// Spec string for kSuccess / kInvalid / kRejected.
const char *opResultText(OpResult r);

// `status` values of interface 2.1 that the arm itself produces.
namespace status_text {
constexpr const char *kStop = "stop";
constexpr const char *kIdle = "idle";
constexpr const char *kManual = "manual";
}  // namespace status_text

class ArmMachine {
public:
    // Called exactly once after a kPending return, with kSuccess (arrived)
    // or kRejected (interrupted by an obstacle, permit = false or manual
    // mode). Runs in the motion task or in the task that interrupted.
    using DoneFn = std::function<void(OpResult)>;
    using StatusFn = std::function<void(const char *status)>;

    explicit ArmMachine(ArmDriver &driver) : driver_(driver) {}

    // wristFollowsTravel: on a set-arm-position move, turn the wrist toward
    // the horizontal direction of travel (4-DOF project only).
    void begin(const arm::ArmConfig &cfg, bool wristFollowsTravel);

    // Called on every status change (and once for an obstacle stop).
    void onStatus(StatusFn fn) { statusFn_ = std::move(fn); }

    // ---- Remote commands (HTTP) ----
    OpResult setArmPosition(float x, float y, float z, DoneFn done);
    OpResult setJointPosition(int jointNumber, float angle, DoneFn done);
    void setRunningPermit(bool permit);

    // ---- Read-back, never rejected ----
    arm::Pose currentPose();
    arm::JointAngles currentJoints();
    const char *status();
    bool permit();
    bool manual();
    bool tcpBlocked();
    const arm::ArmConfig &config() const { return cfg_; }

    // ---- Local inputs ----
    // Joystick SW: switches between manual mode and the automatic side.
    void toggleManual();
    // Joystick motion. Accepted only in manual mode.
    bool manualStreamTo(const arm::JointAngles &target);
    // Obstacle too close: freeze. Outside manual mode this also clears the
    // running permit and reports "stop".
    void obstacleStop();
    // While true, anything that moves the TCP (joints 1 to 3) is refused.
    // The wrist may still turn, so the sensor can be aimed away.
    void setTcpBlocked(bool blocked);

    // ---- Commissioning console (serial) ----
    // Local jog that does not need the running permit. Refused (kRejected)
    // in manual mode, while a remote command is running, or if it would
    // move the TCP while it is blocked. kInvalid if outside the limits.
    OpResult consoleJogTo(const arm::JointAngles &target);
    void consoleStop();

private:
    struct Lock;
    void setStatus(const char *s, bool forceReport = false);
    bool movesTcp(const arm::JointAngles &from, const arm::JointAngles &to) const;

    ArmDriver &driver_;
    arm::ArmConfig cfg_;
    SemaphoreHandle_t mutex_ = nullptr;
    StatusFn statusFn_;
    const char *status_ = status_text::kStop;
    bool permit_ = false;
    bool manual_ = false;
    bool tcpBlocked_ = false;
    bool wristFollowsTravel_ = false;
};
