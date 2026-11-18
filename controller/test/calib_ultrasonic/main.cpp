// Ultrasonic sensor calibration sketch (HC-SR04 style).
// Standalone: uses nothing from lib/.
//
//   pio run -e calib_ultrasonic -t upload
//   pio device monitor
//
// Put a flat target at known distances (for example 50, 100, 200, 400 mm),
// compare the "raw mm" column with a ruler, then set SCALE and OFFSET_MM so
// that the "corrected" column matches.
#include <Arduino.h>

// ---------------- Constants to calibrate ----------------
const int PIN_TRIG = 15;
const int PIN_ECHO = 16;          // through a 5 V -> 3.3 V divider

const float AIR_TEMP_C = 22.0f;   // speed of sound depends on temperature
const float SCALE = 1.0f;         // corrected = SCALE * raw + OFFSET_MM
const float OFFSET_MM = 0.0f;     // for example the sensor face to TCP distance

const int MIN_VALID_MM = 20;      // the sensor cannot measure closer than this
const int MAX_VALID_MM = 4000;    // longer echoes count as "nothing in range"
const uint32_t ECHO_TIMEOUT_US = 30000;

const int MEDIAN_WINDOW = 5;      // odd number of readings in the median filter
const uint32_t PERIOD_MS = 60;    // keep at 50 or more so old echoes die out
// ---------------------------------------------------------

static int window[MEDIAN_WINDOW];
static int windowNext = 0;

// Echo pulse width in microseconds, 0 if no echo came back.
static uint32_t measureEchoUs() {
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(4);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    return pulseIn(PIN_ECHO, HIGH, ECHO_TIMEOUT_US);
}

static int median() {
    int sorted[MEDIAN_WINDOW];
    memcpy(sorted, window, sizeof(sorted));
    for (int i = 1; i < MEDIAN_WINDOW; i++) {  // insertion sort
        const int v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }
    return sorted[MEDIAN_WINDOW / 2];
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    for (int i = 0; i < MEDIAN_WINDOW; i++) window[i] = INT32_MAX;
    delay(500);
    Serial.println("\n=== Ultrasonic calibration ===");
    Serial.println("echo_us  raw_mm  corrected_mm  median_mm  (-1 = nothing in range)");
}

void loop() {
    // Speed of sound in mm per microsecond, halved for the round trip.
    const float mmPerUs = (331.3f + 0.606f * AIR_TEMP_C) / 1000.0f / 2.0f;

    const uint32_t echoUs = measureEchoUs();
    const float rawMm = echoUs * mmPerUs;
    const float correctedMm = SCALE * rawMm + OFFSET_MM;
    const bool valid = echoUs > 0 && rawMm >= MIN_VALID_MM && rawMm <= MAX_VALID_MM;

    // "Nothing in range" sorts as very far in the median.
    window[windowNext] = valid ? (int)lroundf(correctedMm) : INT32_MAX;
    windowNext = (windowNext + 1) % MEDIAN_WINDOW;
    const int med = median();

    Serial.printf("%7lu  %6.1f  %12.1f  %9d%s\n", (unsigned long)echoUs, rawMm,
                  valid ? correctedMm : -1.0f, med == INT32_MAX ? -1 : med,
                  echoUs == 0 ? "   no echo (check wiring, 5 V supply, divider)" : "");
    delay(PERIOD_MS);
}
