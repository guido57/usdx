#pragma once

#include <stdint.h>

// Apply audio filter based on UI-selected filter.
// filt: 0 = off, 1..3 = SSB low-pass (with 300 Hz high-pass), 4..6 = CW bandpass, 7 = SSB low-pass with 50 Hz high-pass
// cw_tone: CW tone setting (0 uses narrow band filters; non-zero passes through)
int16_t audio_filter_apply(int16_t sample, int8_t filt, int8_t cw_tone);
