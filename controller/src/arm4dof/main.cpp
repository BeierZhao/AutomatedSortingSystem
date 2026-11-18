// 4-DOF arm project: shared arm controller + ultrasonic obstacle stop,
// buzzer and joystick manual mode. See docs/development_guide.md.
#include <Arduino.h>

#include "AppNet.h"
#include "Arm4dofConfig.h"
#include "ArmDriver.h"
#include "ArmMachine.h"
#include "BoardPins.h"
#include "ControllerApi.h"
#include "HmiCommands.h"
#include "Joystick.h"
#include "ObstacleMonitor.h"
#include "ScadaClient.h"
#include "SerialConsole.h"
#include "ToolHead.h"

using namespace arm4dof;

static ArmDriver driver;
static ArmMachine machine(driver);
static SerialConsole console(driver, machine);
static ControllerApi api(machine, NET_CONTROLLER_HTTP_PORT);
static ScadaClient scada;
static ObstacleMonitor obstacle;
static Joystick joystick(machine);

static void setBuzzer(bool on) {
    digitalWrite(pins::kBuzzer, on == kBuzzerActiveHigh ? HIGH : LOW);
}

// Runs in the obstacle task when the alarm is raised or cleared.
static void onObstacleAlarm(bool active) {
    setBuzzer(active);
    machine.setTcpBlocked(active);
    scada.reportAlarm(kObstacleAlarmMessage, active);
    // Freezes the arm and answers a pending request with "rejected". Outside
    // manual mode it also clears the permit and reports "stop".
    if (active) machine.obstacleStop();
}

void setup() {
    Serial.begin(115200);
    settings::load();

    pinMode(pins::kBuzzer, OUTPUT);
    setBuzzer(false);

    arm::ArmConfig cfg = arm::defaultArmConfig();
    cfg.tool = arm::ultrasonicMountTool();

    ArmHardware hw;
    hw.pin[arm::kBase] = pins::kServoBase;
    hw.pin[arm::kShoulder] = pins::kServoShoulder;
    hw.pin[arm::kElbow] = pins::kServoElbow;
    hw.pin[arm::kWrist] = pins::kServoWrist;

    driver.begin(cfg, hw);
    machine.begin(cfg, /*wristFollowsTravel=*/true);
    console.begin(Serial);
    hmi::begin(console, driver, machine, "arm4dof", [](JsonObject info) {
        info["distance"] = obstacle.distanceMm();
        info["alarm"] = obstacle.alarmActive();
        const Joystick::Reading r = joystick.reading();
        JsonObject js = info["joystick"].to<JsonObject>();
        js["raw_x"] = r.rawX;
        js["raw_y"] = r.rawY;
        js["centre_x"] = r.centreX;
        js["centre_y"] = r.centreY;
        js["x"] = round(r.x * 100.0) / 100.0;
        js["y"] = round(r.y * 100.0) / 100.0;
        js["sw"] = r.pressed;
    });

    wifiBegin(appNetworkConfig());
    scada.begin(settings::active().scadaHost.c_str(), settings::active().scadaPort);
    machine.onStatus([](const char *status) { scada.reportStatus(status); });
    scada.reportStatus(machine.status());  // "stop" after boot

    // 4.8 get obstacle distance
    api.addReadEndpoint("/controller/get-obstacle-distance",
                        [](JsonObject out) { out["distance"] = obstacle.distanceMm(); });
    api.begin();

    obstacle.begin(pins::kUltrasonicTrig, pins::kUltrasonicEcho, onObstacleAlarm);
    joystick.begin(pins::kJoystickVrx, pins::kJoystickVry, pins::kJoystickSw);

    delay(500);
    Serial.println(F("\n=== Arm controller: 4-DOF arm build ==="));
    console.printWorkspace();
    console.printStatus();
    console.printHelp();
}

void loop() {
    console.poll();
    delay(2);
}
