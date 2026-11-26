// Automated Sorting System build. Partly done: the shared arm controller
// (arm motion, serial console, interfaces 4.2, 4.3, 4.5, 4.6, 4.7 and the
// status report 2.1), the conveyor (4.4), the position config (4.1) and the
// sorting result report (2.3).
//
// Still to do (docs/development_guide.md, section 9, step 8): gripper,
// vision client (3.x) and the sequence control, which will call
// scada.reportSortingResult() for each finished part.
#include <Arduino.h>

#include "AppNet.h"
#include "ArmDriver.h"
#include "ArmMachine.h"
#include "BoardPins.h"
#include "ControllerApi.h"
#include "Conveyor.h"
#include "HmiCommands.h"
#include "PositionConfig.h"
#include "ScadaClient.h"
#include "SerialConsole.h"
#include "SortingConfig.h"
#include "ToolHead.h"

static ArmDriver driver;
static ArmMachine machine(driver);
static SerialConsole console(driver, machine);
static ControllerApi api(machine, NET_CONTROLLER_HTTP_PORT);
static ScadaClient scada;
static Conveyor conveyor;
static PositionConfig positions;

// 4.4 move conveyor: {"direction": bool, "distance": int mm}
static void handleMoveConveyor(AsyncWebServerRequest *request, JsonObjectConst body) {
    if (!body["direction"].is<bool>() || !body["distance"].is<int32_t>()) {
        ControllerApi::sendError(request, 400, "direction (boolean), distance (integer) required");
        return;
    }
    const bool direction = body["direction"];
    const int32_t distance = body["distance"];
    // Assumption: a negative or huge distance is a malformed request, since
    // the specification has no "invalid" result for this interface.
    if (distance < 0 || distance > sorting::kConveyorMaxDistanceMm) {
        ControllerApi::sendError(request, 400, "distance must be 0 to 5000 mm");
        return;
    }
    api.deferOperationResult(request, "move-conveyor", [direction, distance](ArmMachine::DoneFn done) {
        return machine.startAux([&]() {
            return conveyor.start(direction, (uint32_t)distance, [done](bool completed) {
                done(completed ? OpResult::kSuccess : OpResult::kRejected);
            });
        });
    });
}

// 4.1 update position config: answers {"operation_done": bool}.
static void handleUpdatePositionConfig(AsyncWebServerRequest *request, JsonObjectConst body) {
    String error;
    const bool done = positions.update(body, error);
    if (done) {
        Serial.printf("[%lu] Position config saved\n", millis());
    } else {
        Serial.printf("[%lu] Position config refused: %s\n", millis(), error.c_str());
    }
    JsonDocument doc;
    doc["operation_done"] = done;
    ControllerApi::sendJson(request, 200, doc);
}

// SRTEST: queues one sorting result report with fixed example values, to
// check the receiver and the stored SRPATH before the sequence exists.
static bool handleSortingCommand(char *line, Stream &io) {
    if (strcasecmp(line, "SRTEST") != 0) return false;
    SortingResult r;
    r.readingResult = "success";
    r.origin = "Hamilton";
    r.destination = "Toronto";
    r.partId = "10234";
    r.repository = "Repo3";
    r.sortingResult = true;
    scada.reportSortingResult(r);
    io.printf("#SR:OK test sorting result queued for %s\n",
              settings::active().scadaResultPath.c_str());
    return true;
}

void setup() {
    Serial.begin(115200);
    settings::load();
    positions.begin();

    arm::ArmConfig cfg = arm::defaultArmConfig();
    cfg.tool = arm::gripperTool();

    ArmHardware hw;
    hw.pin[arm::kBase] = pins::kServoBase;
    hw.pin[arm::kShoulder] = pins::kServoShoulder;
    hw.pin[arm::kElbow] = pins::kServoElbow;
    hw.pin[arm::kWrist] = pins::kServoWrist;

    driver.begin(cfg, hw);
    conveyor.begin(pins::kConveyorStep, pins::kConveyorDir, pins::kConveyorEnable,
                   sorting::kConveyorStepsPerMm, sorting::kConveyorSpeedMmPerS,
                   sorting::kConveyorDirLevelForTrue);
    machine.begin(cfg, /*wristFollowsTravel=*/false);
    machine.attachAux(&conveyor);
    console.begin(Serial);
    hmi::begin(
        console, driver, machine, "sorting",
        [](JsonObject info) {
            info["conveyor_moving"] = conveyor.busy();
            positions.summary(info["position_config"].to<JsonObject>());
        },
        handleSortingCommand);

    wifiBegin(appNetworkConfig());
    const DeviceSettings &net = settings::active();
    scada.begin(net.scadaHost.c_str(), net.scadaPort, net.scadaStatusPath.c_str(),
                net.scadaAlarmPath.c_str(), net.scadaResultPath.c_str());
    machine.onStatus([](const char *status) { scada.reportStatus(status); });
    scada.reportStatus(machine.status());

    api.addEndpoint("/controller/move-conveyor", handleMoveConveyor);
    api.addEndpoint("/controller/update-position-config", handleUpdatePositionConfig);
    api.begin();

    delay(500);
    Serial.println(F("\n=== Arm controller: sorting build ==="));
    console.printStatus();
    console.printHelp();
    Serial.println(F("SRTEST          : Send a test sorting result report (2.3)"));
}

void loop() {
    console.poll();
    delay(2);
}
