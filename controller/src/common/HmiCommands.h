// Extra serial commands for the serial HMI panel (tools/serial_hmi):
//
//   INFO                 one line "#INFO:{json}" with the live state
//   J <joint> <angle>    jog one joint to a physical angle (joint 1 to 4)
//   SET <key> <value>    store a network setting (SSID, PASS, GWHOST, GWPORT,
//                        STPATH, ALPATH, SRPATH)
//   FACTORY              forget the stored settings
//   REBOOT               restart, which applies stored settings
//
// Answers that the panel parses start with "#CFG:" or "#J:".
#pragma once

#include <ArduinoJson.h>
#include <functional>

#include "ArmDriver.h"
#include "ArmMachine.h"
#include "SerialConsole.h"

namespace hmi {

// Lets a project add its own fields to the INFO line.
using InfoExtraFn = std::function<void(JsonObject info)>;
// Lets a project add its own commands. Sees each line (original case) after
// the HMI commands, and returns true if it handled it.
using CommandFn = std::function<bool(char *line, Stream &io)>;

void begin(SerialConsole &console, ArmDriver &driver, ArmMachine &machine,
           const char *appName, InfoExtraFn extra = nullptr, CommandFn commands = nullptr);

}  // namespace hmi
