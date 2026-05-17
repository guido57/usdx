#ifndef QSOSTATS_H
#define QSOSTATS_H

#include <Arduino.h>

#define MAX_DXCC        600 // Kosovo has DXCC code 522
#define NUM_BANDS       10
#define MAX_PREFIXES    5000

#define STATS_MAGIC     0xA55A1234
#define STATS_MAX_RECORDS 16

struct PrefixEntry {
    char prefix[8];
    uint16_t dxcc;
};

struct DXCCEntry {
    int dxcc;
    char name[32];
};

extern const char* BandNames[];

int freqToBand(uint32_t freq_hz);


class QSOStats {
public:
    QSOStats();

    void begin();
    void loadCTY(const char* text);
    bool loadCTYFromFile(const char* path);

    float scoreCQ(int16_t dxcc, int snr, uint32_t freq_hz);
    void onQSOCompleted(const char* call, uint32_t freq_hz, uint32_t epoch);
    int dxccFromCall(const char* call);
    
    String getJsonWorkedDXCC();
    String getJsonQSOsDXCC();
    String getJsonBandsDXCC();
    void clearAllWorkedDXCC();

    void periodicSave(unsigned long now_ms);

private:

    DXCCEntry dxccTable[MAX_DXCC];
    int dxccCount;

    // ---- stats ----
    uint8_t worked_dxcc[MAX_DXCC / 8];
    uint8_t worked_band[MAX_DXCC][(NUM_BANDS + 7) / 8];
    uint16_t qsos_with_dxcc[MAX_DXCC];
    uint32_t last_qso_time[MAX_DXCC];

    uint32_t current_version;
    int current_index;
    unsigned long last_save_ms;
    bool stats_dirty;
    TaskHandle_t save_task_handle;

    // ---- prefixes ----
    PrefixEntry prefixTable[MAX_PREFIXES];
    int prefixCount;

    // ---- logic ----
    float computeScore(int dxcc, int snr, int band);
    void cleanPrefix(char* p);

    bool getBit(const uint8_t* arr, int idx) const;
    void setBit(uint8_t* arr, int idx);
    void clearBit(uint8_t* arr, int idx);

    const char* getCountryName(int dxcc);

    // ---- persistence ----
    void loadFromFlash();
    void saveToFlash();
    int findLatestRecord();
    void queueSaveSnapshot();
    static void flashSaveTask(void* arg);

    bool readRecord(int index, void* out);
    bool writeRecord(int index, const void* data, size_t len);
    bool eraseRecord(int index);

    void makeFilename(int index, char* out, size_t len);
};

#endif
