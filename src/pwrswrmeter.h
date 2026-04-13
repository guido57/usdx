#pragma once
#include <Arduino.h>

class PowerSWRMeter {
public:
    PowerSWRMeter(gpio_num_t pwrPin, gpio_num_t swrPin, uint8_t samples = 16)
        : _pwrPin(pwrPin), _swrPin(swrPin), _samples(samples) {}

    void begin() {
        analogReadResolution(12); // 0–4095

        analogSetPinAttenuation(_pwrPin, ADC_11db);
        analogSetPinAttenuation(_swrPin, ADC_11db);
    }

    // --- Public API ---
    float readPower() {
        float v = readVoltagePWR();
        return v * v / 50.0f; // P = V^2 / R, with R=50 ohms. Adjust if your detector is different.
    }

    float readSWR() {
        float v = readVoltageSWR(); // there's a 2:1 voltage divider on the SWR input, so multiply by 2 to get actual voltage before conversion
        v = v * v / 50.0f;    
        
        return voltageToSWR(v);
    }

    static const int conversionFactor = 10;  // Power and SWR voltages must be multiplied by this factor
    float readVoltagePWR() { return  conversionFactor * readVoltage(_pwrPin); }
    float readVoltageSWR() { return conversionFactor * readVoltage(_swrPin); }

private:
    gpio_num_t _pwrPin;
    gpio_num_t _swrPin;
    uint8_t _samples;

    // ESP32 ADC reference (approximate)
    static constexpr float VREF = 3.3f;

    // --- Core ADC read with averaging ---
    float readVoltage(gpio_num_t pin) {
        uint32_t sum = 0;

        for (uint8_t i = 0; i < _samples; i++) {
            sum += analogRead(pin);
        }

        float raw = sum / (float)_samples;

        // Convert to voltage
        return (raw / 4095.0f) * VREF;
    }

    // --- SWR conversion (example placeholder) ---
    float voltageToSWR(float v) {
        // You MUST adapt this depending on your detector!
        // Typical directional coupler formula:
        // SWR = (1 + sqrt(Pr/Pf)) / (1 - sqrt(Pr/Pf))

        // For now just return voltage (replace later)
        return v;
    }
};