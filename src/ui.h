#pragma once

#include <stdint.h>
#include <stddef.h>

// UI/menu handling extracted from main.cpp

void ui_setup();
void ui_loop();
void ui_load_settings();
void ui_set_waterfall_line(const uint8_t* bins, size_t count);

// --- Radio state accessors for hardware programming (SI5351) ---
// Keep these minimal: main.cpp uses them to mirror main.ori "change -> si5351.freq()" logic.
enum UiMode : uint8_t { UI_LSB = 0, UI_USB = 1, UI_CW = 2, UI_AM = 3 };

// UI helpers used by main.cpp (logging + driver-mode mapping).
const char* ui_mode_label(UiMode mode);
uint8_t ui_mode_to_si5351_rx_mode(UiMode mode);

UiMode ui_get_mode();
int32_t ui_get_vfo_freq();
uint8_t ui_get_vfo_sel();

bool ui_get_rit_active();
int16_t ui_get_rit();

int16_t ui_get_cw_offset();
int16_t ui_get_iq_phase();
float ui_get_iq_balance();
float ui_get_iq_delay();
int8_t ui_get_agc();
int8_t ui_get_wf_thresh();

int32_t ui_get_sifxtal();

int8_t ui_get_att();

// Audio-related UI settings
int8_t ui_get_filter();
int8_t ui_get_cw_tone();
int8_t ui_get_volume();
