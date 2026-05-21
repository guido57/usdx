#include <Arduino.h>
#include "qsostats.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ---------------- RECORD ----------------
struct StatsRecord {
    uint32_t magic;
    uint32_t version;

    uint8_t worked_dxcc[MAX_DXCC / 8];
    uint8_t worked_band[MAX_DXCC][(NUM_BANDS + 7) / 8];
    uint16_t qsos_with_dxcc[MAX_DXCC];
    uint32_t last_qso_time[MAX_DXCC];

    uint32_t end_marker;
};

static portMUX_TYPE stats_save_mux = portMUX_INITIALIZER_UNLOCKED;
static StatsRecord pending_rec;
static StatsRecord save_worker_rec;
static bool pending_valid = false;
static int pending_index = -1;
static uint32_t pending_version = 0;

extern fs::LittleFSFS LogsFS; // Separate LittleFS instance for logs to avoid wear on the main filesystem

// ---------------- CTOR ----------------
QSOStats::QSOStats()
{
    memset(worked_dxcc, 0, sizeof(worked_dxcc));
    memset(worked_band, 0, sizeof(worked_band));
    memset(qsos_with_dxcc, 0, sizeof(qsos_with_dxcc));
    memset(last_qso_time, 0, sizeof(last_qso_time));

    current_version = 0;
    current_index = -1;
    last_save_ms = 0;
    stats_dirty = false;
    save_task_handle = nullptr;

    prefixCount = 0;
}
// ---------------- INIT ----------------
void QSOStats::begin()
{
    loadFromFlash();

    if (!save_task_handle) {
        xTaskCreatePinnedToCore(
            QSOStats::flashSaveTask,
            "QSOStatsSave",
            12288,
            this,
            1,
            &save_task_handle,
            0);
    }
}

const char* BandNames[] = {
    "160m",
    "80m",
    "60m",
    "40m",
    "30m",
    "20m",
    "17m",
    "15m",
    "12m",
    "10m"
};


enum Bands {
    BAND_160M = 0,
    BAND_80M,
    BAND_60M,
    BAND_40M,
    BAND_30M,
    BAND_20M,
    BAND_17M,
    BAND_15M,
    BAND_12M,
    BAND_10M
};




int freqToBand(uint32_t freq_hz)
{
    if (freq_hz >= 1800000 && freq_hz <= 2000000) return BAND_160M; // 160m
    if (freq_hz >= 3500000 && freq_hz <= 3800000) return BAND_80M; // 80m
    if (freq_hz >= 5250000 && freq_hz <= 5450000) return BAND_60M; // 60m
    if (freq_hz >= 7000000 && freq_hz <= 7200000) return BAND_40M; // 40m
    if (freq_hz >= 10100000 && freq_hz <= 10150000) return BAND_30M; // 30m
    if (freq_hz >= 14000000 && freq_hz <= 14350000) return BAND_20M; // 20m
    if (freq_hz >= 18068000 && freq_hz <= 18168000) return BAND_17M; // 17m
    if (freq_hz >= 21000000 && freq_hz <= 21450000) return BAND_15M; // 15m
    if (freq_hz >= 24890000 && freq_hz <= 24990000) return BAND_12M; // 12m
    if (freq_hz >= 28000000 && freq_hz <= 29700000) return BAND_10M; // 10m

    return -1;
}

// ---------------- BIT OPS ----------------
// Simple bit array accessors for worked_dxcc and worked_band
// it returns true if the bit at index idx is set, false otherwise. 
bool QSOStats::getBit(const uint8_t* arr, int idx) const
{
    return arr[idx >> 3] & (1 << (idx & 7));
}

void QSOStats::setBit(uint8_t* arr, int idx)
{
    arr[idx >> 3] |= (1 << (idx & 7));
}

void QSOStats::clearBit(uint8_t* arr, int idx)
{
    arr[idx >> 3] &= ~(1 << (idx & 7));
}
// ---------------- PREFIX CLEAN ----------------
void QSOStats::cleanPrefix(char* p)
{
    if (!p) return;

    // --- 1. trim leading whitespace ---
    char* start = p;
    while (*start && isspace((unsigned char)*start))
        start++;

    if (start != p)
        memmove(p, start, strlen(start) + 1);

    // --- 2. copy ONLY valid prefix chars ---
    char* out = p;

    for (char* s = p; *s; s++) {

        // stop at known CTY modifiers
        if (*s == '(' || *s == '[' || *s == '=')
            break;

        // stop at anything that is NOT callsign-safe
        if (!(isalnum((unsigned char)*s) || *s == '/'))
            break;

        *out++ = toupper(*s);
    }

    *out = 0;
}

bool QSOStats::loadCTYFromFile(const char* path)
{
    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("Failed to open CTY file: %s\n", path);
        return false;
    }

    size_t size = f.size();
    Serial.printf("CTY file size: %d bytes\n", (int)size);

    // ⚠️ IMPORTANT: allocate buffer
    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        Serial.println("Memory allocation failed for CTY");
        f.close();
        return false;
    }

    size_t read = f.read((uint8_t*)buffer, size);
    buffer[read] = 0;

    f.close();

    // parse buffer and place prefixes on prefixTable
    loadCTY(buffer);

    free(buffer);

    Serial.printf("Loaded %d prefixes\n", prefixCount);
    for(int i = 0; i < prefixCount ; i++) {
        // if(prefixTable[i].dxcc == 318 /* China */ ) {
        //     Serial.printf("Prefix: %s, DXCC: %d\n", prefixTable[i].prefix, prefixTable[i].dxcc);
        // }
        
    }

    return true;
}

static inline char* ltrim(char* s)
{
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')
        s++;
    return s;
}
// ---------------- CTY PARSER ----------------
void QSOStats::loadCTY(const char* text)
{
    Serial.println("Loading CTY data...");
    Serial.printf("CTY data length: %d\n", (int)strlen(text));

    prefixCount = 0;
    dxccCount = 0;

    const char* ptr = text;

    while (*ptr) {

        const char* end = strchr(ptr, ';');
        if (!end) break;

        int len = end - ptr;
        if (len <= 0) {
            ptr = end + 1;
            continue;
        }

        // allocate full block
        char* block = (char*)malloc(len + 1);
        if (!block) {
            Serial.println("malloc failed");
            ptr = end + 1;
            continue;
        }

        memcpy(block, ptr, len);
        block[len] = 0;

        // --- find last colon safely ---
        char* lastColon = strrchr(block, ':');
        if (!lastColon) {
            free(block);
            ptr = end + 1;
            continue;
        }

        char* prefixPart = lastColon + 1;
        *lastColon = 0;  // terminate header

        // --- parse header ---
        char* fields[12];
        int fieldCount = 0;

        char* tok = strtok(block, ":");
        while (tok && fieldCount < 12) {
            fields[fieldCount++] = tok;
            tok = strtok(NULL, ":");
        }

        if (fieldCount < 2) {
            free(block);
            ptr = end + 1;
            continue;
        }

        int dxcc = (fieldCount >= 9) ? atoi(fields[8]) : atoi(fields[1]);
        if (dxcc <= 0) {
            free(block);
            ptr = end + 1;
            continue;
        }

        // get DXCC name from fields[0] if available, 
        // otherwise use "DXCC <code>"
        bool found = false;

        for (int i = 0; i < dxccCount; i++) {
            if (dxccTable[i].dxcc == dxcc) {
                found = true;
                break;
            }
        }

        if (!found && dxccCount < MAX_DXCC) {
            dxccTable[dxccCount].dxcc = dxcc;

            const char* name = ltrim(fields[0]);
            strncpy(dxccTable[dxccCount].name, name, 31);
            dxccTable[dxccCount].name[31] = 0;

            dxccCount++;
        }

        // --- parse prefixes ---
        char* pfx = strtok(prefixPart, ",");

        while (pfx && prefixCount < MAX_PREFIXES) {

            cleanPrefix(pfx);

            // uppercase
            for (char* p = pfx; *p; p++)
                *p = toupper(*p);

            if (*pfx) {
                strncpy(prefixTable[prefixCount].prefix, pfx, 7);
                prefixTable[prefixCount].prefix[7] = 0;
                prefixTable[prefixCount].dxcc = dxcc;

                prefixCount++;
            }

            pfx = strtok(NULL, ",");
        }

        free(block);
        ptr = end + 1;
    }

    Serial.printf("CTY loaded: %d prefixes\n", prefixCount);
}// ---------------- DXCC ----------------

int QSOStats::dxccFromCall(const char* call)
{
    int bestDxcc = -1;
    int bestLen = 0;

    for (int i = 0; i < prefixCount; i++) {
        //Serial.printf("Checking prefix %s (DXCC %d) against call %s\n", prefixTable[i].prefix, prefixTable[i].dxcc, call);
        int len = strlen(prefixTable[i].prefix);

        if (len <= bestLen) continue;

        if (strncmp(call, prefixTable[i].prefix, len) == 0) {
            bestLen = len;
            bestDxcc = prefixTable[i].dxcc;
        }
    }
    return bestDxcc;
}

// ---------------- SCORING ----------------

float QSOStats::scoreCQ(const int16_t dxcc, int snr, uint32_t freq_hz)
{
    int band = freqToBand(freq_hz);
    if (band < 0) return 0.0f;

   //int dxcc = dxccFromCall(call);
    if (dxcc < 0) return 0.0f;
    float score = computeScore(dxcc, snr, band);
    return score;
}

// compute score based on whether it's a new DXCC, new band, rarity (QSOs with that DXCC), and SNR
// a new DXCC is worth 1000 points, 
// a new band with an already worked DXCC is worth 300 points, 
// rarity (few QSOs) is worth up to 200 points, 
// SNR can contribute up to 50 points for very weak signals
float QSOStats::computeScore(int dxcc, int snr, int band)
{
    bool isNewDXCC = !getBit(worked_dxcc, dxcc);
    bool isNewBand = !getBit(worked_band[dxcc], band);

    float rarity = 1.0f / (1.0f + qsos_with_dxcc[dxcc]);

    float snrNorm = (snr + 30.0f) / 60.0f;  // e.g. snr=-5 → snrNorm=0.416, snr=+10 → snrNorm=0.666 
    if (snrNorm < 0) snrNorm = 0;           
    if (snrNorm > 1) snrNorm = 1;           

    float snrScore = snrNorm;  // linear contribution from 0 to 50 points
                               // the higher the SNR, the higher the score, but even weak signals (e.g. SNR=-20 dB) can contribute some points 

    return
        1000.0f * (isNewDXCC ? 1.0f : 0.0f) +
        300.0f  * (isNewBand ? 1.0f : 0.0f) +
        200.0f  * rarity +
        50.0f   * snrScore;
}

// ---------------- UPDATE ----------------

void QSOStats::onQSOCompleted(const char* call, uint32_t freq_hz, uint32_t epoch)
{
    int band = freqToBand(freq_hz);
    Serial.printf("onQSOCompleted: freq_hz=%d, band=%d\n", freq_hz, band);  
    if (band < 0) return;

    int dxcc = dxccFromCall(call);
    Serial.printf("onQSOCompleted: dxccFromCall returned %d from call=%s\n", dxcc, call);
    if (dxcc < 0){ 
        return;
    };

    Serial.printf("QSO Completed: call=%s, dxcc=%d, band=%d\n", call, dxcc, band);
    setBit(worked_dxcc, dxcc);
    setBit(worked_band[dxcc], band);

    if (qsos_with_dxcc[dxcc] < 65535)
        qsos_with_dxcc[dxcc]++;

    last_qso_time[dxcc] = epoch;
    stats_dirty = true;
}
// ---------------- PERIODIC SAVE ----------------

void QSOStats::periodicSave(unsigned long now_ms)
{
    if (now_ms < last_save_ms + 60000UL) return;

    // if (!stats_dirty) {
    //     last_save_ms = now_ms;
    //     return;
    // }

    saveToFlash();
    last_save_ms = now_ms;
    Serial.println("QSOStats: Save queued at " + String(now_ms) + " ms");
}

// ---------------- FILE HELPERS ----------------

void QSOStats::makeFilename(int index, char* out, size_t len)
{
    snprintf(out, len, "/qsostats_%02d.bin", index);
}

// ---------------- FLASH ----------------

bool QSOStats::readRecord(int index, void* out)
{
    char path[32];
    //Serial.printf("readRecord: Reading record at index %d\n", index);
    makeFilename(index, path, sizeof(path));

    // Serial.printf("Reading record from flash: %s\n", path);
    File f = LogsFS.open(path, "r");
    if (!f){ 
        Serial.println("Failed to open file"); 
        return false; 
    }

    if (f.size() != sizeof(StatsRecord)) {
        f.close();
        Serial.println("File size mismatch, expected " + String(sizeof(StatsRecord)) + " bytes");
        return false;
    }

    size_t r = f.read((uint8_t*)out, sizeof(StatsRecord));
    f.close();

    if (r != sizeof(StatsRecord)) {
        Serial.println("Failed to read complete record");
        return false;
    }

    return true;
}

bool QSOStats::writeRecord(int index, const void* data, size_t len)
{
    char path[32];

    makeFilename(index, path, sizeof(path));

    File f = LogsFS.open(path, "w");
    if (!f) return false;

    size_t w = f.write((const uint8_t*)data, len);
    f.close();

    return w == len;
}

bool QSOStats::eraseRecord(int index)
{
    char path[32];
    makeFilename(index, path, sizeof(path));

    if (LogsFS.exists(path))
        return LogsFS.remove(path);

    return true;
}

// allocate record once in .bss, not in stack
static StatsRecord rec;

// ---------------- LOAD ----------------

void QSOStats::loadFromFlash()
{
    Serial.println("Loading QSOStats from flash...");
    int idx = findLatestRecord();
    Serial.printf("QSOStats: Latest QSOStats record index: %d\n", idx);
    if (idx < 0) return;

    //StatsRecord rec;

    if (!readRecord(idx, &rec)) {
        Serial.println("QSOStats: Failed to read record from flash");
        return;
    }
    memcpy(worked_dxcc, rec.worked_dxcc, sizeof(worked_dxcc));
    memcpy(worked_band, rec.worked_band, sizeof(worked_band));
    memcpy(qsos_with_dxcc, rec.qsos_with_dxcc, sizeof(qsos_with_dxcc));
    memcpy(last_qso_time, rec.last_qso_time, sizeof(last_qso_time));

    current_version = rec.version;
    current_index = idx;
}

// ---------------- SAVE ----------------

void QSOStats::saveToFlash()
{
    queueSaveSnapshot();
}

void QSOStats::queueSaveSnapshot()
{
    int next;
    uint32_t version;

    portENTER_CRITICAL(&stats_save_mux);

    pending_rec.magic = STATS_MAGIC;
    version = current_version + 1;
    next = (current_index + 1) % STATS_MAX_RECORDS;
    pending_rec.version = version;

    memcpy(pending_rec.worked_dxcc, worked_dxcc, sizeof(worked_dxcc));
    memcpy(pending_rec.worked_band, worked_band, sizeof(worked_band));
    memcpy(pending_rec.qsos_with_dxcc, qsos_with_dxcc, sizeof(qsos_with_dxcc));
    memcpy(pending_rec.last_qso_time, last_qso_time, sizeof(last_qso_time));

    pending_rec.end_marker = STATS_MAGIC;

    pending_index = next;
    pending_version = version;
    pending_valid = true;
    stats_dirty = false;

    portEXIT_CRITICAL(&stats_save_mux);

    if (save_task_handle) {
        xTaskNotifyGive(save_task_handle);
    }
}

void QSOStats::flashSaveTask(void* arg)
{
    QSOStats* self = static_cast<QSOStats*>(arg);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;) {
            int local_index;
            uint32_t local_version;

            portENTER_CRITICAL(&stats_save_mux);
            if (!pending_valid) {
                portEXIT_CRITICAL(&stats_save_mux);
                break;
            }

            save_worker_rec = pending_rec;
            local_index = pending_index;
            local_version = pending_version;
            pending_valid = false;
            portEXIT_CRITICAL(&stats_save_mux);

            bool ok = self->writeRecord(local_index, &save_worker_rec, sizeof(save_worker_rec));

            portENTER_CRITICAL(&stats_save_mux);
            if (ok) {
                self->current_index = local_index;
                self->current_version = local_version;
            } else {
                self->stats_dirty = true;
            }
            bool has_more = pending_valid;
            portEXIT_CRITICAL(&stats_save_mux);

            if (!has_more) {
                break;
            }
        }
    }
}

// ---------------- FIND ----------------
int QSOStats::findLatestRecord()
{
    // Serial.println("findLatestRecord: Finding latest QSOStats record in flash...");
    
    uint32_t bestVersion = 0;
    int bestIndex = -1;

    for (int i = 0; i < STATS_MAX_RECORDS; i++) {

        // Serial.printf("findLatestRecord: Checking record index %d...\n", i);
        if (!readRecord(i, &rec)) continue;

        if (rec.magic == STATS_MAGIC &&
            rec.end_marker == STATS_MAGIC) {

            if (rec.version >= bestVersion) {
                bestVersion = rec.version;
                bestIndex = i;
            }
        }
    }

    return bestIndex;
}

const char* QSOStats::getCountryName(int dxcc)
{
    for (int i = 0; i < MAX_DXCC; i++) {
        if (dxccTable[i].dxcc == dxcc)
            return dxccTable[i].name;
    }
    return "Unknown";
}

String QSOStats::getJsonWorkedDXCC()
{
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int dxcc = 0; dxcc < MAX_DXCC; dxcc++) {

        char * country_name = (char *)getCountryName(dxcc);
        // not all DXCC codes are valid, skip those with "Unknown" name
        if(strcmp(country_name, "Unknown") == 0) {
            continue;
        }

        bool worked = getBit(worked_dxcc, dxcc);
        JsonObject o = arr.add<JsonObject>();
        o["dxcc"] = dxcc;
        o["country"] = country_name;
        o["worked"] = worked;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

String QSOStats::getJsonQSOsDXCC()
{
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int dxcc = 0; dxcc < MAX_DXCC; dxcc++) {

        char * country_name = (char *)getCountryName(dxcc);
        
        // not all DXCC codes are valid, skip those with "Unknown" name
        if(strcmp(country_name, "Unknown") == 0) {
            continue;
        }

        uint16_t num_qsos = qsos_with_dxcc[dxcc];
                
        JsonObject o = arr.add<JsonObject>();
        o["dxcc"] = dxcc;
        o["country"] = country_name;
        o["qsos"] = num_qsos;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

String QSOStats::getJsonBandsDXCC()
{
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int dxcc = 0; dxcc < MAX_DXCC; dxcc++) {

        char * country_name = (char *)getCountryName(dxcc);
        
        // not all DXCC codes are valid, skip those with "Unknown" name
        if(strcmp(country_name, "Unknown") == 0) {
            continue;
        }

        for(int band = 0; band < NUM_BANDS; band++) {
            
            bool worked = getBit(worked_band[dxcc], band);
            if(worked){
                JsonObject o = arr.add<JsonObject>();
                o["d"] = dxcc;
                o["c"] = country_name;
                o["b"] = band;
                o["n"] = BandNames[band];
                o["w"] = worked;
            }
        }    
    }

    String out;
    serializeJson(doc, out);
    return out;
}

void QSOStats::clearAllWorkedDXCC()
{
    for (int dxcc = 0; dxcc < MAX_DXCC; dxcc++) {

        qsos_with_dxcc[dxcc] = 0;
        clearBit(worked_dxcc, dxcc);

        for (int band = 0; band < NUM_BANDS; band++) {
            clearBit(worked_band[dxcc], band);
        }
    }

    stats_dirty = true;
}