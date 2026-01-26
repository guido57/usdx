#pragma once

#include <stdint.h>

// CIC (Cascaded Integrator-Comb) decimating filter
// 3rd-order CIC with decimation by 4 (synchronous I/Q)

class CicFilter {
public:
  CicFilter();
  
  // Reset filter state
  void reset();
  
  // Process one I/Q pair. Returns true when decimated output is ready.
  // When returns true, read getOutputI() and getOutputQ().
  bool processSample(int16_t iSample, int16_t qSample);
  
  // Get decimated output (valid after processSample returns true)
  int32_t getOutputI() const { return i_output; }
  int32_t getOutputQ() const { return q_output; }

private:
  // CIC integrator state (32-bit)
  int32_t i_int1, i_int2, i_int3;
  int32_t q_int1, q_int2, q_int3;

  // CIC comb delay state (decimated rate)
  int32_t i_c1_z, i_c2_z, i_c3_z;
  int32_t q_c1_z, q_c2_z, q_c3_z;
  
  // Input interpolation state (from main.ori sdr_rx_common_i/q)
  int16_t i_prev, q_prev;
  
  // Decimation counter
  uint8_t decim;
  
  // Output values
  int32_t i_output;
  int32_t q_output;
};
