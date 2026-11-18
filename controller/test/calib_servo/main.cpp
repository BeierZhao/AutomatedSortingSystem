// Servo test sketch. Standalone: uses nothing from lib/ and not even the
// ESP32Servo library. It drives the pins with the ESP32's LEDC PWM directly,
// so a servo that moves here but not with the firmware points at software,
// and one that does not move here points at wiring, power or the servo.
//
//   pio run -e calib_servo -t upload
//   pio device monitor        (send lines ending with Enter)
//
// Commands (servo number 1 to 4, or 0 for all):
//   u <n> <microseconds>   set the pulse width, for example  u 1 1500
//   a <n> <degrees>        set a servo angle 0 to 180 using PULSE_MIN/MAX
//   s <n>                  sweep slowly between SWEEP_MIN_DEG and SWEEP_MAX_DEG
//   x <n>                  stop the pulses (servo goes limp)
//   p                      print the current state
#include <Arduino.h>

// ---------------- Constants to calibrate ----------------
const int SERVO_COUNT = 4;
const char *const NAMES[SERVO_COUNT] = {"base", "shoulder", "elbow", "wrist"};
const int PINS[SERVO_COUNT] = {11, 12, 13, 14};

// Pulse width at servo angle 0 and 180, per servo.
const int PULSE_MIN_US[SERVO_COUNT] = {544, 544, 544, 544};
const int PULSE_MAX_US[SERVO_COUNT] = {2400, 2400, 2400, 2400};

// Pulse sent at start-up. 0 = send nothing until a command arrives.
const int START_US[SERVO_COUNT] = {0, 0, 0, 0};

// Hard limits on any commanded pulse, to protect the mechanics.
const int SAFE_MIN_US = 500;
const int SAFE_MAX_US = 2500;

const int SWEEP_MIN_DEG = 70;
const int SWEEP_MAX_DEG = 110;
const int SWEEP_STEP_MS = 30;     // time per degree

const int PWM_HZ = 50;            // 20 ms frame
const int PWM_BITS = 14;          // the ESP32-S3 LEDC maximum
// ---------------------------------------------------------

static int pulseUs[SERVO_COUNT] = {0, 0, 0, 0};

static void writePulse(int i, int us) {
    if (us != 0) us = constrain(us, SAFE_MIN_US, SAFE_MAX_US);
    pulseUs[i] = us;
    const uint32_t frameUs = 1000000UL / PWM_HZ;
    const uint32_t duty = (uint32_t)(((uint64_t)us << PWM_BITS) / frameUs);
    ledcWrite(i, duty);  // LEDC channel number = servo index
}

static int angleToUs(int i, float deg) {
    deg = constrain(deg, 0.0f, 180.0f);
    return PULSE_MIN_US[i] + (int)lroundf(deg / 180.0f * (PULSE_MAX_US[i] - PULSE_MIN_US[i]));
}

static float usToAngle(int i, int us) {
    return (us - PULSE_MIN_US[i]) * 180.0f / (PULSE_MAX_US[i] - PULSE_MIN_US[i]);
}

static void printState() {
    Serial.println("servo      pin  pulse_us  angle_deg");
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (pulseUs[i] == 0) {
            Serial.printf("%d %-8s %3d       off\n", i + 1, NAMES[i], PINS[i]);
        } else {
            Serial.printf("%d %-8s %3d  %8d  %9.1f\n", i + 1, NAMES[i], PINS[i], pulseUs[i],
                          usToAngle(i, pulseUs[i]));
        }
    }
}

static void sweep(int i) {
    Serial.printf("Sweeping %s (GPIO %d) %d -> %d -> %d deg\n", NAMES[i], PINS[i],
                  SWEEP_MIN_DEG, SWEEP_MAX_DEG, SWEEP_MIN_DEG);
    for (int d = SWEEP_MIN_DEG; d <= SWEEP_MAX_DEG; d++) {
        writePulse(i, angleToUs(i, d));
        delay(SWEEP_STEP_MS);
    }
    for (int d = SWEEP_MAX_DEG; d >= SWEEP_MIN_DEG; d--) {
        writePulse(i, angleToUs(i, d));
        delay(SWEEP_STEP_MS);
    }
}

static void handle(char *line) {
    char cmd = 0;
    int n = -1;
    float value = 0.0f;
    const int fields = sscanf(line, " %c %d %f", &cmd, &n, &value);
    cmd = (char)tolower((unsigned char)cmd);

    if (fields >= 1 && cmd == 'p') {
        printState();
        return;
    }
    if (fields < 2 || n < 0 || n > SERVO_COUNT ||
        ((cmd == 'u' || cmd == 'a') && fields < 3) || !strchr("uasx", cmd)) {
        Serial.println("Usage: u <n> <us> | a <n> <deg> | s <n> | x <n> | p   (n = 1..4, 0 = all)");
        return;
    }
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (n != 0 && n != i + 1) continue;
        switch (cmd) {
            case 'u': writePulse(i, (int)value); break;
            case 'a': writePulse(i, angleToUs(i, value)); break;
            case 's': sweep(i); break;
            case 'x': writePulse(i, 0); break;
        }
    }
    printState();
}

void setup() {
    Serial.begin(115200);
    for (int i = 0; i < SERVO_COUNT; i++) {
        const double actualHz = ledcSetup(i, PWM_HZ, PWM_BITS);
        ledcAttachPin(PINS[i], i);
        writePulse(i, START_US[i]);
        if (actualHz == 0) Serial.printf("LEDC setup FAILED for %s\n", NAMES[i]);
    }
    delay(500);
    Serial.println("\n=== Servo test (raw LEDC PWM, 50 Hz) ===");
    Serial.println("u <n> <us> | a <n> <deg> | s <n> | x <n> | p   (n = 1..4, 0 = all)");
    printState();
}

void loop() {
    static char buf[48];
    static size_t len = 0;
    while (Serial.available() > 0) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (len > 0) {
                buf[len] = '\0';
                handle(buf);
                len = 0;
            }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}
