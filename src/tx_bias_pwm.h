#pragma once

#include <Arduino.h>
#include "configuration.h"

// ------------------- UI / Voltage Limits -------------------
#define TX_BIAS_MAX_V    3.3f      // Max gate control voltage (V). 3.3V
#define TX_BIAS_MAX      33        // Max gate control value (UI steps). 3.3V
#define TX_BIAS_FOR_RX   10        // Gate polarization when receving. It's 1V because at 0V the Vgs and Vds capacitances are too high
// ------------------- PWM Configuration ----------------------
#define TX_BIAS_PWM_CH   3
#define TX_BIAS_PWM_FREQ 31250     // 31.25 kHz
#define TX_BIAS_PWM_RES  10        // 10-bit resolution (0–1023)
#define TX_BIAS_PWM_MAX  ((1 << TX_BIAS_PWM_RES) - 1)

// ------------------- Functions ------------------------------
void txBiasPwmInit();
void setTxBiasFromUi(uint8_t uiVal);