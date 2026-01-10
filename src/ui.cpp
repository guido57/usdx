#include "ui.h"

#include <Arduino.h>
#include <Wire.h>

#include "configuration.h"
#include "display.h"
#include "encoder.h"
#include "phy/si5351.h"

// --- Core UI state (trimmed from main.ori but behaviorally similar) ---
enum Mode { LSB = 0, USB, CW, FM, AM, N_MODES };
static const char* kModeLabel[] = { "LSB", "USB", "CW", "FM", "AM" };

enum Step { STEP_10M, STEP_1M, STEP_500k, STEP_100k, STEP_10k, STEP_1k, STEP_500, STEP_100, STEP_10, STEP_1 };
static const uint32_t kStepValues[] = { 10000000UL, 1000000UL, 500000UL, 100000UL, 10000UL, 1000UL, 500UL, 100UL, 10UL, 1UL };
static const char* kStepLabel[] = { "10M", "1M", "500k", "100k", "10k", "1k", "500", "100", "10", "1" };

static volatile uint8_t stepsize = STEP_1k;
static int32_t vfo[2] = { 7100000, 7200000 };
static uint8_t vfoSel = 0;  // 0=A, 1=B
static Mode vfomode[2] = { LSB, LSB };

static const uint8_t N_BANDS = 11;
static const int32_t bands[N_BANDS] = {
  1840000, 3573000, 5357000, 7074000, 10136000, 14074000, 18100000, 21074000, 24915000, 28074000, 50313000
};
static int32_t bandval = 3;  // default 40m FT8-ish like main.ori

static int16_t rit = 0;
static bool ritActive = false;
static Mode mode = LSB;
static int8_t volume = 5;
static int8_t filt = 0;
// Additional menu settings (placeholders in this ESP32 UI harness)
static int8_t agc = 1;
static int8_t nr = 0;
static int8_t att = 0;
static int8_t smode = 1;
static int8_t cw_tone = 2;
static int16_t cw_offset = 700;
static int8_t vox = 0;
static int8_t vox_gain = 50;
static int8_t drive = 4;
static int16_t txdelay = 0;
static int8_t mox = 0;
static int8_t backlight = 1;
static int32_t sifxtal = (int32_t)F_XTAL;

static bool change = true;  // force initial draw

// --- Getters used by main.cpp to program SI5351 (main.ori-style) ---
const char* ui_mode_label(UiMode m) {
  switch (m) {
    case UI_LSB: return "LSB";
    case UI_USB: return "USB";
    case UI_CW:  return "CW";
    case UI_FM:  return "FM";
    case UI_AM:  return "AM";
    default:     return "?";
  }
}

uint8_t ui_mode_to_si5351_rx_mode(UiMode m) {
  switch (m) {
    case UI_LSB: return SI5351_RX_MODE_LSB;
    case UI_USB: return SI5351_RX_MODE_USB;
    case UI_CW:  return SI5351_RX_MODE_CW;
    case UI_FM:  return SI5351_RX_MODE_FM;
    case UI_AM:  return SI5351_RX_MODE_AM;
    default:     return SI5351_RX_MODE_LSB;
  }
}

UiMode ui_get_mode() { return static_cast<UiMode>(mode); }
int32_t ui_get_vfo_freq() { return vfo[vfoSel]; }
uint8_t ui_get_vfo_sel() { return vfoSel; }
bool ui_get_rit_active() { return ritActive; }
int16_t ui_get_rit() { return rit; }
int16_t ui_get_cw_offset() { return cw_offset; }
int32_t ui_get_sifxtal() { return sifxtal; }
int8_t ui_get_att() { return att; }

int8_t ui_get_filter() { return filt; }
int8_t ui_get_cw_tone() { return cw_tone; }
int8_t ui_get_volume() { return volume; }

// Menu handling
static volatile uint8_t menumode = 0;  // 0=home, 1=menu select, 2=edit
static volatile int8_t menu = 0;

// Input devices
static Display lcd(I2C_SDA, I2C_SCL, 0x3C);
static Encoder encoder;

// Button tracking for gestures
struct ButtonTracker {
  bool down = false;
  uint32_t pressedAt = 0;
  uint32_t lastRelease = 0;
  uint8_t clickCount = 0;
  bool rotatedWhileHeld = false;
};
static ButtonTracker btn[3];

// Event encoding, matching main.ori intent
enum EventFlags { SC = 0x01, DC = 0x02, PL = 0x04, PLC = 0x05, PT = 0x0C };
static const uint8_t BTN_L_CODE = 0x10;  // left
static const uint8_t BTN_R_CODE = 0x20;  // right
static const uint8_t BTN_E_CODE = 0x30;  // encoder press

static volatile uint8_t pendingEvent = 0;
static volatile bool eventAvailable = false;

// --- Helpers ---
static int32_t currentFreq() { return vfo[vfoSel]; }
static void setCurrentFreq(int32_t f) { vfo[vfoSel] = constrain(f, 1, 999999999); }

static String formatFrequency(int32_t f) {
  // Format integer Hz using '.' as thousands separator, e.g. 7.100.000
  bool negative = f < 0;
  uint32_t v = negative ? static_cast<uint32_t>(-f) : static_cast<uint32_t>(f);

  char digits[12];
  ultoa(v, digits, 10);
  const size_t len = strlen(digits);

  char out[16];
  size_t o = 0;
  if (negative) out[o++] = '-';

  size_t first = len % 3;
  if (first == 0) first = 3;

  for (size_t i = 0; i < len; ++i) {
    if (i == first || (i > first && ((i - first) % 3) == 0)) {
      out[o++] = '.';
    }
    out[o++] = digits[i];
  }
  out[o] = 0;
  return String(out);
}

// --- Parameter model (expanded main-menu inspired by main.ori) ---
enum ParamType { P_INT, P_BOOL, P_ENUM, P_FREQ, P_BAND };
struct Param {
  const char* label;
  int32_t* value;
  int32_t min;
  int32_t max;
  bool wrap;
  ParamType type;
  const char* const* labels;
  uint8_t labelsCount;
};

static int32_t param_mode = mode;
static int32_t param_step = stepsize;
static int32_t param_band = bandval;
static int32_t param_vfo_sel = vfoSel;
static int32_t param_freqA = vfo[0];
static int32_t param_freqB = vfo[1];
static int32_t param_rit = rit;
static int32_t param_rit_en = ritActive ? 1 : 0;
static int32_t param_volume = volume;
static int32_t param_filter = filt;

static int32_t param_agc = agc;
static int32_t param_nr = nr;
static int32_t param_att = att;
static int32_t param_smode = smode;
static int32_t param_cw_tone = cw_tone;
static int32_t param_cw_offset = cw_offset;
static int32_t param_vox = vox;
static int32_t param_vox_gain = vox_gain;
static int32_t param_drive = drive;
static int32_t param_txdelay = txdelay;
static int32_t param_mox = mox;
static int32_t param_backlight = backlight;
static int32_t param_sifxtal = sifxtal;

static const char* const kOnOffLabel[] = { "OFF", "ON" };
static const char* const kSmodeLabel[] = { "OFF", "dBm", "S", "BAR", "WPM", "VSS", "CLK" };

static Param params[] = {
  { "Mode",      &param_mode,      0, N_MODES - 1, true,  P_ENUM, kModeLabel, (uint8_t)N_MODES },
  { "Step",      &param_step,      STEP_1M, STEP_1, false, P_ENUM, kStepLabel, (uint8_t)(sizeof(kStepLabel) / sizeof(kStepLabel[0])) },
  { "Band",      &param_band,      0, N_BANDS - 1, true,  P_BAND, nullptr, 0 },
  { "VFO Sel",   &param_vfo_sel,   0, 1, true,            P_INT,  nullptr, 0 },
  { "Freq A",    &param_freqA,     1, 999999999, false,   P_FREQ, nullptr, 0 },
  { "Freq B",    &param_freqB,     1, 999999999, false,   P_FREQ, nullptr, 0 },
  { "RIT",       &param_rit,       -9999, 9999, false,    P_INT,  nullptr, 0 },
  { "RIT EN",    &param_rit_en,    0, 1, true,            P_BOOL, kOnOffLabel, 2 },

  { "Volume",    &param_volume,    0, 10, false,          P_INT,  nullptr, 0 },
  { "Filter",    &param_filter,    0, 7, true,            P_INT,  nullptr, 0 },

  { "AGC",       &param_agc,       0, 1, true,            P_BOOL, kOnOffLabel, 2 },
  { "NR",        &param_nr,        0, 1, true,            P_BOOL, kOnOffLabel, 2 },
  { "ATT",       &param_att,       0, 2, true,            P_INT,  nullptr, 0 },
  { "S-Meter",   &param_smode,     0, 6, true,            P_ENUM, kSmodeLabel, (uint8_t)(sizeof(kSmodeLabel) / sizeof(kSmodeLabel[0])) },

  { "CW Tone",   &param_cw_tone,   0, 5, true,            P_INT,  nullptr, 0 },
  { "CW Off",    &param_cw_offset, 300, 1200, false,      P_INT,  nullptr, 0 },

  { "VOX",       &param_vox,       0, 1, true,            P_BOOL, kOnOffLabel, 2 },
  { "VOX Gain",  &param_vox_gain,  0, 100, false,         P_INT,  nullptr, 0 },

  { "Drive",     &param_drive,     0, 10, false,          P_INT,  nullptr, 0 },
  { "TX Delay",  &param_txdelay,   0, 500, false,         P_INT,  nullptr, 0 },
  { "MOX",       &param_mox,       0, 1, true,            P_BOOL, kOnOffLabel, 2 },
  { "Backlight", &param_backlight, 0, 1, true,            P_BOOL, kOnOffLabel, 2 },
  { "SI Fxtal",  &param_sifxtal,   10000000, 40000000, false,    P_INT,  nullptr, 0 },
};

static const uint8_t N_PARAMS = sizeof(params) / sizeof(params[0]);

static String formatParam(const Param& p) {
  switch (p.type) {
    case P_ENUM:
      if (p.labels && *p.value >= 0 && *p.value < p.labelsCount) return String(p.labels[*p.value]);
      return String(*p.value);
    case P_BAND: {
      int32_t idx = *p.value;
      if (idx < 0) idx = 0;
      if (idx >= N_BANDS) idx = N_BANDS - 1;
      return formatFrequency(bands[idx]);
    }
    case P_FREQ:
      return formatFrequency(*p.value);
    case P_BOOL:
      return (*p.value) ? F("ON") : F("OFF");
    default:
      return String(*p.value);
  }
}

static void syncParamsToState() {
  param_mode = mode;
  param_step = stepsize;
  param_band = bandval;
  param_vfo_sel = vfoSel;
  param_freqA = vfo[0];
  param_freqB = vfo[1];
  param_rit = rit;
  param_rit_en = ritActive ? 1 : 0;
  param_volume = volume;
  param_filter = filt;
  param_agc = agc;
  param_nr = nr;
  param_att = att;
  param_smode = smode;
  param_cw_tone = cw_tone;
  param_cw_offset = cw_offset;
  param_vox = vox;
  param_vox_gain = vox_gain;
  param_drive = drive;
  param_txdelay = txdelay;
  param_mox = mox;
  param_backlight = backlight;
  param_sifxtal = sifxtal;
}

static void applyParamToState() {
  const uint8_t prevVfoSel = vfoSel;
  const int32_t prevBand = bandval;

  stepsize = static_cast<uint8_t>(param_step);
  vfoSel = static_cast<uint8_t>(param_vfo_sel);
  vfo[0] = constrain(param_freqA, 1, 999999999);
  vfo[1] = constrain(param_freqB, 1, 999999999);
  rit = static_cast<int16_t>(constrain(param_rit, -9999, 9999));
  ritActive = (param_rit_en != 0);
  volume = static_cast<int8_t>(constrain(param_volume, 0, 10));
  filt = static_cast<int8_t>(constrain(param_filter, 0, 7));

  agc = static_cast<int8_t>(constrain(param_agc, 0, 1));
  nr = static_cast<int8_t>(constrain(param_nr, 0, 1));
  att = static_cast<int8_t>(constrain(param_att, 0, 2));
  smode = static_cast<int8_t>(constrain(param_smode, 0, 6));
  cw_tone = static_cast<int8_t>(constrain(param_cw_tone, 0, 5));
  cw_offset = static_cast<int16_t>(constrain(param_cw_offset, 300, 1200));
  vox = static_cast<int8_t>(constrain(param_vox, 0, 1));
  vox_gain = static_cast<int8_t>(constrain(param_vox_gain, 0, 100));
  drive = static_cast<int8_t>(constrain(param_drive, 0, 10));
  txdelay = static_cast<int16_t>(constrain(param_txdelay, 0, 500));
  mox = static_cast<int8_t>(constrain(param_mox, 0, 1));
  backlight = static_cast<int8_t>(constrain(param_backlight, 0, 1));
  sifxtal = constrain(param_sifxtal, 10000000, 40000000);

  // Mode is stored per VFO like main.ori
  mode = static_cast<Mode>(param_mode);
  vfomode[vfoSel] = mode;

  // If VFO selection changed, restore that VFO's mode.
  if (vfoSel != prevVfoSel) {
    mode = vfomode[vfoSel];
    param_mode = mode;
  }

  // Band selection applies to current VFO frequency.
  bandval = constrain(param_band, 0, (int32_t)N_BANDS - 1);
  if (bandval != prevBand) {
    setCurrentFreq(bands[bandval]);
    // keep menu in sync
    if (vfoSel == 0) param_freqA = vfo[0];
    else param_freqB = vfo[1];
  }
}

static void adjustParam(int8_t delta) {
  Param& p = params[menu];
  int32_t val = *p.value;

  if (p.type == P_FREQ) {
    val += (int32_t)delta * (int32_t)kStepValues[stepsize];
  } else {
    val += delta;
  }

  if (p.wrap) {
    if (val > p.max) val = p.min;
    if (val < p.min) val = p.max;
  } else {
    val = constrain(val, p.min, p.max);
  }
  *p.value = val;
  applyParamToState();
  change = true;
}

// --- Display ---
static void show_banner() {
  lcd.clear();
  lcd.setTextSize(1);
  lcd.setTextColor(SSD1306_WHITE);
  lcd.setCursor(0, 0);
  lcd.raw().print(F("uSDX "));
  lcd.raw().print(vfoSel ? 'B' : 'A');
  lcd.raw().print(F("  "));
  lcd.raw().print(kModeLabel[mode]);
  if (ritActive) lcd.raw().print(F(" RIT"));
}

static void renderHome() {
  show_banner();

  lcd.setCursor(0, 16);
  lcd.raw().print(formatFrequency(currentFreq() + (ritActive ? rit : 0)));
  lcd.raw().print(F(" Hz"));

  lcd.setCursor(0, 24);
  lcd.raw().print(F("Base "));
  lcd.raw().print(formatFrequency(currentFreq()));

  lcd.setCursor(0, 32);
  lcd.raw().print(F("RIT "));
  lcd.raw().print(ritActive ? rit : 0);

  lcd.setCursor(0, 40);
  lcd.raw().print(F("Step "));
  lcd.raw().print(kStepLabel[stepsize]);

  lcd.setCursor(0, 48);
  lcd.raw().print(F("Vol  "));
  lcd.raw().print(volume);

  lcd.flush();
}

static void renderMenu() {
  syncParamsToState();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.raw().print(F("Menu "));
  lcd.raw().print(menu + 1);
  lcd.raw().print(F("/"));
  lcd.raw().print(N_PARAMS);
  lcd.raw().print(F("  "));
  lcd.raw().print(params[menu].label);

  lcd.setCursor(0, 16);
  lcd.raw().print(F("Value: "));
  lcd.raw().print(formatParam(params[menu]));
  if (menumode == 2) lcd.raw().print(F("  <edit>"));
  lcd.flush();
}

// --- Tuning / steps ---
static void process_encoder_tuning_step(int8_t steps) {
  int32_t stepval = kStepValues[stepsize];
  if (ritActive) {
    rit += steps * stepval;
    rit = constrain(rit, -9999, 9999);
  } else {
    setCurrentFreq(currentFreq() + steps * stepval);
  }
  change = true;
}

static void stepsize_change(int8_t val) {
  int16_t next = static_cast<int16_t>(stepsize) + val;
  // Cycle through all defined step sizes in the UI range, including 10k and 1.
  // Keep 10M and 500k out of the encoder cycle to match the menu's editable range.
  if (next < STEP_1M) next = STEP_1;
  if (next > STEP_1) next = STEP_1M;
  stepsize = static_cast<uint8_t>(next);
  change = true;
}

// --- Button/encoder event handling ---
static uint8_t encodeEvent(uint8_t buttonNibble, uint8_t action) {
  return buttonNibble | action;
}

static void queueEvent(uint8_t ev) {
  pendingEvent = ev;
  eventAvailable = true;
}

static void handleRotate(int8_t direction) {
  // Note if a button is held, mark it for push-turn detection
  for (int i = 0; i < 3; ++i) {
    if (btn[i].down) btn[i].rotatedWhileHeld = true;
  }

  if (menumode == 0) {
    process_encoder_tuning_step(direction);
  } else if (menumode == 1) {
    menu += direction;
    if (menu < 0) menu = N_PARAMS - 1;
    if (menu >= N_PARAMS) menu = 0;
    change = true;
  } else if (menumode == 2) {
    adjustParam(direction);
  }
}

static void handleButton(uint8_t id, bool pressed) {
  const uint32_t LONG_PRESS_MS = 600;
  const uint32_t DOUBLE_MS = 350;
  ButtonTracker& b = btn[id];

  if (pressed) {
    b.down = true;
    b.pressedAt = millis();
    b.rotatedWhileHeld = false;
    return;
  }

  // release
  if (!b.down) return;
  b.down = false;
  uint32_t now = millis();
  uint32_t duration = now - b.pressedAt;

  // push-turn wins
  if (b.rotatedWhileHeld) {
    queueEvent(encodeEvent((id == Encoder::BTN_LEFT) ? BTN_L_CODE : (id == Encoder::BTN_RIGHT) ? BTN_R_CODE : BTN_E_CODE, PT));
    b.rotatedWhileHeld = false;
    b.clickCount = 0;
    b.lastRelease = now;
    return;
  }

  if (duration >= LONG_PRESS_MS) {
    queueEvent(encodeEvent((id == Encoder::BTN_LEFT) ? BTN_L_CODE : (id == Encoder::BTN_RIGHT) ? BTN_R_CODE : BTN_E_CODE, PL));
    b.clickCount = 0;
    b.lastRelease = now;
    return;
  }

  // short press / double detection
  if (now - b.lastRelease <= DOUBLE_MS && b.clickCount == 1) {
    queueEvent(encodeEvent((id == Encoder::BTN_LEFT) ? BTN_L_CODE : (id == Encoder::BTN_RIGHT) ? BTN_R_CODE : BTN_E_CODE, DC));
    b.clickCount = 0;
  } else {
    b.clickCount = 1;
    b.lastRelease = now;
    queueEvent(encodeEvent((id == Encoder::BTN_LEFT) ? BTN_L_CODE : (id == Encoder::BTN_RIGHT) ? BTN_R_CODE : BTN_E_CODE, SC));
  }
}

static void processEvent(uint8_t event) {
  switch (event) {
    // Left button events
    case BTN_L_CODE | PL:    // long press: jump into edit mode
    case BTN_L_CODE | PLC:
      menumode = 2; change = true; break;
    case BTN_L_CODE | PT:    // push-turn: go to menu list
      menumode = 1; change = true; break;
    case BTN_L_CODE | SC: {  // short click: cycle menu states
      int8_t next = menumode;
      if (menumode == 0) { next = 1; if (menu < 0) menu = 0; }
      else if (menumode == 1) next = 2;
      else next = 0;
      menumode = next; change = true;
      break;
    }
    case BTN_L_CODE | DC:
      // no-op reserved
      break;

    // Right button events
    case BTN_R_CODE | SC:  // mode cycle or exit edit
      if (!menumode) {
        mode = static_cast<Mode>((mode + 1) % N_MODES);
        vfomode[vfoSel] = mode;
        param_mode = mode;
      } else if (menumode == 1) {
        menumode = 0;
      } else {
        menumode = 1;
      }
      change = true;
      break;
    case BTN_R_CODE | DC:  // toggle RIT enable
      ritActive = !ritActive;
      if (!ritActive) rit = 0;
      param_rit_en = ritActive ? 1 : 0;
      param_rit = rit;
      change = true;
      break;
    case BTN_R_CODE | PL:  // long press: toggle VFO or RIT per main.ori spirit
      vfoSel ^= 0x1;
      mode = vfomode[vfoSel];
      param_vfo_sel = vfoSel;
      param_mode = mode;
      change = true;
      break;
    case BTN_R_CODE | PT:  // push-turn: quick stepsize down
      stepsize_change(-1); break;

    // Encoder button events
    case BTN_E_CODE | SC:  // short: stepsize up or toggle menu selection
      if (!menumode) stepsize_change(+1); else menumode = (menumode == 1) ? 2 : 1; change = true; break;
    case BTN_E_CODE | DC: { // double: cycle through a small band list
      bandval = (bandval + 1) % N_BANDS;
      param_band = bandval;
      setCurrentFreq(bands[bandval]);
      if (vfoSel == 0) param_freqA = vfo[0];
      else param_freqB = vfo[1];
      change = true;
      break;
    }
    case BTN_E_CODE | PL:  // long: stepsize down
      stepsize_change(-1); break;
    case BTN_E_CODE | PT:  // push-turn: adjust volume up one notch
      volume = constrain(volume + 1, 0, 10); param_volume = volume; change = true; break;
  }
}

void ui_setup() {
  
  // I2C is initialized in main.cpp (shared bus for OLED + SI5351)

  if (!lcd.begin()) {
    Serial.println("Display init failed");
    while (true) { delay(1000); }
  }else {
    Serial.println("Display initialized");
  }

  encoder.begin(GPIO_ROT_A, GPIO_ROT_B, GPIO_ROT_SW, GPIO_L_SW, GPIO_R_SW);
  encoder.onRotate(handleRotate);
  encoder.onButton(handleButton);

  renderHome();
}

void ui_loop() {
  encoder.update();

  if (eventAvailable) {
    uint8_t ev = pendingEvent;
    eventAvailable = false;
    processEvent(ev);
  }

  if (change) {
    change = false;
    if (menumode == 0) renderHome(); else renderMenu();
  }

  delay(1);  // Small delay to avoid tight loop, matches working encoder test
}
