// CIC (Cascaded Integrator-Comb) decimating filter
// 3rd-order CIC with decimation by 4 (synchronous I/Q)

#include "cic_filter.h"

static constexpr uint8_t CIC_DECIM = 4;

CicFilter::CicFilter() {
  reset();
}

void CicFilter::reset() {
  i_int1 = i_int2 = i_int3 = 0;
  q_int1 = q_int2 = q_int3 = 0;
  i_c1_z = i_c2_z = i_c3_z = 0;
  q_c1_z = q_c2_z = q_c3_z = 0;
  i_prev = 0;
  q_prev = 0;
  decim = 0;
  i_output = 0;
  q_output = 0;
}

// CIC filter - decimate by 4 on I/Q pairs
bool CicFilter::processSample(int16_t iSample, int16_t qSample) {
  bool outputReady = false;
  
  // Apply input interpolation/smoothing like main.ori (sdr_rx_common_i/q)
  int16_t i_smooth = (i_prev + iSample) / 2;
  int16_t q_smooth = (q_prev + qSample) / 2;
  i_prev = iSample;
  q_prev = qSample;

  // Integrators at full rate
  i_int1 += i_smooth;
  i_int2 += i_int1;
  i_int3 += i_int2;
  q_int1 += q_smooth;
  q_int2 += q_int1;
  q_int3 += q_int2;

  decim = (uint8_t)((decim + 1) & (CIC_DECIM - 1));
  if (decim == 0) {
    // Combs at decimated rate
    int32_t i_c1 = i_int3 - i_c1_z; i_c1_z = i_int3;
    int32_t i_c2 = i_c1 - i_c2_z;   i_c2_z = i_c1;
    int32_t i_c3 = i_c2 - i_c3_z;   i_c3_z = i_c2;

    int32_t q_c1 = q_int3 - q_c1_z; q_c1_z = q_int3;
    int32_t q_c2 = q_c1 - q_c2_z;   q_c2_z = q_c1;
    int32_t q_c3 = q_c2 - q_c3_z;   q_c3_z = q_c2;

    i_output = i_c3;
    q_output = q_c3;
    outputReady = true;
  }

  return outputReady;
}
