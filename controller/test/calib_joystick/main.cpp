// Joystick calibration sketch. Standalone: uses nothing from lib/.
//
//   pio run -e calib_joystick -t upload
//   pio device monitor
//
// Prints, ten times a second, what the ADC reads and what the processing
// below makes of it. Edit the constants, upload again, repeat.
#include <Arduino.h>

// ---------------- Constants to calibrate ----------------
const int PIN_VRX = 1;   // ADC1_CH0
const int PIN_VRY = 2;   // ADC1_CH1o
const int PIN_SW = 42;   // active low, internal pull-up

// ADC input range. ADC_11db reads up to about 3.1 V on the ESP32-S3, so a
// joystick powered from 3.3 V saturates near the top of its travel.
const adc_attenuation_t ADC_ATTEN = ADC_11db;
const int SAMPLES = 16;          // readings averaged per print

// Raw ADC counts (0 to 4095) at rest and at the ends of travel.
// Read them from the "raw" columns and put them here.
const int X_MIN = 0;
const int X_CENTRE = 2048;
const int X_MAX = 4095;
const int Y_MIN = 0;
const int Y_CENTRE = 2048;
const int Y_MAX = 4095;

const float DEADBAND = 0.15f;    // fraction of full travel ignored around the centre
const float X_SIGN = 1.0f;       // set to -1 to flip the axis
const float Y_SIGN = 1.0f;
const float EXPO = 1.0f;         // 1 = linear, 2 = softer near the centre

const uint32_t PRINT_MS = 100;
// ---------------------------------------------------------

// Raw counts -> -1..+1, using a separate span on each side of the centre.
static float normalise(int raw, int lo, int centre, int hi) {
    float v = raw >= centre ? (float)(raw - centre) / (float)(hi - centre)
                            : (float)(raw - centre) / (float)(centre - lo);
    return constrain(v, -1.0f, 1.0f);
}

// Deadband, rescaled so the output starts from 0 at its edge, then expo.
static float shape(float v) {
    const float mag = fabsf(v);
    if (mag < DEADBAND) return 0.0f;
    const float out = powf((mag - DEADBAND) / (1.0f - DEADBAND), EXPO);
    return v > 0.0f ? out : -out;
}

static void readAveraged(int pin, int &raw, int &millivolts) {
    long sumRaw = 0, sumMv = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sumRaw += analogRead(pin);
        sumMv += analogReadMilliVolts(pin);  // factory-calibrated, more linear
    }
    raw = (int)(sumRaw / SAMPLES);
    millivolts = (int)(sumMv / SAMPLES);
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_SW, INPUT_PULLUP);
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_VRX, ADC_ATTEN);
    analogSetPinAttenuation(PIN_VRY, ADC_ATTEN);
    delay(500);
    Serial.println("\n=== Joystick calibration ===");
    Serial.println("rawX  mvX  normX  outX | rawY  mvY  normY  outY | SW");
}

void loop() {
    int rawX, mvX, rawY, mvY;
    readAveraged(PIN_VRX, rawX, mvX);
    readAveraged(PIN_VRY, rawY, mvY);

    const float normX = X_SIGN * normalise(rawX, X_MIN, X_CENTRE, X_MAX);
    const float normY = Y_SIGN * normalise(rawY, Y_MIN, Y_CENTRE, Y_MAX);

    Serial.printf("%4d %4d %+6.2f %+6.2f | %4d %4d %+6.2f %+6.2f | %s\n",
                  rawX, mvX, normX, shape(normX),
                  rawY, mvY, normY, shape(normY),
                  digitalRead(PIN_SW) == LOW ? "PRESSED" : "-");
    delay(PRINT_MS);
}
