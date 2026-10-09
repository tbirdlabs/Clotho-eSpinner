/**
 * Clotho Foot Pedal — SparkFun Pro Micro (ATmega32U4, 5 V / 16 MHz)
 *
 * Reads a potentiometer and drives a BLD-510B brushless motor driver
 * via ~1 kHz PWM on Timer1 OC1A (pin 9).
 *
 * Serial commands (9600 baud, newline-terminated):
 *   CAL    — enter calibration mode; sweep pedal full range, then
 *             type SAVE to commit or CANCEL to abort
 *   DZ     — set dead zone percentages (heel end, toe end; each 0–50)
 *   STATUS — print stored cal min/max, dead zones, and live ADC reading
 *
 * EEPROM layout (bytes 0-6):
 *   0-1  uint16  calibrated ADC minimum
 *   2-3  uint16  calibrated ADC maximum
 *   4    uint8   dead zone percentage at heel (bottom) end   (default 5)
 *   5    uint8   dead zone percentage at toe  (top)    end   (default 3, in testing)
 *   6    uint8   magic byte (0xAB = all preceding values valid)
 */

#include <Arduino.h>
#include <EEPROM.h>

// ── Pin assignments ──────────────────────────────────────────────────────────
// Change these constants if you rewire the hardware.
static const uint8_t PIN_POT = A0;  // Potentiometer wiper  (analog input)
static const uint8_t PIN_PWM = 9;   // PWM → BLD-510B signal (Timer1 OC1A)

// ── EEPROM layout ────────────────────────────────────────────────────────────
static const uint16_t EEPROM_ADDR_MIN      = 0;     // 2 bytes — calibrated ADC min
static const uint16_t EEPROM_ADDR_MAX      = 2;     // 2 bytes — calibrated ADC max
static const uint16_t EEPROM_ADDR_DZ_BOT   = 4;     // 1 byte  — heel dead zone %
static const uint16_t EEPROM_ADDR_DZ_TOP   = 5;     // 1 byte  — toe  dead zone %
static const uint16_t EEPROM_ADDR_MAGIC    = 6;     // 1 byte  — validity sentinel
static const uint8_t  EEPROM_MAGIC_VAL     = 0xAB;  // value written when data is valid

// ── Timer1 / PWM settings ────────────────────────────────────────────────────
// Fast PWM, Mode 14 (ICR1 as TOP), prescaler ÷8.
//
//   f_PWM = F_CPU / (prescaler × (ICR1 + 1))
//         = 16 000 000 / (8 × 2 000) = 1 000 Hz
//
// To change frequency:
//   999  → 2 000 Hz
//  1999  → 1 000 Hz   ← default
//  3999  →   500 Hz
static const uint16_t PWM_TOP = 1999;

// ── Calibration ──────────────────────────────────────────────────────────────
// Minimum ADC span (of 1023) CAL will accept.  Rejects a SAVE made before the
// pedal was swept, where ADC noise alone would yield a tiny range and turn the
// pedal into an on/off switch.
static const uint16_t CAL_MIN_RANGE = 100;

// ── Serial input ─────────────────────────────────────────────────────────────
// Longest line kept from the serial console.  Extra characters are dropped so
// a stream of text with no newline can't grow a String until RAM runs out.
static const uint8_t INPUT_MAX_LEN = 16;

// ── Runtime state ────────────────────────────────────────────────────────────
static uint16_t calMin   = 0;
static uint16_t calMax   = 1023;
static uint8_t  dzBottom = 5;   // heel-end dead zone, percent of travel
// The 3% toe default lets the pedal reach full speed without being pressed hard
// against its stop.  Still in testing.  Pedals with saved settings keep theirs;
// only a blank EEPROM picks this up.
static uint8_t  dzTop    = 3;   // toe-end  dead zone, percent of travel
static bool     armed    = false;  // motor enabled only after pedal seen at rest

// ── Forward declarations ─────────────────────────────────────────────────────
static void loadSettings();
static void saveCalibration(uint16_t minVal, uint16_t maxVal);
static void saveDZ(uint8_t bot, uint8_t top);
static void setupTimer1PWM();
static void setDuty(uint8_t duty);
static void runCalibration();
static void runDZ();
static void printStatus();

// =============================================================================
void setup()
{
    // Drive the PWM pin LOW immediately so the BLD-510B's SV input isn't left
    // floating during the USB wait below.  OUTPUT is also required for the
    // Timer1 hardware override to drive the pin later.
    pinMode(PIN_PWM, OUTPUT);

    Serial.begin(9600);

    // ATmega32U4 uses USB-CDC for Serial; wait up to 2 s for host enumeration.
    // If no serial monitor is open the timeout expires and normal operation runs.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 2000)) { /* wait */ }

    pinMode(PIN_POT, INPUT);

    loadSettings();
    setupTimer1PWM();

    Serial.println(F("Clotho Foot Pedal ready.  Commands: CAL | DZ | STATUS"));
}

// =============================================================================
void loop()
{
    // ── Non-blocking serial command reader ───────────────────────────────────
    // Characters are accumulated until CR or LF, then dispatched as a command.
    static String cmdBuf = "";

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            cmdBuf.trim();
            if (cmdBuf.length() > 0) {
                cmdBuf.toUpperCase();
                if (cmdBuf == F("CAL")) {
                    runCalibration();
                } else if (cmdBuf == F("DZ")) {
                    runDZ();
                } else if (cmdBuf == F("STATUS")) {
                    printStatus();
                } else {
                    Serial.print(F("Unknown command: "));
                    Serial.println(cmdBuf);
                    Serial.println(F("Valid commands: CAL | DZ | STATUS"));
                }
            }
            cmdBuf = "";
        } else if (cmdBuf.length() < INPUT_MAX_LEN) {
            cmdBuf += c;
        }
    }

    // ── Normal operation: pot reading → PWM duty with dead zones ────────────
    uint16_t adc    = analogRead(PIN_POT);
    uint32_t travel = (calMax > calMin) ? (calMax - calMin) : 0;

    uint16_t dzBotThreshold = calMin + (uint16_t)(travel * dzBottom / 100UL);
    uint16_t dzTopThreshold = calMax - (uint16_t)(travel * dzTop    / 100UL);

    uint8_t duty;
    if (adc <= dzBotThreshold) {
        duty = 0;
    } else if (adc >= dzTopThreshold) {
        duty = 255;
    } else {
        long mapped = map((long)adc,
                          (long)dzBotThreshold, (long)dzTopThreshold,
                          0L, 255L);
        duty = (uint8_t)constrain(mapped, 0, 255);
    }

    // ── Safety interlock ─────────────────────────────────────────────────────
    // After power-up or leaving a menu, hold the motor off until the pedal is
    // seen at rest (inside the heel dead zone), so it never starts on its own.
    static bool warned = false;
    if (!armed) {
        if (duty == 0) {
            armed  = true;
            warned = false;
        } else {
            duty = 0;
            if (!warned) {
                Serial.println(F("Pedal not at rest — release it to enable the motor."));
                warned = true;
            }
        }
    }
    setDuty(duty);
}

// =============================================================================
// loadSettings
//   Read persisted calibration and dead zone values from EEPROM.  If the magic
//   byte is absent (blank EEPROM reads 0xFF) fall back to full defaults.
// =============================================================================
static void loadSettings()
{
    uint8_t magic = EEPROM.read(EEPROM_ADDR_MAGIC);
    if (magic == EEPROM_MAGIC_VAL) {
        EEPROM.get(EEPROM_ADDR_MIN,    calMin);
        EEPROM.get(EEPROM_ADDR_MAX,    calMax);
        dzBottom = EEPROM.read(EEPROM_ADDR_DZ_BOT);
        dzTop    = EEPROM.read(EEPROM_ADDR_DZ_TOP);
        Serial.print(F("Settings loaded — min="));
        Serial.print(calMin);
        Serial.print(F(", max="));
        Serial.print(calMax);
        Serial.print(F(", dzBottom="));
        Serial.print(dzBottom);
        Serial.print(F("%, dzTop="));
        Serial.print(dzTop);
        Serial.println(F("%"));
    } else {
        calMin   = 0;
        calMax   = 1023;
        dzBottom = 5;
        dzTop    = 3;
        Serial.println(F("EEPROM blank — using defaults (min=0, max=1023, dzBottom=5%, dzTop=3%)."));
    }
}

// =============================================================================
// saveCalibration
//   Persist min/max to EEPROM and re-stamp the magic byte.
//   Dead zone bytes are preserved: written back from the current runtime values
//   so the full record remains consistent without disturbing DZ.
//   EEPROM.put() performs update-only writes to reduce cell wear.
// =============================================================================
static void saveCalibration(uint16_t minVal, uint16_t maxVal)
{
    EEPROM.put(EEPROM_ADDR_MIN,   minVal);
    EEPROM.put(EEPROM_ADDR_MAX,   maxVal);
    EEPROM.update(EEPROM_ADDR_DZ_BOT, dzBottom);  // preserve current DZ values
    EEPROM.update(EEPROM_ADDR_DZ_TOP, dzTop);
    EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC_VAL);
    calMin = minVal;
    calMax = maxVal;
}

// =============================================================================
// saveDZ
//   Persist dead zone percentages to EEPROM and re-stamp the magic byte.
//   Calibration bytes are preserved from current runtime values.
// =============================================================================
static void saveDZ(uint8_t bot, uint8_t top)
{
    EEPROM.put(EEPROM_ADDR_MIN,   calMin);   // preserve current cal values
    EEPROM.put(EEPROM_ADDR_MAX,   calMax);
    EEPROM.update(EEPROM_ADDR_DZ_BOT, bot);
    EEPROM.update(EEPROM_ADDR_DZ_TOP, top);
    EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC_VAL);
    dzBottom = bot;
    dzTop    = top;
}

// =============================================================================
// setupTimer1PWM
//   Configure Timer1 for Fast PWM, Mode 14 (ICR1 as TOP) on OC1A (pin 9).
//
//   Register breakdown:
//     TCCR1A  COM1A1=1, COM1A0=0  non-inverting output on OC1A
//             WGM11=1             Fast PWM / ICR1 as TOP (bit 1 of WGM)
//     TCCR1B  WGM13=1, WGM12=1   Fast PWM / ICR1 as TOP (bits 3-2 of WGM)
//             CS11=1              ÷8 prescaler
//
//   Resulting frequency: 16 MHz / (8 × 2 000) = 1 000 Hz
//   PWM resolution: log2(2000) ≈ 11 bits
// =============================================================================
static void setupTimer1PWM()
{
    TIMSK1 = 0;         // disable Timer1 interrupts (none required here)

    ICR1  = PWM_TOP;    // set period / TOP value
    OCR1A = 0;          // initial duty = 0 % (motor off)

    // Fast PWM, non-inverting on OC1A, ÷8 prescaler — see register breakdown above
    TCCR1A = _BV(COM1A1) | _BV(WGM11);
    TCCR1B = _BV(WGM13)  | _BV(WGM12) | _BV(CS11);
}

// =============================================================================
// setDuty
//   Convert an 8-bit duty value (0–255) to a Timer1 compare value and write
//   it to OCR1A.
//
//   Scaling: OCR1A = duty × (PWM_TOP + 1) / 256
//   The >> 8 replaces the /256 division with a fast right-shift.
// =============================================================================
static void setDuty(uint8_t duty)
{
    OCR1A = ((uint32_t)duty * (PWM_TOP + 1UL)) >> 8;
}

// =============================================================================
// runCalibration
//   Interactive calibration mode.
//
//   Streams "ADC: <value>  [<obsMin>-<obsMax>]" at 10 Hz while the user
//   sweeps the pedal through its full mechanical range.  Then:
//     SAVE    — validate and commit observed min/max to EEPROM, then return
//     CANCEL  — discard and return without changing stored calibration
// =============================================================================
static void runCalibration()
{
    setDuty(0);      // motor off while in menu (this function blocks)
    armed = false;   // require pedal at rest before the motor runs again
    Serial.println(F("--- CAL MODE ---"));
    Serial.println(F("Sweep pedal through full range.  Type SAVE or CANCEL."));

    uint16_t obsMin    = 1023;
    uint16_t obsMax    = 0;
    String   input     = "";
    uint32_t lastPrint = millis();

    while (true) {
        uint16_t adc = analogRead(PIN_POT);

        // Continuously track the extremes observed since CAL started
        if (adc < obsMin) obsMin = adc;
        if (adc > obsMax) obsMax = adc;

        // Print live reading at ~10 Hz to avoid flooding the serial buffer
        uint32_t now = millis();
        if (now - lastPrint >= 100UL) {
            lastPrint = now;
            Serial.print(F("ADC: "));
            Serial.print(adc);
            Serial.print(F("  ["));
            Serial.print(obsMin);
            Serial.print(F("-"));
            Serial.print(obsMax);
            Serial.println(F("]"));
        }

        // Accumulate characters; act on CR/LF
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                input.trim();
                input.toUpperCase();

                if (input == "SAVE") {
                    if (obsMax < obsMin + CAL_MIN_RANGE) {
                        // Pedal never moved, or not swept through its full range
                        Serial.print(F("ERROR: range too narrow (need "));
                        Serial.print(CAL_MIN_RANGE);
                        Serial.println(F("+ counts) — keep sweeping before SAVE."));
                    } else {
                        saveCalibration(obsMin, obsMax);
                        Serial.print(F("Saved — min="));
                        Serial.print(calMin);
                        Serial.print(F(", max="));
                        Serial.println(calMax);
                        Serial.println(F("--- CAL END ---"));
                        return;
                    }
                } else if (input == "CANCEL") {
                    Serial.println(F("Calibration cancelled — previous values unchanged."));
                    Serial.println(F("--- CAL END ---"));
                    return;
                } else if (input.length() > 0) {
                    Serial.println(F("Type SAVE or CANCEL."));
                }
                input = "";
            } else if (input.length() < INPUT_MAX_LEN) {
                input += c;
            }
        }
    }
}

// =============================================================================
// runDZ
//   Interactive dead zone configuration.
//
//   Prompts for bottom (heel) dead zone percentage, then top (toe) dead zone
//   percentage.  Each must be in the range 0–50.  On valid input the values
//   are saved to EEPROM and take effect immediately.
// =============================================================================
static void runDZ()
{
    setDuty(0);      // motor off while in menu (this function blocks)
    armed = false;   // require pedal at rest before the motor runs again
    // Drain any trailing CR/LF left in the serial buffer from the "DZ\r\n"
    // command that invoked us.  At 9600 baud a byte arrives in ~1 ms; 5 ms is
    // enough for the paired byte to land before we peek at the buffer.
    delay(5);
    while (Serial.available()) {
        char c = (char)Serial.peek();
        if (c == '\n' || c == '\r') Serial.read();
        else break;
    }

    // ── Helper lambda equivalent: block until a complete line is received ────
    // Returns the trimmed, upper-cased line.
    auto readLine = []() -> String {
        String buf = "";
        while (true) {
            while (!Serial.available()) { /* spin */ }
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                // Consume the paired CR or LF of a \r\n / \n\r sequence so
                // the next readLine() call does not see a spurious empty line.
                delay(2);  // ~2 ms — enough for the paired byte to arrive at 9600 baud
                if (Serial.available()) {
                    char next = (char)Serial.peek();
                    if (next == '\n' || next == '\r') Serial.read();
                }
                buf.trim();
                return buf;
            }
            if (buf.length() < INPUT_MAX_LEN) buf += c;
        }
    };

    // ── Parse a dead zone percentage ─────────────────────────────────────────
    // Returns 0–50 for a whole number in range, or -1 for anything else
    // (non-digits, too many digits, or out of range).  toInt() alone would
    // silently turn "x" into 0 and "10abc" into 10.
    auto parsePercent = [](const String &s) -> long {
        if (s.length() == 0 || s.length() > 2) return -1;
        for (unsigned int i = 0; i < s.length(); i++) {
            if (!isDigit(s[i])) return -1;
        }
        long v = s.toInt();
        return (v <= 50) ? v : -1;
    };

    // ── Bottom dead zone ─────────────────────────────────────────────────────
    Serial.print(F("Enter bottom (heel) dead zone % [0-50], current="));
    Serial.print(dzBottom);
    Serial.println(F(":"));

    String line = readLine();
    if (line.length() == 0) {
        Serial.println(F("DZ cancelled."));
        return;
    }
    long newBot = parsePercent(line);
    if (newBot < 0) {
        Serial.println(F("ERROR: enter a whole number 0–50.  DZ cancelled."));
        return;
    }

    // ── Top dead zone ────────────────────────────────────────────────────────
    Serial.print(F("Enter top (toe) dead zone % [0-50], current="));
    Serial.print(dzTop);
    Serial.println(F(":"));

    line = readLine();
    if (line.length() == 0) {
        Serial.println(F("DZ cancelled."));
        return;
    }
    long newTop = parsePercent(line);
    if (newTop < 0) {
        Serial.println(F("ERROR: enter a whole number 0–50.  DZ cancelled."));
        return;
    }

    saveDZ((uint8_t)newBot, (uint8_t)newTop);

    Serial.print(F("Dead zones saved — bottom="));
    Serial.print(dzBottom);
    Serial.print(F("%, top="));
    Serial.print(dzTop);
    Serial.println(F("%"));
}

// =============================================================================
// printStatus
//   Report stored calibration bounds, dead zone settings, and live ADC reading.
// =============================================================================
static void printStatus()
{
    uint16_t adc = analogRead(PIN_POT);
    Serial.print(F("Cal min="));
    Serial.print(calMin);
    Serial.print(F(", max="));
    Serial.print(calMax);
    Serial.print(F(" | dzBottom="));
    Serial.print(dzBottom);
    Serial.print(F("%, dzTop="));
    Serial.print(dzTop);
    Serial.print(F("% | Live ADC="));
    Serial.println(adc);
}
