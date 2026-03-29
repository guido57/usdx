// ft8_freq_opt.h
#pragma once
#include <stdint.h>

class FT8FreqOptimizer {
public:
    FT8FreqOptimizer();

    // Reset all internal maps
    void clear();

    // Store a JSON spot
    void store(const char *json);

    // Compute the best FT8 audio offset (Hz)
    // collision_prediction: avoid next-frame transmitters
    // cq_attractor: favor CQ-active zones
    uint16_t best_freq(uint32_t base_freq_hz, bool collision_prediction, bool cq_attractor);

    static constexpr int MIN_HZ = 100;
    static constexpr int MAX_HZ = 3000;
    static constexpr float BIN_HZ = 6.25f;
    static constexpr int NUM_BINS = int((MAX_HZ - MIN_HZ) / BIN_HZ + 0.5f);
    float activity[NUM_BINS];
    uint16_t bin_to_hz(int bin) const;
    float cq_density[NUM_BINS];
    uint32_t last_update[NUM_BINS];
    
private:

    static constexpr int SPREAD_BINS = 4;
    static constexpr float DECAY_SEC = 30.0f;

    uint32_t current_base_freq;

    int hz_to_bin(int hz) const;
    float snr_weight(int snr) const;
    float gaussian(int x) const;

    uint32_t extract_uint(const char *s, const char *key);
    int extract_int(const char *s, const char *key);
    bool extract_bool(const char *s, const char *key);
    uint32_t current_time() const;
};
