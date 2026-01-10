// CIC (Cascaded Integrator-Comb) decimating filter
// Ported from main.ori (lines 3044-3061)
// 3rd-order CIC with decimation by 8 (output once per 8-phase cycle)

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

// CIC filter implementation matching main.ori state machine
// Processes 8 samples (4 I, 4 Q alternating) and outputs 1 I/Q pair
bool CicFilter::processSample(int16_t iSample, int16_t qSample) {
  bool outputReady = false;
  
  // Apply input interpolation/smoothing like main.ori (sdr_rx_common_i/q)
  int16_t i_smooth = (i_prev + iSample) / 2;
  int16_t q_smooth = (q_prev + qSample) / 2;
  i_prev = iSample;
  q_prev = qSample;
  
  switch (phase) {
    case 0: { // sdr_rx_00: Process I, output result
      int16_t i_s1za0 = (i_smooth + (i_s0za1 + i_s0zb0) * 3 + i_s0zb1) >> M_SR;
      i_s0za1 = i_smooth;
      i_output = (i_s1za0 + (i_s1za1 + i_s1zb0) * 3 + i_s1zb1);
      i_s1za1 = i_s1za0;
      outputReady = true;
      break;
    }
    
    case 1: { // sdr_rx_01: Process Q
      q_s0zb1 = q_s0zb0;
      q_s0zb0 = q_smooth;
      break;
    }
    
    case 2: { // sdr_rx_02: Process I
      i_s0zb1 = i_s0zb0;
      i_s0zb0 = i_smooth;
      break;
    }
    
    case 3: { // sdr_rx_03: Process Q
      q_s1zb1 = q_s1zb0;
      q_s1zb0 = (q_smooth + (q_s0za1 + q_s0zb0) * 3 + q_s0zb1) >> M_SR;
      q_s0za1 = q_smooth;
      break;
    }
    
    case 4: { // sdr_rx_04: Process I
      i_s1zb1 = i_s1zb0;
      i_s1zb0 = (i_smooth + (i_s0za1 + i_s0zb0) * 3 + i_s0zb1) >> M_SR;
      i_s0za1 = i_smooth;
      break;
    }
    
    case 5: { // sdr_rx_05: Process Q
      q_s0zb1 = q_s0zb0;
      q_s0zb0 = q_smooth;
      break;
    }
    
    case 6: { // sdr_rx_06: Process I
      i_s0zb1 = i_s0zb0;
      i_s0zb0 = i_smooth;
      break;
    }
    
    case 7: { // sdr_rx_07: Process Q, output result
      int16_t q_s1za0 = (q_smooth + (q_s0za1 + q_s0zb0) * 3 + q_s0zb1) >> M_SR;
      q_s0za1 = q_smooth;
      q_output = (q_s1za0 + (q_s1za1 + q_s1zb0) * 3 + q_s1zb1);
      q_s1za1 = q_s1za0;
      break;
    }
  }
  
  phase = (phase + 1) & 0x07; // Wrap 0-7
  
  return outputReady;
}
