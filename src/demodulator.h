#pragma once

#include <stdint.h>

// Hilbert transform and SSB demodulator
// Ported from main.ori

enum DemodMode {
  DEMOD_LSB,
  DEMOD_USB,
  DEMOD_AM,
  DEMOD_FM
};

// Initialize demodulator
void demod_init();

// Process one I/Q sample pair (downsampled to ~8kHz rate)
// Returns audio sample for the selected mode
int16_t demod_process(int16_t i_sample, int16_t q_sample, DemodMode mode);
