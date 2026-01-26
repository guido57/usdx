// Hilbert transform and SSB/AM/FM demodulator
// Ported from main.ori (lines 2975-3000)

#include "demodulator.h"
#include "configuration.h"
#include <Arduino.h>

// Hilbert transform state for Q channel (14 taps)
static int16_t v_q[14];

// Delay line for I channel to match Hilbert latency (7 taps)
static int16_t v_i[7];

// State for AM DC decoupling
static int32_t am_dc = 0;

// State for FM demodulator
static int16_t fm_zi = 0;

void demod_init() {
  // Clear all filter state
  for (int i = 0; i < 14; i++) v_q[i] = 0;
  for (int i = 0; i < 7; i++) v_i[i] = 0;
  am_dc = 0;
  fm_zi = 0;
}

// Simple magnitude approximation from main.ori, 32-bit safe
static inline int32_t magn32(int16_t i, int16_t q) {
  int32_t ii = abs(i);
  int32_t qq = abs(q);
  if (ii > qq) {
    return ii + (qq >> 1) - (ii >> 4);
  } else {
    return qq + (ii >> 1) - (qq >> 4);
  }
}

int16_t demod_process(int16_t i_sample, int16_t q_sample, DemodMode mode) {
  int16_t ac;
  
  // Apply Hilbert transform on Q channel
  // Hilbert transform, 43dB side-band rejection in 650..3400Hz (@8kSPS)
  // BasicDSP model: outi= fir(inl,  0, 0, 0, 0, 0,  0,  0, 1,   0, 0,   0, 0,  0, 0, 0, 0); 
  //                 outq = fir(inr, 2, 0, 8, 0, 21, 0, 79, 0, -79, 0, -21, 0, -8, 0, -2, 0) / 128;
  int16_t qh = ((v_q[0] - q_sample) + (v_q[2] - v_q[12]) * 4) / 64 + 
               ((v_q[4] - v_q[10]) + (v_q[6] - v_q[8])) / 8 + 
               ((v_q[4] - v_q[10]) * 5 - (v_q[6] - v_q[8])) / 128 + 
               (v_q[6] - v_q[8]) / 2;
  
  // Shift Q delay line
  for (int i = 0; i < 13; i++) {
    v_q[i] = v_q[i + 1];
  }
  v_q[13] = q_sample;
  
  // Delay I channel to match Hilbert transform latency
  int16_t i_delayed = v_i[0];
  for (int i = 0; i < 6; i++) {
    v_i[i] = v_i[i + 1];
  }
  v_i[6] = i_sample;
  
  // Store aligned I/Q samples
  // - SSB uses delayed I + Hilbert Q (qh)
  // - AM/FM should use current I/Q (no delay skew)
  int16_t i = i_sample;
  int16_t q = q_sample;
  
  // Demodulate based on mode
  switch (mode) {
    case DEMOD_LSB:
      // LSB: -i - qh (from main.ori line 2992)
      // Inverting I and Q helps dampen feedback-loop between PWM out and ADC inputs
      ac = -i_delayed - qh;
      break;
      
    case DEMOD_USB:
      // USB: i + qh (from main.ori line 3156)
      ac = i_delayed + qh;
      break;
      
    case DEMOD_AM:
      // AM envelope detection (from main.ori lines 2795-2800)
      {
      int32_t ac32;
    #if AM_DEMOD_COHERENT
      // Coherent AM: use I only (avoids 2x frequency on DSB-SC)
      ac32 = (int32_t)i;
    #else
      // Envelope AM (exact magnitude for lower distortion)
      const float ii = (float)i;
      const float qq = (float)q;
      ac32 = (int32_t)sqrtf(ii * ii + qq * qq);
    #endif
      // DC decoupling (slow to preserve audio)
      am_dc += (ac32 - am_dc) >> 8;
      ac32 = ac32 - am_dc;
      // AM gain boost to improve audio level
      ac32 = ac32 << 1; // 2x
        if (ac32 > INT16_MAX) ac32 = INT16_MAX;
        if (ac32 < INT16_MIN) ac32 = INT16_MIN;
        ac = (int16_t)ac32;
      }
      break;
      
    case DEMOD_FM:
      // FM quadrature detection (from main.ori lines 2801-2804)
      ac = ((ac + i) * fm_zi);  // Note: ac here comes from previous iteration
      fm_zi = i;
      break;
      
    default:
      ac = 0;
      break;
  }
  
  return ac;
}
