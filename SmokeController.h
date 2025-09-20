#pragma once
#include <Arduino.h>

class SmokeController {
public:
    enum State { IDLE, PULSING, WAITING, FULL_ON };

    using StateChangeCallback = void (*)(State);
    using SimpleCallback      = void (*)();

    SmokeController(int readyPin, int readyLedPin,
                    int smokePin, int pulseLedPin, int feedbackPin,
                    unsigned long dmxTimeout = 3000, bool enableFailsafe = true,
                    uint16_t minPulse = 100, uint16_t maxPulse = 900,
                    uint16_t maxPeriod = 10000, uint16_t minPeriod = 1000)
        : readyPin(readyPin), readyLedPin(readyLedPin),
          smokePin(smokePin), pulseLedPin(pulseLedPin), feedbackPin(feedbackPin),
          dmxTimeout(dmxTimeout), failsafeEnabled(enableFailsafe),
          minPulse(minPulse), maxPulse(maxPulse),
          maxPeriod(maxPeriod), minPeriod(minPeriod) {}

    void begin() {
        if (readyPin >= 0) pinMode(readyPin, INPUT_PULLUP);
        if (readyLedPin >= 0) pinMode(readyLedPin, OUTPUT);
        if (pulseLedPin >= 0) pinMode(pulseLedPin, OUTPUT);
        if (feedbackPin >= 0) pinMode(feedbackPin, OUTPUT);
        if (smokePin >= 0) pinMode(smokePin, OUTPUT);

        smokeOff();
        lastDMXMillis = millis();
        lastTransitionTime = millis();
    }

    // Caller provides the latest DMX value (0–255)
    void setDMXValue(byte value) {
        dmxValue = value;
        lastDMXMillis = millis();
    }

    // Register callbacks
    void onStateChange(StateChangeCallback cb) { stateCb = cb; }
    void onSmokeOn(SimpleCallback cb)          { smokeOnCb = cb; }
    void onSmokeOff(SimpleCallback cb)         { smokeOffCb = cb; }

    void update() {
        unsigned long now = millis();

        // READY state (always true if readyPin == -1)
        bool readyState = (readyPin >= 0) ? digitalRead(readyPin) : true;

        // READY indicator LED mirrors machine status (if enabled)
        if (readyLedPin >= 0) {
            digitalWrite(readyLedPin, !readyState);
        }

        // Warm-up handling
        if (readyState != prevReadyState) {
            prevReadyState = readyState;
            if (readyState) {
                nextPulseTime = now;  // can start immediately
            } else {
                smokeOff();
            }
        }

        if (!readyState) {
            smokeOff();
            return;
        }

        // --- DMX failsafe ---
        if (failsafeEnabled && (millis() - lastDMXMillis > dmxTimeout)) {
            if (feedbackPin >= 0) analogWrite(feedbackPin, 0);
            smokeOff();
            return;
        } else {
            if (feedbackPin >= 0) analogWrite(feedbackPin, 255-dmxValue);
        }

        // DMX = 0 → OFF
        if (dmxValue == 0) {
            smokeOff();
            return;
        }

        // DMX = 255 → FULL_ON
        if (dmxValue == 255) {
            if (state != FULL_ON) smokeFullOn();
            return;
        } else if (state == FULL_ON) {
            smokeOff(); // Exit FULL_ON
        }

        // --- Pulsing logic ---
        byte safeValue = constrain(dmxValue, 1, 254);
        uint16_t newPulseDuration = map(safeValue, 1, 254, minPulse, maxPulse);
        uint16_t newPeriod        = map(safeValue, 1, 254, maxPeriod, minPeriod);

        if (state == PULSING) {
            unsigned long elapsed = now - pulseStartTime;
            if (elapsed >= newPulseDuration) {
                smokeOff();
                nextPulseTime = now + (newPeriod - newPulseDuration);
                changeState(WAITING);
            } else {
                pulseDuration = newPulseDuration;
                period = newPeriod;
            }
        }
        else if (state == WAITING) {
            unsigned long remaining = (nextPulseTime > now) ? nextPulseTime - now : 0;
            if (remaining > newPeriod - newPulseDuration) {
                nextPulseTime = now + (newPeriod - newPulseDuration);
            }
            pulseDuration = newPulseDuration;
            period = newPeriod;

            if (now >= nextPulseTime) smokeOn();
        }
        else if (state == IDLE) {
            pulseDuration = newPulseDuration;
            period = newPeriod;
            if (now >= nextPulseTime) smokeOn();
        }
    }

    // --- Public queries ---
    State getState() const { return state; }
    byte getDMXValue() const { return dmxValue; }

    unsigned long timeSinceTransition() const {
        return millis() - lastTransitionTime;
    }

    unsigned long timeSinceSmokeOn() const {
        if (isSmokeActive()) return millis() - lastTransitionTime;
        return 0;
    }

    unsigned long timeSinceSmokeOff() const {
        if (!isSmokeActive()) return millis() - lastTransitionTime;
        return 0;
    }

    bool isSmokeActive() const {
        return (state == PULSING || state == FULL_ON);
    }

private:
    // --- Pins & config ---
    int readyPin, readyLedPin, smokePin, pulseLedPin, feedbackPin;
    unsigned long dmxTimeout;
    bool failsafeEnabled;

    // --- Timing ranges ---
    uint16_t minPulse, maxPulse;
    uint16_t maxPeriod, minPeriod;

    // --- Timing state ---
    uint16_t pulseDuration = 200;
    uint16_t period        = 5000;
    unsigned long pulseStartTime = 0;
    unsigned long nextPulseTime  = 0;
    unsigned long lastDMXMillis  = 0;
    unsigned long lastTransitionTime = 0;

    // --- State ---
    State state = IDLE;
    byte dmxValue = 0;
    bool prevReadyState = false;

    // --- Callbacks ---
    StateChangeCallback stateCb = nullptr;
    SimpleCallback smokeOnCb    = nullptr;
    SimpleCallback smokeOffCb   = nullptr;

    // --- Helpers ---
    void smokeOn() {
        if (smokePin >= 0) digitalWrite(smokePin, HIGH);
        if (pulseLedPin >= 0) digitalWrite(pulseLedPin, HIGH);
        pulseStartTime = millis();
        changeState(PULSING);
        if (smokeOnCb) smokeOnCb();
    }

    void smokeFullOn() {
        if (smokePin >= 0) digitalWrite(smokePin, HIGH);
        if (pulseLedPin >= 0) digitalWrite(pulseLedPin, HIGH);
        changeState(FULL_ON);
        if (smokeOnCb) smokeOnCb();
    }

    void smokeOff() {
        if (state != IDLE && state != WAITING) {
            if (smokeOffCb) smokeOffCb();
        }
        if (smokePin >= 0) digitalWrite(smokePin, LOW);
        if (pulseLedPin >= 0) digitalWrite(pulseLedPin, LOW);
        changeState(IDLE);
    }

    void changeState(State newState) {
        if (newState != state) {
            state = newState;
            lastTransitionTime = millis();
            if (stateCb) stateCb(state);
        }
    }
};