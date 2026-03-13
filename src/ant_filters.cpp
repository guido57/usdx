#include "ant_filters.h"

#define IODIRA 0x00
#define IODIRB 0x01
#define GPIOA  0x12
#define GPIOB  0x13

AntennaFilters::AntennaFilters(uint8_t address, uint16_t pulse_ms)
{
    _addr = address;
    _pulse_ms = pulse_ms;
    gpioA_state = 0;
    gpioB_state = 0;
}

void AntennaFilters::writeRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

void AntennaFilters::begin()
{
    Wire.beginTransmission(_addr);

    if (Wire.endTransmission() != 0)
    {
        Serial.println("MCP23017 NOT found");
        while (1);
    }

    Serial.println("MCP23017 detected");

    writeRegister(IODIRA, 0x00);
    writeRegister(IODIRB, 0x00);

    writeRegister(GPIOA, gpioA_state);
    writeRegister(GPIOB, gpioB_state);
}

void AntennaFilters::updatePorts()
{
    writeRegister(GPIOA, gpioA_state);
    writeRegister(GPIOB, gpioB_state);
}

void AntennaFilters::releaseAll()
{
    gpioA_state = 0;
    gpioB_state = 0;
    updatePorts();
}

void AntennaFilters::pulse()
{
    updatePorts();
    delay(_pulse_ms);
    releaseAll();
}

void AntennaFilters::setBit(uint8_t &port, int bit)
{
    port |= (1 << bit);
}

void AntennaFilters::clearBit(uint8_t &port, int bit)
{
    port &= ~(1 << bit);
}

void AntennaFilters::setTx()
{
    clearBit(gpioB_state, 3);
    clearBit(gpioB_state, 2);
    setBit(gpioB_state, 1);
    setBit(gpioB_state, 0);

    pulse();
}

void AntennaFilters::setRx()
{
    setBit(gpioB_state, 3);
    setBit(gpioB_state, 2);
    clearBit(gpioB_state, 1);
    clearBit(gpioB_state, 0);

    pulse();
}

void AntennaFilters::set14_21_28()
{
    clearBit(gpioB_state, 7);
    clearBit(gpioB_state, 6);
    setBit(gpioB_state, 5);
    setBit(gpioB_state, 4);

    setBit(gpioA_state, 7);
    setBit(gpioA_state, 6);
    clearBit(gpioA_state, 5);
    clearBit(gpioA_state, 4);

    setBit(gpioA_state, 3);
    setBit(gpioA_state, 2);
    clearBit(gpioA_state, 1);
    clearBit(gpioA_state, 0);

    pulse();
}

void AntennaFilters::set1_8_3_5()
{
    setBit(gpioB_state, 7);
    setBit(gpioB_state, 6);
    clearBit(gpioB_state, 5);
    clearBit(gpioB_state, 4);

    setBit(gpioA_state, 7);
    setBit(gpioA_state, 6);
    clearBit(gpioA_state, 5);
    clearBit(gpioA_state, 4);

    clearBit(gpioA_state, 3);
    clearBit(gpioA_state, 2);
    setBit(gpioA_state, 1);
    setBit(gpioA_state, 0);

    pulse();
}

void AntennaFilters::set5_7()
{
    setBit(gpioB_state, 7);
    setBit(gpioB_state, 6);
    clearBit(gpioB_state, 5);
    clearBit(gpioB_state, 4);

    clearBit(gpioA_state, 7);
    clearBit(gpioA_state, 6);
    setBit(gpioA_state, 5);
    setBit(gpioA_state, 4);

    setBit(gpioA_state, 3);
    setBit(gpioA_state, 2);
    clearBit(gpioA_state, 1);
    clearBit(gpioA_state, 0);

    pulse();
}

void AntennaFilters::bypassAll()
{
    setBit(gpioB_state, 7);
    setBit(gpioB_state, 6);
    clearBit(gpioB_state, 5);
    clearBit(gpioB_state, 4);

    setBit(gpioA_state, 7);
    setBit(gpioA_state, 6);
    clearBit(gpioA_state, 5);
    clearBit(gpioA_state, 4);

    setBit(gpioA_state, 3);
    setBit(gpioA_state, 2);
    clearBit(gpioA_state, 1);
    clearBit(gpioA_state, 0);

    pulse();
}

void AntennaFilters::setFilter(float freq)
{
    if (freq < 4000000.0)
    {
        set1_8_3_5();
    }
    else if (freq < 10000000.0)
    {
        set5_7();
    }
    else if (freq < 30000000.0)
    {
        set14_21_28();
    }
    else
    {
        bypassAll();
    }
}