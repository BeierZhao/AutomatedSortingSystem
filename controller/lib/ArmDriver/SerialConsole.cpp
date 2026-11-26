#include "SerialConsole.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

using namespace arm;

// The ArmTest 3D GUI classifies lines by the "[Nano OK]" and "[Nano IK FAIL]"
// prefixes and parses the STATUS block with regular expressions, so those
// texts are kept exactly as the Nano firmware printed them.

namespace {
const char *const kServoNames[kJointCount] = {"Base", "Shoulder", "Elbow", "Wrist"};

int servoInt(const ArmConfig &cfg, int joint, float phys) {
    return (int)lroundf(physToServo(cfg, joint, phys));
}
}  // namespace

void SerialConsole::begin(Stream &io) { io_ = &io; }

void SerialConsole::poll() {
    if (!io_) return;
    while (io_->available() > 0) {
        const char c = (char)io_->read();
        if (c == '\n' || c == '\r') {
            if (len_ > 0) {
                buf_[len_] = '\0';
                process(buf_);
                len_ = 0;
            }
        } else if (len_ < sizeof(buf_) - 1) {
            buf_[len_++] = c;
        } else {
            len_ = 0;
            io_->println(F("Command too long."));
        }
    }
}

void SerialConsole::report(OpResult r) {
    if (r == OpResult::kInvalid) {
        io_->println(F("ERROR: outside joint limits."));
    } else if (r == OpResult::kRejected) {
        io_->print(F("ERROR: rejected ("));
        if (machine_.manual()) {
            io_->print(F("manual mode"));
        } else if (driver_.commandPending()) {
            io_->print(F("remote command running"));
        } else {
            io_->print(F("obstacle"));
        }
        io_->println(F(")."));
    }
}

void SerialConsole::cmdXyz(char *args) {
    char *tokenX = strtok(args, " ");
    char *tokenY = strtok(nullptr, " ");
    char *tokenZ = strtok(nullptr, " ");
    if (!tokenX || !tokenY || !tokenZ) {
        io_->println(F("Format error! Usage: XYZ <X> <Y> <Z>"));
        return;
    }
    const float tx = atof(tokenX);
    const float ty = atof(tokenY);
    const float tz = atof(tokenZ);
    const ArmConfig &cfg = machine_.config();

    JointAngles target;
    IkDiag diag;
    const IkError res =
        inverseKinematics(cfg, tx, ty, tz, driver_.target().q[kWrist], target, &diag);

    if (res != kIkOk) {
        io_->printf("[Nano IK FAIL] XYZ (%.1f,%.1f,%.1f) : ", tx, ty, tz);
        switch (res) {
            case kIkBaseLimit:
                io_->printf("Base yaw out of bounds: %.1f\n", diag.yaw);
                break;
            case kIkTooFar:
                io_->printf("D=%.1fmm > Max reach\n", diag.dist);
                break;
            case kIkTooClose:
                io_->printf("D=%.1fmm < Min reach\n", diag.dist);
                break;
            case kIkShoulderLimit:
                io_->printf("Shoulder angle b=%.1f out of limits\n", diag.b);
                break;
            case kIkElbowLimit:
                io_->printf("Elbow angle a=%.1f out of limits\n", diag.a);
                break;
            default:
                io_->println(ikErrorText(res));
                break;
        }
        return;
    }

    const OpResult r = machine_.consoleJogTo(target);
    if (r != OpResult::kSuccess) {
        report(r);
        return;
    }
    const int b = servoInt(cfg, kBase, target.q[kBase]);
    const int s = servoInt(cfg, kShoulder, target.q[kShoulder]);
    const int e = servoInt(cfg, kElbow, target.q[kElbow]);
    // Debug text + fast ACK for the GUI.
    io_->printf("[Nano OK] XYZ (%.1f,%.1f,%.1f) -> Base:%d S:%d E:%d | #OK:%d,%d,%d\n",
                tx, ty, tz, b, s, e, b, s, e);
}

// Jogs one joint to a servo angle, clamped to the servo limits like ArmTest.
void SerialConsole::jogServo(int joint, float servoAngle) {
    const ArmConfig &cfg = machine_.config();
    const JointConfig &jc = cfg.joint[joint];
    servoAngle = constrain(servoAngle, jc.servoMin, jc.servoMax);

    JointAngles target = driver_.target();
    target.q[joint] = constrain(servoToPhys(cfg, joint, servoAngle), jc.physMin, jc.physMax);
    const OpResult r = machine_.consoleJogTo(target);
    if (r != OpResult::kSuccess) {
        report(r);
        return;
    }
    io_->printf("[Nano] %s target: %d\n", kServoNames[joint],
                servoInt(cfg, joint, target.q[joint]));
}

void SerialConsole::process(char *cmd) {
    while (*cmd == ' ') cmd++;
    if (strlen(cmd) == 0) return;
    if (extension_ && extension_(cmd, *io_)) return;

    if (strncasecmp(cmd, "XYZ ", 4) == 0 || strncasecmp(cmd, "MOV ", 4) == 0) {
        cmdXyz(cmd + 4);
        return;
    }
    if (strncasecmp(cmd, "G ", 2) == 0) {
        cmdXyz(cmd + 2);
        return;
    }

    for (uint8_t i = 0; cmd[i] != '\0'; i++) cmd[i] = (char)toupper((unsigned char)cmd[i]);

    const ArmConfig &cfg = machine_.config();

    // Jog and set commands: <B|S|E|W>+, <B|S|E|W>-, <B|S|E|W> <servo angle>.
    static const char kLetters[] = "BSEW";
    const char *hit = cmd[0] ? strchr(kLetters, cmd[0]) : nullptr;
    if (hit && (cmd[1] == '+' || cmd[1] == '-' || cmd[1] == ' ') &&
        (cmd[1] == ' ' || cmd[2] == '\0')) {
        const int joint = (int)(hit - kLetters);
        if (cmd[1] == ' ') {
            jogServo(joint, atof(cmd + 2));
        } else {
            const float now = physToServo(cfg, joint, driver_.target().q[joint]);
            jogServo(joint, now + (cmd[1] == '+' ? jogStep_ : -jogStep_));
        }
        return;
    }

    if (strncmp(cmd, "STEP ", 5) == 0) {
        const int value = atoi(cmd + 5);
        if (value >= 1 && value <= 30) {
            jogStep_ = value;
            io_->printf("[Nano] Jog step set to: %d\n", jogStep_);
        } else {
            io_->println(F("Invalid step. Use 1 to 30 degrees."));
        }
    } else if (strcmp(cmd, "HOME") == 0) {
        const OpResult r = machine_.consoleJogTo(cfg.home);
        if (r == OpResult::kSuccess) {
            io_->println(F("[Nano] Moved to HOME position."));
            printStatus();
        } else {
            report(r);
        }
    } else if (strcmp(cmd, "STATUS") == 0) {
        printStatus();
    } else if (strcmp(cmd, "BOUNDS") == 0) {
        printWorkspace();
    } else if (strcmp(cmd, "STOP") == 0) {
        machine_.consoleStop();
        io_->println(F("[Nano] Stopped."));
    } else if (strncmp(cmd, "PERMIT ", 7) == 0) {
        machine_.setRunningPermit(atoi(cmd + 7) != 0);
        io_->printf("[Nano] Machine status: %s\n", machine_.status());
    } else if (strcmp(cmd, "HELP") == 0) {
        printHelp();
    } else {
        io_->print(F("[Nano] Unknown command: "));
        io_->println(cmd);
    }
}

void SerialConsole::printStatus() {
    const ArmConfig &cfg = machine_.config();
    // Like ArmTest, this shows the commanded target. The servos have no
    // feedback, and the GUI expects the target right after a command.
    const JointAngles q = driver_.target();
    const Pose p = forwardKinematics(cfg, q);
    const ArmHardware &hw = driver_.hardware();

    io_->println(F("\n====== Current Arm Status ======"));
    io_->println(F("[Servo Angles & Pin Mapping]"));
    static const char *const kPad[kJointCount] = {"    ", "", "   ", "   "};
    for (int j = 0; j < kJointCount; j++) {
        io_->printf("  %s Servo%s : %d deg (GPIO %d%s)\n", kServoNames[j], kPad[j],
                    servoInt(cfg, j, q.q[j]), hw.pin[j],
                    driver_.attached(j) ? " [OK]" : " [NOT ATTACHED]");
    }

    io_->println(F("[Physical Angles]"));
    io_->printf("  Yaw (Azimuth)  : %.1f deg\n", q.q[kBase]);
    io_->printf("  b (Shoulder)   : %.1f deg\n", q.q[kShoulder]);
    io_->printf("  a (Elbow drive): %.1f deg\n", q.q[kElbow]);
    io_->printf("  x (Link angle) : %.1f deg  (x = 180 - a - b)\n",
                180.0f - q.q[kElbow] - q.q[kShoulder]);
    io_->printf("  w (Wrist)      : %.1f deg\n", q.q[kWrist]);

    io_->println(F("[Cartesian Coordinates]"));
    io_->printf("  X : %.1f mm (Forward)\n", p.x);
    io_->printf("  Y : %.1f mm (Side)\n", p.y);
    io_->printf("  Z : %.1f mm (Up)\n", p.z);
    io_->printf("  R : %.1f mm (Radial)\n", p.r);

    io_->printf("Jog step: %d deg\n", jogStep_);
    io_->printf("Machine status: %s, permit %s, %s\n", machine_.status(),
                machine_.permit() ? "ON" : "OFF",
                driver_.isMoving() ? "moving" : "at target");
    io_->println(F("================================\n"));
}

void SerialConsole::printWorkspace() {
    const ArmConfig &cfg = machine_.config();
    const JointConfig &sh = cfg.joint[kShoulder];
    const JointConfig &el = cfg.joint[kElbow];
    const float d2r = (float)M_PI / 180.0f;
    const float rMax = cfg.l1 * cosf(sh.physMin * d2r) + cfg.l2 * cosf(el.physMin * d2r) +
                       cfg.tool.radialOffset;
    const float rMin = cfg.l1 * cosf(sh.physMax * d2r) + cfg.l2 * cosf(el.physMax * d2r) +
                       cfg.tool.radialOffset;
    const float zMax = cfg.l1 * sinf(sh.physMax * d2r) - cfg.l2 * sinf(el.physMin * d2r) +
                       cfg.tool.zOffset;
    const float zMin = cfg.l1 * sinf(sh.physMin * d2r) - cfg.l2 * sinf(el.physMax * d2r) +
                       cfg.tool.zOffset;

    io_->println(F("\n======== Arm Workspace Envelope (mm) ========"));
    io_->printf("  Link 1 (L1) : %.1f mm\n", cfg.l1);
    io_->printf("  Link 2 (L2) : %.1f mm\n", cfg.l2);
    io_->printf("  Tool offset : radial %.1f, up %.1f\n", cfg.tool.radialOffset,
                cfg.tool.zOffset);
    io_->println(F("  Physical limits (servo limits):"));
    for (int j = 0; j < kJointCount; j++) {
        const JointConfig &jc = cfg.joint[j];
        io_->printf("    Joint %d %-8s: %.1f ~ %.1f deg (servo %.0f ~ %.0f)\n", j + 1,
                    kServoNames[j], jc.physMin, jc.physMax, jc.servoMin, jc.servoMax);
    }
    io_->println(F("  Theoretical bounds:"));
    io_->printf("    Radius from %.1f to %.1f\n", rMin, rMax);
    io_->printf("    Height from %.1f to %.1f\n", zMin, zMax);
    io_->println(F("  * Note: (X, Y, Z) must satisfy arm kinematics simultaneously."));
    io_->println(F("=============================================\n"));
}

void SerialConsole::printHelp() {
    io_->println(F("===== Arm Controller Serial Commands ====="));
    io_->println(F("XYZ <X> <Y> <Z> : Move the TCP to world coordinates (mm)"));
    io_->println(F("G <X> <Y> <Z>   : Alias for XYZ"));
    io_->println(F("B+ B- S+ S- E+ E- W+ W- : Jog base / shoulder / elbow / wrist"));
    io_->println(F("B|S|E|W <angle> : Set one servo angle (clamped to its limits)"));
    io_->println(F("STEP <deg>      : Set jog step (1 ~ 30 deg)"));
    io_->println(F("HOME            : Move to the home pose"));
    io_->println(F("STOP            : Stop the current motion"));
    io_->println(F("PERMIT <0|1>    : Same as set-running-permit"));
    io_->println(F("STATUS          : Show target angles and coordinates"));
    io_->println(F("BOUNDS          : Show reachable workspace limits"));
    io_->println(F("HELP            : Show this help"));
    io_->println(F("HMI panel       : INFO, J <joint> <angle>, SET <SSID|PASS|GWHOST|GWPORT> <value>,"));
    io_->println(F("                  FACTORY, REBOOT"));
    io_->println(F("Serial jogs do not need the running permit."));
    io_->println(F("=========================================="));
}
