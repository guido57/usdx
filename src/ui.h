#pragma once

#include <stdint.h>
#include <stddef.h>

// UI/menu handling extracted from main.cpp

void ui_setup();
void ui_loop();
void ui_load_settings();
void ui_set_waterfall_line(const uint8_t* bins, size_t count);

// Persistent UI settings snapshot
struct UiSettings {
	uint32_t magic;
	uint8_t version;
	uint8_t stepsize;
	uint8_t vfoSel;
	uint8_t mode;
	uint8_t vfomode0;
	uint8_t vfomode1;
	int32_t vfoA;
	int32_t vfoB;
	int32_t ft8_offset;
	char ft8_testmsg[64];
	int32_t bandval;
	int16_t rit;
	uint8_t ritActive;
	int8_t volume;
	int8_t filt;
	int8_t agc;
	int8_t nr;
	int8_t att;
	int8_t att_rf;
	int8_t tx_bias;
	int8_t smode;
	int8_t cw_tone;
	int16_t cw_offset;
	int8_t vox;
	int8_t vox_gain;
	int8_t drive;
	int16_t txdelay;
	int8_t mox;
	int8_t backlight;
	int32_t sifxtal;
	int16_t iq_phase;
	int16_t iq_balance;
	int16_t iq_delay;
	int8_t wf_thresh;
};

bool ui_get_settings(UiSettings* out);
void ui_apply_settings(const UiSettings& s);

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
int8_t ui_get_att_rf();
int8_t ui_get_tx_bias();

// Audio-related UI settings
int8_t ui_get_filter();
int8_t ui_get_cw_tone();
int8_t ui_get_volume();
