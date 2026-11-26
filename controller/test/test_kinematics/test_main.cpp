// PC unit tests for ArmKinematics and JointInterpolator.
//   pio test -e native
#include <math.h>
#include <unity.h>

#include "ArmKinematics.h"
#include "JointInterpolator.h"
#include "ToolHead.h"

using namespace arm;

static ArmConfig cfg;

void setUp(void) { cfg = defaultArmConfig(); }
void tearDown(void) {}

static JointAngles angles(float yaw, float b, float a, float w) {
    JointAngles q;
    q.q[kBase] = yaw;
    q.q[kShoulder] = b;
    q.q[kElbow] = a;
    q.q[kWrist] = w;
    return q;
}

// ---------------- Servo mapping ----------------

static void test_home_pose_matches_armtest_servo_angles(void) {
    // ArmTest home: base 90, shoulder 62, elbow 72.
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 90.0f, physToServo(cfg, kBase, cfg.home.q[kBase]));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 62.0f, physToServo(cfg, kShoulder, cfg.home.q[kShoulder]));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 72.0f, physToServo(cfg, kElbow, cfg.home.q[kElbow]));
}

static void test_base_is_inverted(void) {
    // invertBase = true: servo = 90 - yaw.
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 60.0f, physToServo(cfg, kBase, 30.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 30.0f, servoToPhys(cfg, kBase, 60.0f));
}

static void test_servo_phys_round_trip(void) {
    for (int j = 0; j < kJointCount; j++) {
        const float phys = 0.5f * (cfg.joint[j].physMin + cfg.joint[j].physMax);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, phys, servoToPhys(cfg, j, physToServo(cfg, j, phys)));
    }
}

static void test_physical_and_servo_limits_agree(void) {
    // Guide section 3: the physical limits map onto the servo limits.
    for (int j = 0; j < kJointCount; j++) {
        const float s0 = physToServo(cfg, j, cfg.joint[j].physMin);
        const float s1 = physToServo(cfg, j, cfg.joint[j].physMax);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, cfg.joint[j].servoMin, fminf(s0, s1));
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, cfg.joint[j].servoMax, fmaxf(s0, s1));
    }
}

// ---------------- Forward kinematics ----------------

static void test_fk_home(void) {
    // Reference values from the ArmTest formulas: b = 67, a = 32, yaw = 0.
    const Pose p = forwardKinematics(cfg, cfg.home);
    const float d2r = 3.14159265f / 180.0f;
    const float r = 140.0f * cosf(67.0f * d2r) + 155.0f * cosf(32.0f * d2r);
    const float z = 140.0f * sinf(67.0f * d2r) - 155.0f * sinf(32.0f * d2r);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, r, p.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 0.0f, p.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, z, p.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 186.15f, p.x);  // hand-checked
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 46.73f, p.z);
}

static void test_fk_yaw_rotates_about_z(void) {
    const Pose p0 = forwardKinematics(cfg, angles(0, 60, 30, 0));
    const Pose p90 = forwardKinematics(cfg, angles(90, 60, 30, 0));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 0.0f, p90.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, p0.x, p90.y);  // positive yaw goes to +Y
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, p0.z, p90.z);
}

static void test_fk_wrist_does_not_move_tcp(void) {
    const Pose p0 = forwardKinematics(cfg, angles(20, 60, 30, -45));
    const Pose p1 = forwardKinematics(cfg, angles(20, 60, 30, 70));
    TEST_ASSERT_EQUAL_FLOAT(p0.x, p1.x);
    TEST_ASSERT_EQUAL_FLOAT(p0.y, p1.y);
    TEST_ASSERT_EQUAL_FLOAT(p0.z, p1.z);
}

static void test_fk_applies_tool_offsets(void) {
    const Pose bare = forwardKinematics(cfg, angles(0, 60, 30, 0));
    cfg.tool.radialOffset = 30.0f;
    cfg.tool.zOffset = -12.0f;
    const Pose tool = forwardKinematics(cfg, angles(0, 60, 30, 0));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, bare.x + 30.0f, tool.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, bare.z - 12.0f, tool.z);
}

// ---------------- Inverse kinematics ----------------

static void check_round_trip(void) {
    // Sweep the whole joint space, including the limits themselves.
    int count = 0;
    for (float yaw = -90.0f; yaw <= 90.0f; yaw += 15.0f) {
        for (float b = 25.0f; b <= 110.0f; b += 5.0f) {
            for (float a = 5.0f; a <= 50.0f; a += 5.0f) {
                const JointAngles q = angles(yaw, b, a, 10.0f);
                const Pose p = forwardKinematics(cfg, q);
                if (p.x < 0.0f) continue;  // rounding at yaw = +/-90
                JointAngles out;
                const IkError e = inverseKinematics(cfg, p.x, p.y, p.z, 10.0f, out);
                TEST_ASSERT_EQUAL_INT(kIkOk, e);
                // Compare in Cartesian space: the pose must be reproduced.
                const Pose back = forwardKinematics(cfg, out);
                TEST_ASSERT_FLOAT_WITHIN(0.05f, p.x, back.x);
                TEST_ASSERT_FLOAT_WITHIN(0.05f, p.y, back.y);
                TEST_ASSERT_FLOAT_WITHIN(0.05f, p.z, back.z);
                // The solution is unique inside the limits (elbow-up only).
                TEST_ASSERT_FLOAT_WITHIN(0.05f, b, out.q[kShoulder]);
                TEST_ASSERT_FLOAT_WITHIN(0.05f, a, out.q[kElbow]);
                if (p.r > 1.0f) TEST_ASSERT_FLOAT_WITHIN(0.05f, yaw, out.q[kBase]);
                TEST_ASSERT_EQUAL_FLOAT(10.0f, out.q[kWrist]);
                count++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(1000, count);
}

static void test_ik_round_trip(void) { check_round_trip(); }

static void test_ik_round_trip_with_tool_offsets(void) {
    cfg.tool.radialOffset = 35.0f;
    cfg.tool.zOffset = -20.0f;
    check_round_trip();
}

static void test_ik_matches_armtest_home(void) {
    // ArmTest: XYZ of the home pose solves back to servo 90 / 62 / 72.
    const Pose p = forwardKinematics(cfg, cfg.home);
    JointAngles out;
    TEST_ASSERT_EQUAL_INT(kIkOk, inverseKinematics(cfg, p.x, p.y, p.z, 0.0f, out));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 90.0f, physToServo(cfg, kBase, out.q[kBase]));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 62.0f, physToServo(cfg, kShoulder, out.q[kShoulder]));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 72.0f, physToServo(cfg, kElbow, out.q[kElbow]));
}

static void test_ik_reports_diagnostics(void) {
    JointAngles out;
    IkDiag d;
    TEST_ASSERT_EQUAL_INT(kIkTooFar, inverseKinematics(cfg, 400, 0, 0, 0, out, &d));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 400.0f, d.dist);
}

static void test_ik_failure_leaves_output_untouched(void) {
    JointAngles out = angles(1, 2, 3, 4);
    TEST_ASSERT_NOT_EQUAL(kIkOk, inverseKinematics(cfg, 400, 0, 0, 0, out));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out.q[kBase]);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, out.q[kWrist]);
}

// One test per IK error kind.

static void test_ik_err_behind_base(void) {
    JointAngles out;
    TEST_ASSERT_EQUAL_INT(kIkBehindBase, inverseKinematics(cfg, -10, 50, 50, 0, out));
}

static void test_ik_err_base_limit(void) {
    // With the measured arm, X >= 0 always gives a yaw inside +/-90, so
    // narrow the base limits to exercise this error.
    cfg.joint[kBase].physMin = -45.0f;
    cfg.joint[kBase].physMax = 45.0f;
    JointAngles out;
    TEST_ASSERT_EQUAL_INT(kIkBaseLimit, inverseKinematics(cfg, 100, 150, 50, 0, out));
    TEST_ASSERT_EQUAL_INT(kIkOk, inverseKinematics(cfg, 150, 100, 50, 0, out));
}

static void test_ik_err_too_far(void) {
    JointAngles out;
    TEST_ASSERT_EQUAL_INT(kIkTooFar, inverseKinematics(cfg, 296, 0, 0, 0, out));
    TEST_ASSERT_EQUAL_INT(kIkTooFar, inverseKinematics(cfg, 200, 200, 100, 0, out));
}

static void test_ik_err_too_close(void) {
    JointAngles out;
    // |L1 - L2| = 15 mm.
    TEST_ASSERT_EQUAL_INT(kIkTooClose, inverseKinematics(cfg, 5, 0, 5, 0, out));
    TEST_ASSERT_EQUAL_INT(kIkTooClose, inverseKinematics(cfg, 0, 0, 0, 0, out));
}

static void test_ik_err_shoulder_limit(void) {
    // FK of b = 120 (limit 110), a = 30.
    const Pose p = forwardKinematics(cfg, angles(0, 120, 30, 0));
    JointAngles out;
    IkDiag d;
    TEST_ASSERT_EQUAL_INT(kIkShoulderLimit, inverseKinematics(cfg, p.x, p.y, p.z, 0, out, &d));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 120.0f, d.b);
    // And below the lower limit.
    const Pose q = forwardKinematics(cfg, angles(0, 15, 30, 0));
    TEST_ASSERT_EQUAL_INT(kIkShoulderLimit, inverseKinematics(cfg, q.x, q.y, q.z, 0, out));
}

static void test_ik_err_elbow_limit(void) {
    const Pose p = forwardKinematics(cfg, angles(0, 60, 60, 0));  // a limit 50
    JointAngles out;
    IkDiag d;
    TEST_ASSERT_EQUAL_INT(kIkElbowLimit, inverseKinematics(cfg, p.x, p.y, p.z, 0, out, &d));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 60.0f, d.a);
    const Pose q = forwardKinematics(cfg, angles(0, 60, 0, 0));  // a limit 5
    TEST_ASSERT_EQUAL_INT(kIkElbowLimit, inverseKinematics(cfg, q.x, q.y, q.z, 0, out));
}

static void test_ik_err_servo_limit(void) {
    // Servo limits tighter than the physical limits.
    cfg.joint[kShoulder].servoMax = 80.0f;  // b <= 85
    const Pose p = forwardKinematics(cfg, angles(0, 100, 30, 0));
    JointAngles out;
    TEST_ASSERT_EQUAL_INT(kIkServoLimit, inverseKinematics(cfg, p.x, p.y, p.z, 0, out));
}

static void test_ik_err_wrist_limit(void) {
    const Pose p = forwardKinematics(cfg, cfg.home);
    JointAngles out;
    TEST_ASSERT_EQUAL_INT(kIkWristLimit, inverseKinematics(cfg, p.x, p.y, p.z, 135.0f, out));
}

// ---------------- Joint limit checks (interface 4.3) ----------------

static void test_check_joint_limits(void) {
    TEST_ASSERT_EQUAL_INT(kIkOk, checkJoint(cfg, kBase, -90.0f));
    TEST_ASSERT_EQUAL_INT(kIkOk, checkJoint(cfg, kBase, 90.0f));
    TEST_ASSERT_EQUAL_INT(kIkBaseLimit, checkJoint(cfg, kBase, 90.5f));
    TEST_ASSERT_EQUAL_INT(kIkBaseLimit, checkJoint(cfg, kBase, -91.0f));

    TEST_ASSERT_EQUAL_INT(kIkOk, checkJoint(cfg, kShoulder, 25.0f));
    TEST_ASSERT_EQUAL_INT(kIkOk, checkJoint(cfg, kShoulder, 110.0f));
    TEST_ASSERT_EQUAL_INT(kIkShoulderLimit, checkJoint(cfg, kShoulder, 24.0f));
    TEST_ASSERT_EQUAL_INT(kIkShoulderLimit, checkJoint(cfg, kShoulder, 111.0f));

    TEST_ASSERT_EQUAL_INT(kIkOk, checkJoint(cfg, kElbow, 5.0f));
    TEST_ASSERT_EQUAL_INT(kIkOk, checkJoint(cfg, kElbow, 50.0f));
    TEST_ASSERT_EQUAL_INT(kIkElbowLimit, checkJoint(cfg, kElbow, 4.0f));
    TEST_ASSERT_EQUAL_INT(kIkElbowLimit, checkJoint(cfg, kElbow, 51.0f));

    TEST_ASSERT_EQUAL_INT(kIkOk, checkJoint(cfg, kWrist, 0.0f));
    TEST_ASSERT_EQUAL_INT(kIkWristLimit, checkJoint(cfg, kWrist, 200.0f));

    TEST_ASSERT_NOT_EQUAL(kIkOk, checkJoint(cfg, 4, 0.0f));
    TEST_ASSERT_NOT_EQUAL(kIkOk, checkJoint(cfg, -1, 0.0f));
    TEST_ASSERT_NOT_EQUAL(kIkOk, checkJoint(cfg, kBase, NAN));
}

static void test_check_joints_home_ok(void) {
    TEST_ASSERT_EQUAL_INT(kIkOk, checkJoints(cfg, cfg.home));
    TEST_ASSERT_EQUAL_INT(kIkElbowLimit, checkJoints(cfg, angles(0, 67, 80, 0)));
}

static void test_wrap_deg(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -170.0f, wrapDeg(190.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 170.0f, wrapDeg(-190.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 180.0f, wrapDeg(180.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 180.0f, wrapDeg(-180.0f));
}

static void test_tool_presets(void) {
    const ToolConfig t = ultrasonicMountTool();
    TEST_ASSERT_EQUAL_FLOAT(kUltrasonicToolRadialOffsetMm, t.radialOffset);
    TEST_ASSERT_EQUAL_FLOAT(kUltrasonicToolZOffsetMm, t.zOffset);
}

// ---------------- Interpolator ----------------

static const float kDt = 0.02f;  // 50 Hz

static void test_interp_coordinated_move_respects_speed_and_arrives_together(void) {
    JointInterpolator ip;
    ip.reset(cfg.home);
    const JointAngles target = angles(60, 90, 20, 45);
    ip.moveTo(cfg, target);

    // Longest joint: base 60 deg at 60 deg/s = 1 s (shoulder 23/45, wrist 45/120).
    int steps = 0;
    JointAngles prev = ip.current();
    bool done = false;
    while (!done && steps < 1000) {
        done = ip.step(kDt);
        steps++;
        for (int j = 0; j < kJointCount; j++) {
            const float v = fabsf(ip.current().q[j] - prev.q[j]) / kDt;
            TEST_ASSERT_TRUE(v <= cfg.joint[j].maxSpeed * 1.001f);
        }
        if (steps < 48) {
            // All joints are still under way until the end of the move.
            for (int j = 0; j < kJointCount; j++) {
                TEST_ASSERT_TRUE(fabsf(ip.current().q[j] - target.q[j]) > 1e-3f);
            }
        }
        prev = ip.current();
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_INT_WITHIN(1, 50, steps);
    for (int j = 0; j < kJointCount; j++) {
        TEST_ASSERT_EQUAL_FLOAT(target.q[j], ip.current().q[j]);
    }
}

static void test_interp_halt_freezes(void) {
    JointInterpolator ip;
    ip.reset(cfg.home);
    ip.moveTo(cfg, angles(60, 90, 20, 0));
    for (int i = 0; i < 10; i++) ip.step(kDt);
    const JointAngles frozen = ip.current();
    TEST_ASSERT_FALSE(ip.arrived());
    ip.halt();
    TEST_ASSERT_TRUE(ip.arrived());
    ip.step(kDt);
    for (int j = 0; j < kJointCount; j++) {
        TEST_ASSERT_EQUAL_FLOAT(frozen.q[j], ip.current().q[j]);
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 12.0f, frozen.q[kBase]);  // 10 * 0.02 s * 60 deg/s
}

static void test_interp_zero_length_move_is_done_at_once(void) {
    JointInterpolator ip;
    ip.reset(cfg.home);
    ip.moveTo(cfg, cfg.home);
    TEST_ASSERT_TRUE(ip.arrived());
    TEST_ASSERT_TRUE(ip.step(kDt));
}

static void test_interp_stream_can_retarget(void) {
    JointInterpolator ip;
    ip.reset(cfg.home);
    ip.streamTo(cfg, angles(30, 67, 32, 0));
    for (int i = 0; i < 5; i++) ip.step(kDt);  // 6 deg
    ip.streamTo(cfg, angles(0, 67, 32, 0));
    int steps = 0;
    while (!ip.step(kDt) && steps < 100) steps++;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ip.current().q[kBase]);
    TEST_ASSERT_INT_WITHIN(1, 4, steps);
}

static void test_interp_wrist_tracks_heading(void) {
    JointInterpolator ip;
    ip.reset(angles(0, 67, 32, 0));
    // Travel toward +Y (heading 90) while the base turns to yaw 30:
    // the wrist must end at heading - yaw = 60.
    const float heading = 90.0f;
    ip.moveTo(cfg, angles(30, 67, 32, -77.0f /* ignored */), &heading);
    // The arm waits while the wrist is more than 10 degrees off (90 deg at
    // 120 deg/s: still 30 deg off after 0.5 s).
    for (int i = 0; i < 25; i++) ip.step(kDt);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ip.current().q[kBase]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, ip.current().q[kWrist]);
    int steps = 0;
    while (!ip.step(kDt) && steps < 1000) steps++;
    TEST_ASSERT_EQUAL_FLOAT(30.0f, ip.current().q[kBase]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 60.0f, ip.current().q[kWrist]);
}

static void test_interp_wrist_heading_is_clamped(void) {
    JointInterpolator ip;
    ip.reset(angles(-60, 67, 32, 0));
    const float heading = 90.0f;  // heading - yaw = 150, limit +90
    ip.moveTo(cfg, angles(-60, 80, 32, 0), &heading);
    int steps = 0;
    while (!ip.step(kDt) && steps < 1000) steps++;
    TEST_ASSERT_EQUAL_FLOAT(90.0f, ip.current().q[kWrist]);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_home_pose_matches_armtest_servo_angles);
    RUN_TEST(test_base_is_inverted);
    RUN_TEST(test_servo_phys_round_trip);
    RUN_TEST(test_physical_and_servo_limits_agree);
    RUN_TEST(test_fk_home);
    RUN_TEST(test_fk_yaw_rotates_about_z);
    RUN_TEST(test_fk_wrist_does_not_move_tcp);
    RUN_TEST(test_fk_applies_tool_offsets);
    RUN_TEST(test_ik_round_trip);
    RUN_TEST(test_ik_round_trip_with_tool_offsets);
    RUN_TEST(test_ik_matches_armtest_home);
    RUN_TEST(test_ik_reports_diagnostics);
    RUN_TEST(test_ik_failure_leaves_output_untouched);
    RUN_TEST(test_ik_err_behind_base);
    RUN_TEST(test_ik_err_base_limit);
    RUN_TEST(test_ik_err_too_far);
    RUN_TEST(test_ik_err_too_close);
    RUN_TEST(test_ik_err_shoulder_limit);
    RUN_TEST(test_ik_err_elbow_limit);
    RUN_TEST(test_ik_err_servo_limit);
    RUN_TEST(test_ik_err_wrist_limit);
    RUN_TEST(test_check_joint_limits);
    RUN_TEST(test_check_joints_home_ok);
    RUN_TEST(test_wrap_deg);
    RUN_TEST(test_tool_presets);
    RUN_TEST(test_interp_coordinated_move_respects_speed_and_arrives_together);
    RUN_TEST(test_interp_halt_freezes);
    RUN_TEST(test_interp_zero_length_move_is_done_at_once);
    RUN_TEST(test_interp_stream_can_retarget);
    RUN_TEST(test_interp_wrist_tracks_heading);
    RUN_TEST(test_interp_wrist_heading_is_clamped);
    return UNITY_END();
}
