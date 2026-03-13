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
  static DemodMode last_mode = (DemodMode)-1;
  if (mode != last_mode) {
    // Serial.printf("DEMOD_MODE CHANGE: %d\n", (int)mode);
    last_mode = mode;
  }
  
  int16_t ac = 0;

  // 1. Shift delay lines FIRST so we work with the most recent window of data
  for (int i = 0; i < 13; i++) v_q[i] = v_q[i + 1];
  v_q[13] = q_sample;

  int16_t i_delayed = v_i[0]; // This is the sample from 7 steps ago
  for (int i = 0; i < 6; i++) v_i[i] = v_i[i + 1];
  v_i[6] = i_sample;

  // 2. Calculate Hilbert Transform (90-degree shift)

 // Normalized Hilbert FIR for 8kHz
// Sum of coefficients approx 1.0 for passband center
float qh_f = (v_q[0]  - v_q[13]) * 0.045f +  // 2/(7*pi)
             (v_q[2]  - v_q[11]) * 0.091f +  // 2/(5*pi)
             (v_q[4]  - v_q[9])  * 0.212f +  // 2/(3*pi)
             (v_q[6]  - v_q[7])  * 0.636f;   // 2/(1*pi)

// Apply a final gain correction to hit Ratio 1.0
qh_f *= 1.0f; 

int16_t qh = (int16_t)qh_f;
  // Add these as statics at the top of demod_process
  static uint32_t debug_count = 0;
  static float i_sum_sq = 0, qh_sum_sq = 0;

  i_sum_sq += (float)i_delayed * i_delayed;
  qh_sum_sq += (float)qh * qh;
  debug_count++;

  if (debug_count >= 8000) { // Once per second at 8kHz
      float i_rms = sqrtf(i_sum_sq / 8000.0f);
      float qh_rms = sqrtf(qh_sum_sq / 8000.0f);
      // Serial.printf("[Demod Debug] I_RMS: %.1f | QH_RMS: %.1f | Ratio: %.2f\n", 
      //               i_rms, qh_rms, qh_rms/i_rms);
      i_sum_sq = 0; qh_sum_sq = 0; debug_count = 0;
  }
  // end of debug code

  // 3. Demodulate (SSB/AM/FM)
  switch (mode) {
    case DEMOD_LSB:
    case DEMOD_USB:
      // LSB/USB is the sum of delayed I and Hilbert Q
      // the switch between LSB/USB is done in the SI5351 
      ac = i_delayed + qh; 
      break;
      
    case DEMOD_AM:
      {
        int32_t ac32;
        float ii = (float)i_sample;
        float qq = (float)q_sample;
        ac32 = (int32_t)sqrtf(ii * ii + qq * qq);

        // DC decoupling
        am_dc += (ac32 - am_dc) >> 8;
        ac32 = ac32 - am_dc;

        // Increased AM gain (4x boost) to match SSB perceived volume
        ac32 = ac32 << 2; 
        
        if (ac32 > 32767)  ac32 = 32767;
        if (ac32 < -32768) ac32 = -32768;
        ac = (int16_t)ac32;
      }
      break;

    case DEMOD_FM:
      // Basic FM quadrature discriminator
      // ac = (I * dQ - Q * dI)
      ac = (int16_t)(((int32_t)i_sample * (q_sample - fm_zi)) >> 8);
      fm_zi = q_sample; 
      break;
      
    default:
      ac = 0;
      break;
  }
  
  return ac;
}