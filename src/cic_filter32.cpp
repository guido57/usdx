#include "cic_filter32.h"
#include <algorithm> // for std::clamp
#include <limits.h>  // for INT32_MIN, INT32_MAX

CicFilter32::CicFilter32() {
    reset();
}

void CicFilter32::reset() {
    i_int1 = i_int2 = i_int3 = 0;
    q_int1 = q_int2 = q_int3 = 0;

    i_c1_z = i_c2_z = i_c3_z = 0;
    q_c1_z = q_c2_z = q_c3_z = 0;

    decim = 0;
    i_output = 0;
    q_output = 0;
}

// simple clamp for int64_t → int32_t
inline int32_t clamp32(int64_t x) {
    if (x > INT32_MAX) return INT32_MAX;
    if (x < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(x);
}

bool CicFilter32::processSample(int32_t iSample, int32_t qSample) {
    // --- Integrator stages (high precision) ---
    i_int1 += iSample;
    i_int2 += i_int1;
    i_int3 += i_int2;

    q_int1 += qSample;
    q_int2 += q_int1;
    q_int3 += q_int2;

    // Decimation counter
    decim++;
    if (decim < R) return false; // not yet decimated
    decim = 0;

    // --- Comb stages ---
    int64_t i_comb = i_int3 - i_c1_z;
    i_c1_z = i_int3;

    i_comb = i_comb - i_c2_z;
    i_c2_z = i_comb + i_c2_z;

    i_comb = i_comb - i_c3_z;
    i_c3_z = i_comb + i_c3_z;

    int64_t q_comb = q_int3 - q_c1_z;
    q_c1_z = q_int3;

    q_comb = q_comb - q_c2_z;
    q_c2_z = q_comb + q_c2_z;

    q_comb = q_comb - q_c3_z;
    q_c3_z = q_comb + q_c3_z;

    // --- Output with saturation to 32-bit ---
    i_output = clamp32(i_comb);
    q_output = clamp32(q_comb);

    return true;
}

int32_t CicFilter32::getOutputI() const { return i_output; }
int32_t CicFilter32::getOutputQ() const { return q_output; }