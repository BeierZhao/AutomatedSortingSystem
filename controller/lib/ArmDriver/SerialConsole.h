// Serial commissioning console, compatible with the ArmTest 3D GUI
// (XYZ, HOME, STATUS, jog commands and the "#OK:b,s,e" acknowledgement).
#pragma once

#include <Arduino.h>
#include <functional>

#include "ArmMachine.h"

class SerialConsole {
public:
    SerialConsole(ArmDriver &driver, ArmMachine &machine)
        : driver_(driver), machine_(machine) {}

    // Handler for project-specific commands. It sees every line first, with
    // its original case, and returns true if it handled the line.
    using ExtensionFn = std::function<bool(char *line, Stream &io)>;
    void setExtension(ExtensionFn fn) { extension_ = std::move(fn); }

    void begin(Stream &io);
    // Call from loop(). Never blocks.
    void poll();

    void printStatus();
    void printWorkspace();
    void printHelp();

private:
    void process(char *cmd);
    void cmdXyz(char *args);
    void jogServo(int joint, float servoAngle);
    void report(OpResult r);

    ArmDriver &driver_;
    ArmMachine &machine_;
    Stream *io_ = nullptr;
    ExtensionFn extension_;
    char buf_[128];
    uint8_t len_ = 0;
    int jogStep_ = 5;  // deg
};
