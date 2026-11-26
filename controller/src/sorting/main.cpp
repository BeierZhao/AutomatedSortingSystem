// Automated Sorting System build. STUB: only the shared arm controller is
// wired up so far (arm motion, serial console, interfaces 4.2, 4.3, 4.5,
// 4.6, 4.7 and the status report 2.1).
//
// Still to do (docs/development_guide.md, section 9, step 8): gripper,
// conveyor (4.4), vision client (3.x), position config (4.1), sequence
// control and the sorting result report (2.3).
#include <Arduino.h>

#include "AppNet.h"
#include "ArmDriver.h"
#include "ArmMachine.h"
#include "BoardPins.h"
#include "ControllerApi.h"
#include "HmiCommands.h"
#include "ScadaClient.h"
#include "SerialConsole.h"
#include "ToolHead.h"

static ArmDriver driver;
static ArmMachine machine(driver);
static SerialConsole console(driver, machine);
static ControllerApi api(machine, NET_CONTROLLER_HTTP_PORT);
static ScadaClient scada;

void setup() {
    Serial.begin(115200);
    settings::load();

    arm::ArmConfig cfg = arm::defaultArmConfig();
    cfg.tool = arm::gripperTool();

    ArmHardware hw;
    hw.pin[arm::kBase] = pins::kServoBase;
    hw.pin[arm::kShoulder] = pins::kServoShoulder;
    hw.pin[arm::kElbow] = pins::kServoElbow;
    hw.pin[arm::kWrist] = pins::kServoWrist;

    driver.begin(cfg, hw);
    machine.begin(cfg, /*wristFollowsTravel=*/false);
    console.begin(Serial);
    hmi::begin(console, driver, machine, "sorting");

    wifiBegin(appNetworkConfig());
    const DeviceSettings &net = settings::active();
    scada.begin(net.scadaHost.c_str(), net.scadaPort, net.scadaStatusPath.c_str(),
                net.scadaAlarmPath.c_str());
    machine.onStatus([](const char *status) { scada.reportStatus(status); });
    scada.reportStatus(machine.status());
    api.begin();

    delay(500);
    Serial.println(F("\n=== Arm controller: sorting build (stub) ==="));
    console.printStatus();
    console.printHelp();
}

void loop() {
    console.poll();
    delay(2);
}
