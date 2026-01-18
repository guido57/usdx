// CIC (Cascaded Integrator-Comb) decimating filter
// Ported from main.ori (lines 3044-3061)
// 3rd-order CIC with decimation by 4

#include "cic_filter.h"

#define M_SR 1  // CIC shift right amount (from main.ori)

CicFilter::CicFilter() {
  reset();
}

void CicFilter::reset() {
  i_s0za1 = i_s0zb0 = i_s0zb1 = 0;
  i_s1za1 = i_s1zb0 = i_s1zb1 = 0;
  q_s0za1 = q_s0zb0 = q_s0zb1 = 0;
  q_s1za1 = q_s1zb0 = q_s1zb1 = 0;
  i_prev = 0;
  q_prev = 0;
  phase = 0;
  i_output = 0;
  q_output = 0;
}

// CIC filter - 4 phases for decimation by 4
bool CicFilter::processSample(int16_t iSample, int16_t qSample) {
  bool outputReady = false;
  
  // Apply input interpolation/smoothing like main.ori (sdr_rx_common_i/q)
  int16_t i_smooth = (i_prev + iSample) / 2;
  int16_t q_smooth = (q_prev + qSample) / 2;
  i_prev = iSample;
  q_prev = qSample;
  
  switch (phase) {
    case 0: { // Update delay lines
      i_s0zb1 = i_s0zb0;
      i_s0zb0 = i_smooth;
      break;
    }
    
    case 1: { // Update delay lines
      q_s0zb1 = q_s0zb0;
      q_s0zb0 = q_smooth;
      break;
    }
    
    case 2: { // Compute I output
      int16_t i_s1za0 = (i_smooth + (i_s0za1 + i_s0zb0) * 3 + i_s0zb1) >> M_SR;
      i_s0za1 = i_smooth;
      i_output = (i_s1za0 + (i_s1za1 + i_s1zb0) * 3 + i_s1zb1);
      i_s1za1 = i_s1za0;
      break;
    }
    
    case 3: { // Compute Q output and return (both I and Q computed at same phase offset)
      int16_t q_s1za0 = (q_smooth + (q_s0za1 + q_s0zb0) * 3 + q_s0zb1) >> M_SR;
      q_s0za1 = q_smooth;
      q_output = (q_s1za0 + (q_s1za1 + q_s1zb0) * 3 + q_s1zb1);
      q_s1za1 = q_s1za0;
      outputReady = true;
      break;
    }
  }
  
  phase = (phase + 1) & 0x03; // Wrap 0-3
  
  return outputReady;
}
