// Audio filters ported from main.ori (IIR, 8kHz domain)
#include "audio_filter.h"

// State variables for SSB filters
static int16_t za1_ssb = 0, za2_ssb = 0;
static int16_t zb0_ssb = 0, zb1_ssb = 0, zb2_ssb = 0;
static int16_t zc0_ssb = 0, zc1_ssb = 0, zc2_ssb = 0;
static int16_t zz1_hp = 0, zz2_hp = 0; // 300 Hz high-pass prefilter state

// State variables for CW filters
static int16_t za1_cw = 0, za2_cw = 0;
static int16_t zb0_cw = 0, zb1_cw = 0, zb2_cw = 0;
static int16_t zc0_cw = 0, zc1_cw = 0, zc2_cw = 0;

int16_t audio_filter_apply(int16_t sample, int8_t filt, int8_t cw_tone, int8_t volume) {
  // Apply volume before filtering (volume range 0-10)
  sample = (int16_t)(sample * (int32_t)volume);
  
  if (filt <= 0) return sample; // no filtering

  // SSB/AM/FM style filters for filt 1..3
  if (filt < 4) {
    // 300 Hz high-pass (from main.ori)
    zz2_hp = zz1_hp;
    zz1_hp = sample;
    int16_t za0 = (int16_t)((30 * (int32_t)(sample - zz2_hp) + 25 * (int32_t)zz1_hp) / 32);

    // 4th order IIR, two biquad sections (original coefficients from main.ori)
    switch (filt) {
      case 1: // 0-2900 Hz first biquad
        zb0_ssb = (int16_t)(((za0 + 2 * (int32_t)za1_ssb + za2_ssb) / 2) - ((13 * (int32_t)zb1_ssb + 11 * (int32_t)zb2_ssb) / 16));
        break;
      case 2: // 0-2400 Hz first biquad
        zb0_ssb = (int16_t)(((za0 + 2 * (int32_t)za1_ssb + za2_ssb) / 2) - ((2 * (int32_t)zb1_ssb + 8 * (int32_t)zb2_ssb) / 16));
        break;
      case 3: // 0-1800 Hz elliptic first biquad
        zb0_ssb = (int16_t)(((za0 + 2 * (int32_t)za1_ssb + za2_ssb) / 2) - ((0 * (int32_t)zb1_ssb + 4 * (int32_t)zb2_ssb) / 16));
        break;
    }

    switch (filt) {
      case 1: // 0-2900 Hz second biquad
        zc0_ssb = (int16_t)(((zb0_ssb + 2 * (int32_t)zb1_ssb + zb2_ssb) / 2) - ((18 * (int32_t)zc1_ssb + 11 * (int32_t)zc2_ssb) / 16));
        break;
      case 2: // 0-2400 Hz second biquad
        zc0_ssb = (int16_t)(((zb0_ssb + 2 * (int32_t)zb1_ssb + zb2_ssb) / 4) - ((4 * (int32_t)zc1_ssb + 8 * (int32_t)zc2_ssb) / 16));
        break;
      case 3: // 0-1800 Hz elliptic second biquad
        zc0_ssb = (int16_t)(((zb0_ssb + 2 * (int32_t)zb1_ssb + zb2_ssb) / 4) - ((0 * (int32_t)zc1_ssb + 4 * (int32_t)zc2_ssb) / 16));
        break;
    }

    // Shift state
    zc2_ssb = zc1_ssb; zc1_ssb = zc0_ssb;
    zb2_ssb = zb1_ssb; zb1_ssb = zb0_ssb;
    za2_ssb = za1_ssb; za1_ssb = za0;

    return zc0_ssb;
  }

  // CW filters for filt >= 4, only when cw_tone == 0 per main.ori
  if (cw_tone == 0) {
    int16_t za0 = sample;
    switch (filt) {
      case 4: // 500-1000 Hz bandpass
        zb0_cw = (int16_t)(((za0 + 2 * (int32_t)za1_cw + za2_cw) / 2) + ((41L * (int32_t)zb1_cw - 23L * (int32_t)zb2_cw) / 32));
        break;
      case 5: // 650-840 Hz bandpass
        zb0_cw = (int16_t)(5 * (int32_t)(za0 - 2 * (int32_t)za1_cw + za2_cw) + ((105L * (int32_t)zb1_cw - 58L * (int32_t)zb2_cw) / 64));
        break;
      case 6: // 650-750 Hz bandpass
        zb0_cw = (int16_t)(3 * (int32_t)(za0 - 2 * (int32_t)za1_cw + za2_cw) + ((108L * (int32_t)zb1_cw - 61L * (int32_t)zb2_cw) / 64));
        break;
      default:
        return sample; // unsupported CW filter index
    }

    // Second biquad section mirrors main.ori for CW filters
    // Note: main.ori shows only first section explicitly for these cases; keep a simple single-section for now
    // Update states
    zc2_cw = zc1_cw; zc1_cw = zc0_cw; // unused in single-section version
    zb2_cw = zb1_cw; zb1_cw = zb0_cw;
    za2_cw = za1_cw; za1_cw = za0;

    return zb0_cw;
  }

  // If CW tone not matching narrow filters or unknown, pass through
  return sample;
}
