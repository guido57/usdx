
#pragma once

#include <stdint.h>

// 3rd-order CIC decimating filter (R=4), input = 32-bit, output = 32-bit
class CicFilter32 {
public:
    CicFilter32();

    // Reset filter state
    void reset();

    // Process one I/Q sample pair. Returns true if decimated output is ready
    bool processSample(int32_t iSample, int32_t qSample);

    // Get decimated output (valid after processSample returns true)
    int32_t getOutputI() const;
    int32_t getOutputQ() const;

private:
    static constexpr uint8_t R = 4; // Decimation factor

    // --- Integrator states (use 64-bit to prevent overflow) ---
    int64_t i_int1, i_int2, i_int3;
    int64_t q_int1, q_int2, q_int3;

    // --- Comb delay states (64-bit) ---
    int64_t i_c1_z, i_c2_z, i_c3_z;
    int64_t q_c1_z, q_c2_z, q_c3_z;

    uint8_t decim;       // decimation counter
    int32_t i_output;
    int32_t q_output;
};