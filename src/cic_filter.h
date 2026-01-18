#pragma once

#include <stdint.h>

// CIC (Cascaded Integrator-Comb) decimating filter
// Ported from main.ori (lines 3044-3061)
// 3rd-order CIC with decimation by 4 (4-phase cycle)

class CicFilter {
public:
  CicFilter();
  
  // Reset filter state
  void reset();
  
  // Process one I or Q sample. Returns true when decimated output is ready.
  // Call alternately for I and Q samples.
  // When returns true, read getOutputI() and getOutputQ()
  bool processSample(int16_t iSample, int16_t qSample);
  
  // Get decimated output (valid after processSample returns true)
  int16_t getOutputI() const { return i_output; }
  int16_t getOutputQ() const { return q_output; }

private:
  // CIC filter state for I channel
  int16_t i_s0za1, i_s0zb0, i_s0zb1;
  int16_t i_s1za1, i_s1zb0, i_s1zb1;
  
  // CIC filter state for Q channel
  int16_t q_s0za1, q_s0zb0, q_s0zb1;
  int16_t q_s1za1, q_s1zb0, q_s1zb1;
  
  // Input interpolation state (from main.ori sdr_rx_common_i/q)
  int16_t i_prev, q_prev;
  
  // State machine counter (0-3)
  uint8_t phase;
  
  // Output values
  int16_t i_output;
  int16_t q_output;
};
