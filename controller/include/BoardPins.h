// Pin assignment for the ESP32-S3-DevKitC-1 (docs/development_guide.md,
// section 4.2).
//
// Checked against the ESP32-S3 datasheet and the DevKitC-1 pinout:
//  - None of these is a strapping pin (0, 3, 45, 46), a USB pin (19, 20),
//    a UART0 pin (43, 44), or a flash/PSRAM pin (26 to 32 on every module,
//    33 to 37 on octal flash/PSRAM modules such as N8R8 / N16R8).
//  - GPIO 1 and 2 are ADC1 channels 0 and 1, so they keep working with
//    Wi-Fi on. (GPIO 11 to 18 are ADC2, used here only as digital pins.)
//  - GPIO 15 and 16 double as the 32 kHz crystal pins, which the DevKitC-1
//    leaves unconnected, so they are free.
//  - GPIO 48 (38 on board revision 1.1) drives the on-board RGB LED and is
//    left alone.
// Not verified on real hardware yet.
#pragma once

namespace pins {

const int kServoBase = 11;
const int kServoShoulder = 12;
const int kServoElbow = 13;
const int kServoWrist = 14;

// 4-DOF arm project only.
const int kJoystickVrx = 1;      // ADC1_CH0. Power the joystick from 3.3 V.
const int kJoystickVry = 2;      // ADC1_CH1
const int kJoystickSw = 42;      // internal pull-up, active low
const int kUltrasonicTrig = 15;
const int kUltrasonicEcho = 16;  // through a 5 V -> 3.3 V divider

}  // namespace pins
