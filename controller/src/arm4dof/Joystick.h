// Joystick: the SW button switches manual mode, and in manual mode the
// stick moves the TCP in world X-Y with Z held (20 Hz task).
#pragma once

#include <Arduino.h>

#include "ArmMachine.h"

class Joystick {
public:
    explicit Joystick(ArmMachine &machine) : machine_(machine) {}

    // Reads the stick centre, so the stick must be released at power-up.
    void begin(int vrxPin, int vryPin, int swPin);

private:
    static void taskEntry(void *self);
    void taskLoop();
    float readAxis(int pin, int centre) const;
    void manualStep(float dt);

    ArmMachine &machine_;
    int vrxPin_ = -1;
    int vryPin_ = -1;
    int swPin_ = -1;
    int centreX_ = 2048;
    int centreY_ = 2048;
    bool swStable_ = false;  // debounced "pressed"
    bool swLast_ = false;
    // Commanded TCP position in manual mode, mm.
    float x_ = 0.0f, y_ = 0.0f, z_ = 0.0f;
};
