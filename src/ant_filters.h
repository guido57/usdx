#ifndef ANT_FILTERS_H
#define ANT_FILTERS_H

#include <Arduino.h>
#include <Wire.h>

class AntennaFilters
{
public:
    AntennaFilters(uint8_t address, uint16_t pulse_ms = 50);

    void begin();

    void bypassAll();

    void set14_21_28();
    void set1_8_3_5();
    void set5_7();

    void setTx();
    void setRx();

     void setFilter(float freq);   // NEW

private:
    uint8_t _addr;
    uint16_t _pulse_ms;

    uint8_t gpioA_state;
    uint8_t gpioB_state;

    void writeRegister(uint8_t reg, uint8_t value);
    void updatePorts();
    void releaseAll();
    void pulse();

    void setBit(uint8_t &port, int bit);
    void clearBit(uint8_t &port, int bit);
};

#endif