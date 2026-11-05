/* =====================================================================
 *  ROG-BEEP / TX-TAIL CONTROLLER              -  Arduino NANO
 *  =====================================================================
 *
 *  WHAT THIS DOES
 *  --------------
 *  This sketch sits between your microphone (PTT button) and your
 *  transceiver. It provides:
 *
 *    1. The microphone PTT is picked up and passed 1-to-1 to the radio,
 *       at the same speed as without the NANO (delay < ~20 ms).
 *    2. When you release the PTT, the NANO keeps the radio in transmit
 *       for a "tail" that consists of three consecutive parts:
 *           a. TXLEAD ms  : silent lead-time (carrier only, no tone)
 *           b. TXTONE ms  : the 1..3 beeps (700..2000 Hz), sent to the
 *                           microphone audio line through a capacitor
 *           c. TXEXIT ms  : silent exit-time (carrier only, no tone)
 *       Total tail = TXLEAD + TXTONE + TXEXIT.
 *    3. The tones run back to back inside the TXTONE part: with 3 tones,
 *       each tone lasts exactly TXTONE/3, with no gap.
 *    4. The three times, the number of tones and the frequencies are
 *       configurable (at compile time via the constants below, and/or at
 *       runtime via the serial monitor).
 *
 *  HARDWARE
 *  --------
 *    D2   PTT-in     microphone PTT line via 10k (INPUT_PULLUP, LOW=pressed)
 *    D3   button     short press : play the tone pattern once, no transmit
 *                   long press  : toggle "chime" mode (hold LONGPRESS_MS).
 *                                In chime mode the tone pattern repeats
 *                                endlessly and the PTT stays active until
 *                                the next long press.
 *    D8   PTT-out    via 10k to the base of an NPN; collector to the radio's
 *                   PTT input (open collector, switches to GND)
 *    D9   tone-out   via capacitor + pot to the microphone audio line
 *    D13  LED        lit while transmitting (tail included)
 *
 *    Nano note: A6 and A7 are the I2C pins and CANNOT be used as digital
 *    I/O. That is why everything here runs on D2/D3/D8/D9.
 *    The 5V pin may supply at most ~500 mA (including the NANO itself).
 *
 *  FILES
 *  -----
 *    rogbeep.ino        this file
 *    MANUAL.md          manual + wiring diagrams + troubleshooting
 *
 *  ===================================================================== */

#include <EEPROM.h>

/* ===================== 1. USER SETTINGS ==============================
 * These are the DEFAULTS. At startup they are overwritten with the values
 * stored in the EEPROM (see 'save' in the serial monitor).
 * Change the #defines here and re-upload to change the defaults.
 * ==================================================================== */

/* The tail after releasing the PTT is split into three parts:
 *   TXLEAD  silent lead-time  | TXTONE  the tones  | TXEXIT  silent exit-time
 * Each part is 0..1000 ms. Total tail = TXLEAD + TXTONE + TXEXIT. */
#define TXLEAD_DEF    200UL   /* silent lead-time before the tones, 0..1000 ms */
#define TXTONE_DEF    400UL   /* total length of the tones, 0..1000 ms         */
#define TXEXIT_DEF    200UL   /* silent exit-time after the tones, 0..1000 ms  */

#define NBEEPS_DEF    1     /* number of tones in the tail, 1..3         */
#define FREQ1_DEF     1800U   /* tone 1 in Hz, 700..2000                    */
#define FREQ2_DEF     900U   /* tone 2 in Hz, 700..2000                    */
#define FREQ3_DEF     1350U   /* tone 3 in Hz, 700..2000                    */

/* upper limit for each of the three times (ms) */
#define TIME_MAX      1000UL

/* debounce time of the PTT input (ms) */
#define DEBOUNCE_MS   15UL

/* length of one chime cycle (ms). The chime always runs on this length,
 * independent of TXLEAD/TXTONE/TXEXIT: with 3 tones that is 600 ms per
 * tone. The chime has no lead-time or exit-time. */
#define CHIME_CYCLE_MS 1800UL

/* number of tones in the chime. The chime is a fixed little tune and is
 * deliberately independent of the 'beeps' setting of the tail. */
#define CHIME_TONES    3

/* how long to hold the button for chime mode (ms) */
#define LONGPRESS_MS  3000UL

/* ===================== 2. BOARD NAME ================================ */

#if defined(ARDUINO_AVR_NANO) || defined(ARDUINO_NANO)
  #define BOARD_NAME "Arduino Nano (ATmega328P)"
#elif defined(ARDUINO_AVR_NANO_EVERY) || defined(ARDUINO_NANO_EVERY)
  #define BOARD_NAME "Arduino Nano Every (ATmega4809)"
#else
  #define BOARD_NAME "Arduino"
#endif

/* ===================== 3. PINS ====================================== */

#define PIN_PTT_IN    2
#define PIN_PTT_OUT   8
#define PIN_TONE      9
#define PIN_LED       13
#define PIN_TEST      3

/* ===================== 4. EEPROM LAYOUT ============================ */

#define EE_MAGIC      0        /* 1 byte  marker                          */
#define EE_VERSION    1        /* 1 byte  settings version                */
#define EE_LEAD       2        /* 2 bytes TXLEAD (uint16)                 */
#define EE_TONE       4        /* 2 bytes TXTONE (uint16)                 */
#define EE_EXIT       6        /* 2 bytes TXEXIT (uint16)                 */
#define EE_NBEEPS     8        /* 1 byte  number of tones                  */
#define EE_FREQ       9        /* 6 bytes 3 x frequency (uint16)           */

#define EE_MAGIC_VAL  0xA5
#define EE_VER_VAL    3        /* layout changed -> old EEPROM data is ignored */

/* ===================== 5. STATE ==================================== */

enum TxState { ST_IDLE, ST_TX, ST_TAIL, ST_TEST, ST_LOOP };

TxState       g_state       = ST_IDLE;
unsigned long g_txlead      = TXLEAD_DEF;   /* silent lead-time (ms)      */
unsigned long g_txtone      = TXTONE_DEF;   /* total tone time (ms)       */
unsigned long g_txexit      = TXEXIT_DEF;   /* silent exit-time (ms)      */
uint8_t       g_nbeeps      = NBEEPS_DEF;
uint16_t      g_freq[3]     = { FREQ1_DEF, FREQ2_DEF, FREQ3_DEF };

unsigned long g_patternStart = 0UL;   /* pattern start time (ms)          */
unsigned long g_patternLead  = 0UL;   /* silent lead part of the pattern  */
unsigned long g_patternTone  = 0UL;   /* tone part of the pattern         */
unsigned long g_patternLen   = 0UL;   /* total pattern length (ms)        */
uint8_t       g_patternTones = 1;      /* tones in the running pattern     */

int          g_rawPtt       = HIGH;   /* last raw PTT reading             */
int          g_stablePtt    = HIGH;   /* debounced PTT reading            */
unsigned long g_rawChange   = 0UL;    /* when the raw value changed       */
uint16_t     g_toneNow      = 0;      /* current tone frequency (0=silent)*/

/* button */
bool         g_btnDown      = false;  /* button is pressed                */
bool         g_btnLongFired = false;  /* long press already triggered     */
unsigned long g_btnDownMs   = 0UL;    /* moment the button went down      */
bool         g_loopMode     = false;  /* chime mode active                */

String       g_line;                   /* serial command line              */

/* ===================== 6. HELPERS =================================== */

/* PTT output + LED.  'on' = transmitting (tail included). */
void setPtt(bool on) {
  digitalWrite(PIN_PTT_OUT, on ? HIGH : LOW);
  digitalWrite(PIN_LED,     on ? HIGH : LOW);
}

/* Tone on/off. tone() is only called when the frequency changes,
 * otherwise the waveform would be restarted on every loop. */
void setTone(uint16_t f) {
  if (f == g_toneNow) return;
  if (f == 0) {
    noTone(PIN_TONE);
  } else {
    tone(PIN_TONE, f);
  }
  g_toneNow = f;
}

/* Read the PTT input with a simple debounce.
 * The input uses INPUT_PULLUP: HIGH = released, LOW = pressed. */
bool readPtt() {
  int raw = digitalRead(PIN_PTT_IN);
  if (raw != g_rawPtt) {
    g_rawPtt     = raw;
    g_rawChange  = millis();
  }
  if (millis() - g_rawChange >= DEBOUNCE_MS && raw != g_stablePtt) {
    g_stablePtt = raw;
  }
  return (g_stablePtt == LOW);
}

/* Start a tone pattern (also used by the button).
 * A pattern consists of three consecutive parts:
 *   'lead'    = silent lead-time in ms
 *   'toneLen' = length of the tone part in ms, split into 'tones' equal tones
 *   'exitLen' = silent exit-time in ms
 * 'tones' = number of tones (1..3). The tail and the preview use the
 *           'beeps' setting, the chime uses CHIME_TONES. */
void startPattern(unsigned long lead, unsigned long toneLen,
                  unsigned long exitLen, uint8_t tones) {
  g_patternStart = millis();
  g_patternLead  = lead;
  g_patternTone  = toneLen;
  g_patternLen   = lead + toneLen + exitLen;
  g_patternTones = (tones < 1) ? 1 : ((tones > 3) ? 3 : tones);
}

/* Start the tail / preview pattern with the current settings. */
void startTailPattern() {
  startPattern(g_txlead, g_txtone, g_txexit, g_nbeeps);
}

/* Work out which tone should be sounding at this moment.
 *
 * Timeline of a pattern:
 *   0 .. lead                 silence
 *   lead .. lead+toneLen      the tones, split into g_patternTones equal
 *                             parts that run back to back
 *   lead+toneLen .. end       silence (exit-time)
 *
 * Example: lead 200, toneLen 600, 3 tones, exit 200:
 *   0-200 silence | 200-400 tone 1 | 400-600 tone 2 | 600-800 tone 3 |
 *   800-1000 silence
 *
 * 'elapsed' = ms since the start of the pattern. */
void updatePattern(unsigned long elapsed) {
  if (g_patternLen == 0UL || g_patternTones == 0 || g_patternTone == 0UL) {
    setTone(0);
    return;
  }

  /* lead-time: silent */
  if (elapsed < g_patternLead) { setTone(0); return; }

  unsigned long rel = elapsed - g_patternLead;

  /* exit-time: silent */
  if (rel >= g_patternTone) { setTone(0); return; }

  /* which tone segment are we in? */
  uint8_t idx = g_patternTones;
  for (uint8_t i = 0; i < g_patternTones; i++) {
    unsigned long segStart = (g_patternTone * i) / g_patternTones;
    unsigned long segEnd   = (g_patternTone * (i + 1)) / g_patternTones;
    if (rel >= segStart && rel < segEnd) { idx = i; break; }
  }

  if (idx < g_patternTones) {
    setTone(g_freq[idx]);
  } else {
    setTone(0);
  }
}

/* ===================== 6b. BUTTON ================================== */

/* Toggle chime mode.
 * ON : the tone pattern repeats endlessly and the PTT stays active.
 *      The cycle always lasts CHIME_CYCLE_MS, independent of the
 *      TXLEAD / TXTONE / TXEXIT settings.
 * OFF: no more tone, PTT inactive, back to normal operation. */
void toggleChime() {
  g_loopMode = !g_loopMode;
  if (g_loopMode) {
    g_state = ST_LOOP;
    startPattern(0UL, CHIME_CYCLE_MS, 0UL, CHIME_TONES);
    setPtt(true);
    Serial.print(F("Chime mode ON: chime + PTT active, cycle "));
    Serial.print(CHIME_CYCLE_MS);
    Serial.println(F(" ms"));
  } else {
    setTone(0);
    setPtt(false);
    g_state = ST_IDLE;
    Serial.println(F("Chime mode OFF: PTT inactive"));
  }
}

/* Button handling:
 *   short press (less than LONGPRESS_MS) = play the tone pattern once,
 *                                          without transmitting
 *   long press  (LONGPRESS_MS or more)   = toggle chime mode
 * The long press fires while you are still holding the button; chime mode
 * stays on until you hold the button down for LONGPRESS_MS again. */
void handleButton(unsigned long now) {
  bool down = (digitalRead(PIN_TEST) == LOW);

  if (down && !g_btnDown) {                 /* just pressed */
    g_btnDown      = true;
    g_btnLongFired = false;
    g_btnDownMs    = now;
    return;
  }

  if (down && g_btnDown) {                  /* still pressed */
    if (!g_btnLongFired && (now - g_btnDownMs) >= LONGPRESS_MS) {
      g_btnLongFired = true;
      toggleChime();
    }
    return;
  }

  if (!down && g_btnDown) {                 /* released */
    if (!g_btnLongFired && g_state == ST_IDLE) {
      /* short press: play the pattern once, without PTT */
      startTailPattern();
      g_state = ST_TEST;
    }
    g_btnDown      = false;
    g_btnLongFired = false;
  }
}

/* ===================== 7. SETTINGS ================================= */

void applyDefaults() {
  g_txlead  = TXLEAD_DEF;
  g_txtone  = TXTONE_DEF;
  g_txexit  = TXEXIT_DEF;
  g_nbeeps  = NBEEPS_DEF;
  g_freq[0] = FREQ1_DEF;
  g_freq[1] = FREQ2_DEF;
  g_freq[2] = FREQ3_DEF;
}

bool loadSettings() {
  if (EEPROM.read(EE_MAGIC) != EE_MAGIC_VAL) return false;
  if (EEPROM.read(EE_VERSION) != EE_VER_VAL) return false;

  g_txlead  = (unsigned long)EEPROM.read(EE_LEAD) |
              ((unsigned long)EEPROM.read(EE_LEAD + 1) << 8);
  g_txtone  = (unsigned long)EEPROM.read(EE_TONE) |
              ((unsigned long)EEPROM.read(EE_TONE + 1) << 8);
  g_txexit  = (unsigned long)EEPROM.read(EE_EXIT) |
              ((unsigned long)EEPROM.read(EE_EXIT + 1) << 8);
  g_nbeeps  = EEPROM.read(EE_NBEEPS);
  for (uint8_t i = 0; i < 3; i++) {
    g_freq[i] = (uint16_t)EEPROM.read(EE_FREQ + i * 2) |
                ((uint16_t)EEPROM.read(EE_FREQ + i * 2 + 1) << 8);
  }

  /* sanity check: fall back to the defaults for out-of-range values */
  if (g_txlead > TIME_MAX) g_txlead = TXLEAD_DEF;
  if (g_txtone > TIME_MAX) g_txtone = TXTONE_DEF;
  if (g_txexit > TIME_MAX) g_txexit = TXEXIT_DEF;
  if (g_nbeeps < 1 || g_nbeeps > 3) g_nbeeps = NBEEPS_DEF;
  for (uint8_t i = 0; i < 3; i++) {
    if (g_freq[i] < 700U || g_freq[i] > 2000U) g_freq[i] = FREQ1_DEF;
  }
  return true;
}

void saveSettings() {
  EEPROM.write(EE_MAGIC,   EE_MAGIC_VAL);
  EEPROM.write(EE_VERSION, EE_VER_VAL);
  EEPROM.update(EE_LEAD,      (uint8_t)(g_txlead & 0xFF));
  EEPROM.update(EE_LEAD + 1,  (uint8_t)((g_txlead >> 8) & 0xFF));
  EEPROM.update(EE_TONE,      (uint8_t)(g_txtone & 0xFF));
  EEPROM.update(EE_TONE + 1,  (uint8_t)((g_txtone >> 8) & 0xFF));
  EEPROM.update(EE_EXIT,      (uint8_t)(g_txexit & 0xFF));
  EEPROM.update(EE_EXIT + 1,  (uint8_t)((g_txexit >> 8) & 0xFF));
  EEPROM.update(EE_NBEEPS, g_nbeeps);
  for (uint8_t i = 0; i < 3; i++) {
    EEPROM.update(EE_FREQ + i * 2,     (uint8_t)(g_freq[i] & 0xFF));
    EEPROM.update(EE_FREQ + i * 2 + 1, (uint8_t)((g_freq[i] >> 8) & 0xFF));
  }
}

/* ===================== 8. SERIAL CONTROL ============================ */

/* Reads the commands from the serial monitor.
 * Commands (only the first letter is needed):
 *   help / ?              overview of all commands
 *   status                show the current settings
 *   lead <ms>             silent lead-time before the tones, 0..1000 ms
 *   tone <ms>             total length of the tones, 0..1000 ms
 *   exit <ms>             silent exit-time after the tones, 0..1000 ms
 *   beeps <1-3>           number of tones
 *   f1 <hz>               frequency of tone 1 (700..2000)
 *   f2 <hz>               frequency of tone 2 (700..2000)
 *   f3 <hz>               frequency of tone 3 (700..2000)
 *   test                  play the tone pattern without transmitting
 *   save                  write the settings to the EEPROM
 *   load                  read the settings from the EEPROM
 *   defaults              restore the compile-time defaults
 */
void printHelp() {
  Serial.println(F("Commands (one per line, press enter):"));
  Serial.println(F("  help/?            this overview"));
  Serial.println(F("  status            current settings"));
  Serial.println(F("  lead <0-1000>     silent lead-time in ms"));
  Serial.println(F("  tone <0-1000>     total tone time in ms"));
  Serial.println(F("  exit <0-1000>     silent exit-time in ms"));
  Serial.println(F("  beeps <1-3>       number of tones"));
  Serial.println(F("  f1 <700-2000>     frequency of tone 1 (Hz)"));
  Serial.println(F("  f2 <700-2000>     frequency of tone 2 (Hz)"));
  Serial.println(F("  f3 <700-2000>     frequency of tone 3 (Hz)"));
  Serial.println(F("  test              play the tone pattern (no TX)"));
  Serial.println(F("  save              save to EEPROM"));
  Serial.println(F("  load              load from EEPROM"));
  Serial.println(F("  defaults          restore default values"));
}

void printStatus() {
  Serial.print(F("TXLEAD    : ")); Serial.print(g_txlead); Serial.println(F(" ms (silent)"));
  Serial.print(F("TXTONE    : ")); Serial.print(g_txtone); Serial.println(F(" ms (tones)"));
  Serial.print(F("TXEXIT    : ")); Serial.print(g_txexit); Serial.println(F(" ms (silent)"));
  Serial.print(F("Total tail: ")); Serial.print(g_txlead + g_txtone + g_txexit);
  Serial.println(F(" ms"));
  Serial.print(F("Tones     : ")); Serial.print(g_nbeeps); Serial.println(F(""));
  for (uint8_t i = 0; i < 3; i++) {
    Serial.print(F("Tone ")); Serial.print(i + 1); Serial.print(F("     : "));
    Serial.print(g_freq[i]); Serial.println(F(" Hz"));
  }
  if (g_nbeeps > 1) {
    Serial.print(F("Per tone  : ")); Serial.print(g_txtone / g_nbeeps);
    Serial.println(F(" ms (back to back)"));
  }
  Serial.print(F("Chime     : "));
  Serial.println(g_loopMode ? F("ON (PTT active)") : F("off"));
  Serial.print(F("Chime cyc : ")); Serial.print(CHIME_CYCLE_MS);
  Serial.print(F(" ms, ")); Serial.print(CHIME_TONES);
  Serial.println(F(" tones (independent of lead/tone/exit and 'beeps')"));
}

/* Parse "command [number]" from a line. */
bool parseCommand(const String &s, char *cmd, size_t cmdSize, long &val, bool &hasVal) {
  int i = 0;
  while (i < (int)s.length() && s[i] == ' ') i++;

  size_t j = 0;
  while (i < (int)s.length() && s[i] != ' ' && j < cmdSize - 1) {
    cmd[j++] = s[i];
    i++;
  }
  cmd[j] = '\0';
  if (j == 0) return false;

  while (i < (int)s.length() && s[i] == ' ') i++;

  hasVal = false;
  val    = 0;
  if (i < (int)s.length()) {
    bool neg = false;
    if (s[i] == '-') { neg = true; i++; }
    long v = 0;
    bool any = false;
    while (i < (int)s.length() && s[i] >= '0' && s[i] <= '9') {
      v = v * 10 + (s[i] - '0');
      i++;
      any = true;
    }
    if (any) { val = neg ? -v : v; hasVal = true; }
  }
  return true;
}

void processCommand(String s) {
  s.trim();
  s.toLowerCase();

  char cmd[16];
  long val = 0;
  bool hasVal = false;
  if (!parseCommand(s, cmd, sizeof(cmd), val, hasVal)) return;

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    printHelp();
  }
  else if (!strcmp(cmd, "status")) {
    printStatus();
  }
  else if (!strcmp(cmd, "lead") || !strcmp(cmd, "tone") || !strcmp(cmd, "exit")) {
    if (!hasVal) { Serial.print(F("Usage: ")); Serial.print(cmd); Serial.println(F(" <0-1000>")); return; }
    if (val < 0 || val > (long)TIME_MAX) { Serial.println(F("Error: value must be 0..1000")); return; }
    if (cmd[0] == 'l') {
      g_txlead = (unsigned long)val;
      Serial.print(F("TXLEAD = ")); Serial.print(g_txlead);
    } else if (cmd[0] == 't') {
      g_txtone = (unsigned long)val;
      Serial.print(F("TXTONE = ")); Serial.print(g_txtone);
    } else {
      g_txexit = (unsigned long)val;
      Serial.print(F("TXEXIT = ")); Serial.print(g_txexit);
    }
    Serial.println(F(" ms"));
    /* NB: the chime cycle has its own length (CHIME_CYCLE_MS) and is not
       affected by these settings. */
  }
  else if (!strcmp(cmd, "beeps")) {
    if (!hasVal) { Serial.println(F("Usage: beeps <1-3>")); return; }
    if (val < 1 || val > 3) { Serial.println(F("Error: value must be 1..3")); return; }
    g_nbeeps = (uint8_t)val;
    Serial.print(F("Number of tones = ")); Serial.println(g_nbeeps);
  }
  else if (!strcmp(cmd, "f1") || !strcmp(cmd, "f2") || !strcmp(cmd, "f3")) {
    if (!hasVal) { Serial.println(F("Usage: f1/f2/f3 <700-2000>")); return; }
    if (val < 700 || val > 2000) { Serial.println(F("Error: value must be 700..2000")); return; }
    uint8_t idx = (uint8_t)(cmd[1] - '1');
    g_freq[idx] = (uint16_t)val;
    Serial.print(F("Tone ")); Serial.print(idx + 1); Serial.print(F(" = "));
    Serial.print(g_freq[idx]); Serial.println(F(" Hz"));
  }
  else if (!strcmp(cmd, "test")) {
    if (g_state == ST_IDLE) {
      startTailPattern();
      g_state = ST_TEST;
      Serial.println(F("Tone pattern played (no TX)"));
    } else {
      Serial.println(F("Ignored: busy transmitting"));
    }
  }
  else if (!strcmp(cmd, "save")) {
    saveSettings();
    Serial.println(F("Settings saved to EEPROM"));
  }
  else if (!strcmp(cmd, "load")) {
    if (loadSettings()) { Serial.println(F("Settings loaded from EEPROM")); printStatus(); }
    else               { Serial.println(F("No valid settings found")); }
  }
  else if (!strcmp(cmd, "defaults")) {
    applyDefaults();
    printStatus();
    Serial.println(F("Default values restored (not saved yet)"));
  }
  else {
    Serial.print(F("Unknown command: ")); Serial.println(cmd);
    Serial.println(F("Type 'help' for the command list"));
  }
}

void handleSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (g_line.length() > 0) {
        processCommand(g_line);
        g_line = "";
      }
    } else if (g_line.length() < 48) {
      g_line += c;
    }
  }
}

/* ===================== 9. SETUP / LOOP ============================== */

void setup() {
  pinMode(PIN_PTT_IN,  INPUT_PULLUP);
  pinMode(PIN_TEST,   INPUT_PULLUP);
  pinMode(PIN_PTT_OUT, OUTPUT);
  pinMode(PIN_TONE,   OUTPUT);
  pinMode(PIN_LED,    OUTPUT);

  digitalWrite(PIN_PTT_OUT, LOW);
  digitalWrite(PIN_TONE,   LOW);
  digitalWrite(PIN_LED,    LOW);

  /* take the first PTT reading right away, so a button that is already
     held down at startup is recognised immediately */
  g_rawPtt    = digitalRead(PIN_PTT_IN);
  g_stablePtt = g_rawPtt;
  g_rawChange = millis();

  Serial.begin(115200);
  Serial.print(F("Rog-beep controller (")); Serial.print(F(BOARD_NAME));
  Serial.println(F(") ready. Type 'help' for commands."));

  if (!loadSettings()) {
    applyDefaults();
    Serial.println(F("Using default settings"));
  }
  printStatus();
}

void loop() {
  handleSerial();

  unsigned long now = millis();
  handleButton(now);                 /* short/long press on the button   */
  bool ptt = readPtt();

  switch (g_state) {

    case ST_IDLE:
      setPtt(false);
      if (ptt) {
        setPtt(true);
        g_state = ST_TX;
      }
      break;

    case ST_TX:
      setPtt(true);
      if (!ptt) {
        /* PTT released -> start the tail (lead + tones + exit) */
        startTailPattern();
        g_state = ST_TAIL;
        if (g_patternLen == 0UL) {      /* total tail = 0 -> release at once */
          setTone(0);
          setPtt(false);
          g_state = ST_IDLE;
        }
      }
      break;

    case ST_TAIL:
      setPtt(true);
      if (ptt) {
        /* pressed again: just keep transmitting, abort the tone */
        setTone(0);
        g_state = ST_TX;
      } else {
        updatePattern(now - g_patternStart);
        if ((now - g_patternStart) >= g_patternLen) {
          setTone(0);
          setPtt(false);
          g_state = ST_IDLE;
        }
      }
      break;

    case ST_TEST:
      setPtt(false);
      if (ptt) {
        /* PTT during the test: abort the test and start transmitting */
        setTone(0);
        setPtt(true);
        g_state = ST_TX;
        break;
      }
      updatePattern(now - g_patternStart);
      if ((now - g_patternStart) >= g_patternLen) {
        setTone(0);
        g_state = ST_IDLE;
      }
      break;

    case ST_LOOP:
      /* chime mode: the PTT stays on and the tone pattern repeats
         endlessly. The microphone PTT is ignored in this mode. */
      setPtt(true);
      if (g_patternLen > 0UL) {
        while ((now - g_patternStart) >= g_patternLen) {
          g_patternStart += g_patternLen;      /* roll into the next cycle */
        }
        updatePattern(now - g_patternStart);
      } else {
        setTone(0);
      }
      break;
  }
}
