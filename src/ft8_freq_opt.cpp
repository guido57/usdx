#include "ft8_freq_opt.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cstdio>

FT8FreqOptimizer::FT8FreqOptimizer() {
    clear();
    current_base_freq = 7074000;
}

void FT8FreqOptimizer::clear() {
    memset(activity, 0, sizeof(activity));
    memset(cq_density, 0, sizeof(cq_density));
    memset(last_update, 0, sizeof(last_update));
}

void FT8FreqOptimizer::store(const char *json) {
    uint32_t freq = extract_uint(json, "\"frequency_hz\":");
    int snr = extract_int(json, "\"snr_db\":");
    uint32_t ts = extract_uint(json, "\"timestamp\":");
    bool is_cq = extract_bool(json, "\"cq\":");

    if (!freq || !ts) return;

    int32_t audio = (int32_t)freq - (int32_t)current_base_freq;
    if (audio < MIN_HZ || audio > MAX_HZ) return;

    int bin = hz_to_bin(audio);
    float weight = snr_weight(snr);

    for (int i = -SPREAD_BINS; i <= SPREAD_BINS; i++) {
        int b = bin + i;
        if (b < 0 || b >= NUM_BINS) continue;
        activity[b] += weight * gaussian(i);
        last_update[b] = ts;
    }

    if (is_cq) {
        cq_density[bin] += 1.0f;
    }

    
}

uint16_t FT8FreqOptimizer::best_freq(uint32_t base_freq_hz, bool collision_prediction, bool cq_attractor) {
    current_base_freq = base_freq_hz;
    uint32_t now = current_time();

    float best_score = 1e9;
    int best_bin = hz_to_bin(1200);

    for (int i = 0; i < NUM_BINS; i++) {
        float a = activity[i];
        uint32_t age = now - last_update[i];
        float decay = expf(-(float)age / DECAY_SEC);
        a *= decay;

        if (collision_prediction && age < 20) {
            a *= 3.0f;
        }

        float smooth = 0;
        int count = 0;
        for (int k = -5; k <= 5; k++) {
            int j = i + k;
            if (j < 0 || j >= NUM_BINS) continue;
            smooth += activity[j];
            count++;
        }
        smooth /= count;

        float score = smooth;

        if (cq_attractor) {
            float cq = cq_density[i] * decay;
            score -= cq * 0.4f;
        }

        uint16_t hz = bin_to_hz(i);
        if (hz < 150) score += 50;
        if (hz > 2900) score += 30;

        if (score < best_score) {
            best_score = score;
            best_bin = i;
        }
    }

    return bin_to_hz(best_bin);
}

int FT8FreqOptimizer::hz_to_bin(int hz) const {
    int b = int((hz - MIN_HZ) / BIN_HZ + 0.5f);
    if (b < 0) b = 0;
    if (b >= NUM_BINS) b = NUM_BINS - 1;
    return b;
}
uint16_t FT8FreqOptimizer::bin_to_hz(int bin) const {
    return MIN_HZ + uint16_t(bin * BIN_HZ + 0.5f);
}
float FT8FreqOptimizer::snr_weight(int snr) const { return 1.0f + (snr + 20) * 0.04f; }
float FT8FreqOptimizer::gaussian(int x) const { return expf(-(x * x) / 8.0f); }

uint32_t FT8FreqOptimizer::extract_uint(const char *s, const char *key) {
    const char *p = strstr(s, key);
    if (!p) return 0;
    p += strlen(key);
    return (uint32_t)strtoul(p, NULL, 10);
}

int FT8FreqOptimizer::extract_int(const char *s, const char *key) {
    const char *p = strstr(s, key);
    if (!p) return 0;
    p += strlen(key);
    return (int)strtol(p, NULL, 10);
}

bool FT8FreqOptimizer::extract_bool(const char *s, const char *key) {
    const char *p = strstr(s, key);
    if (!p) return false;
    p += strlen(key);
    // Skip spaces and colon
    while (*p == ' ' || *p == ':' ) p++;

    if (strncmp(p, "true", 4) == 0) return true;
    return false;
    
}

uint32_t FT8FreqOptimizer::current_time() const { return (uint32_t)time(NULL); }
