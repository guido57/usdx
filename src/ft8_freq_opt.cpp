#include <Arduino.h>
#include "ft8_freq_opt.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cstdio>


FT8FreqOptimizer::FT8FreqOptimizer() {
    clear();
    current_base_freq = 7074000;
    memset(cq_last_update, 0, sizeof(cq_last_update));
   
}

void FT8FreqOptimizer::clear() {
    memset(activity, 0, sizeof(activity));
    memset(cq_density, 0, sizeof(cq_density));
    memset(last_update, 0, sizeof(last_update));
    memset(cq_last_update, 0, sizeof(cq_last_update));
}

void FT8FreqOptimizer::store(uint32_t freq, int snr, uint32_t ts, bool is_cq, uint32_t current_base_freq_hz) {
    current_base_freq = current_base_freq_hz;
    
    //uint32_t freq = extract_uint(json, "\"frequency_hz\":");
    //int snr = extract_int(json, "\"snr_db\":");
    // uint32_t ts = extract_uint(json, "\"timestamp\":");
    // bool is_cq = extract_bool(json, "\"cq\":");

    if (!freq || !ts) return;

    int32_t audio = (int32_t)freq - (int32_t)current_base_freq;
    if (audio < MIN_HZ || audio > MAX_HZ) return;

    int bin = hz_to_bin(audio);
    float weight = snr_weight(snr);

    for (int i = -SPREAD_BINS; i <= SPREAD_BINS; i++) {
        int b = bin + i;
        if (b < 0 || b >= NUM_BINS) continue;

        uint32_t age = (ts > last_update[b]) ? (ts - last_update[b]) : 0;
        float decay = expf(-(float)age / DECAY_SEC);

        // decay previous content before adding new energy
        activity[b] = activity[b] * decay + weight * gaussian(i);
        last_update[b] = ts;

    }

    if (is_cq) {
        uint32_t age = (ts > cq_last_update[bin]) ? (ts - cq_last_update[bin]) : 0;
        float decay = expf(-(float)age / (DECAY_SEC * 2)); // slower decay optional

        cq_density[bin] = cq_density[bin] * decay + 1.0f;
        cq_last_update[bin] = ts;
    }
    
}

uint16_t FT8FreqOptimizer::best_freq(uint32_t base_freq_hz, bool collision_prediction, bool cq_attractor) {
    current_base_freq = base_freq_hz;
    uint32_t now = current_time();


    for (int i = 0; i < NUM_BINS; i++) {
        uint32_t age = (now > last_update[i]) ? (now - last_update[i]) : 0;
        float decay = expf(-(float)age / DECAY_SEC);
        decayed[i] = activity[i] * decay;
    }

    float best_score = 1e9;
    int best_bin = hz_to_bin(1200);

    for (int i = 0; i < NUM_BINS; i++) {
        float a = decayed[i];

        uint32_t age = (now > last_update[i]) ? (now - last_update[i]) : 0;
        if (collision_prediction && age < 20) {
            a *= 3.0f;
        }

        float smooth = 0;
        int count = 0;

        for (int k = -SMOOTH_RADIUS; k <= SMOOTH_RADIUS; k++) {
            int j = i + k;
            if (j < 0 || j >= NUM_BINS) continue;

            smooth += decayed[j] ;
            count++;
        }

        smooth /= (count > 0 ? count : 1);

        // guard band
        float guard = 0;
        for (int k = -GUARD_RADIUS; k <= GUARD_RADIUS; k++) {
            int j = i + k;
            if (j < 0 || j >= NUM_BINS) continue;

            guard += decayed[j] ;
        }

        float score = 0.7f * smooth + 0.3f * a;
        score += guard * 0.3;// penalize bins with active neighbors (potential collisions)

        if (cq_attractor) {
           float cq = cq_density[i];
           score -= logf(1.0f + cq) * 2.0f;
        }

        // edge penalty
        uint16_t hz = bin_to_hz(i);
        float edge_penalty = 0;

        if (hz < 400)
            edge_penalty += expf((400.0f - hz) / 100.0f);

        if (hz > 2700)
            edge_penalty += expf((hz - 2700.0f) / 100.0f);

        score += edge_penalty * 5.0f;
        
        if (score < best_score) {
            best_score = score;
            best_bin = i;
        }
        // if(activity[i] > 0.01f)
        //     Serial.printf("Bin %3d: Activity=%.2f, Score=%.2f\n", i, activity[i], score);
    }

    Serial.printf("Best bin: %d, Best freq: %u Hz, Activity=%.2f, Score=%.2f\n", best_bin, bin_to_hz(best_bin), decayed[best_bin], best_score);
    // Serial.printf("Activity around best bin: ");
    // int start_bin = max(0, best_bin - 5);
    // int end_bin = min(NUM_BINS - 1, best_bin + 5);
    // for (int i = start_bin; i <= end_bin; i++) {
    //         Serial.printf("Bin %3d  Freq %u Hz:  Activity=%.2f, CQ Density=%.2f\n", i, bin_to_hz(i), decayed[i], cq_density[i]);
    // }
    
    return  bin_to_hz(best_bin);
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

float FT8FreqOptimizer::snr_weight(int snr) const { 
    return powf(10.0f, snr / 20.0f);  // physically meaningful

}

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
