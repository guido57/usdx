#pragma once

#include <Arduino.h>
#include "configuration.h"

// ------------------- UI / Voltage Limits -------------------
#define RX_ATT_UI_MAX   30.0f     // UI scale max value
#define RX_ATT_MAX_V    3.0f      // Max gate control voltage (V)

// ------------------- PWM Configuration ----------------------
#define RX_ATT_PWM_PIN  GPIO_PWM_RX_ATT
#define RX_ATT_PWM_CH   2
#define RX_ATT_PWM_FREQ 31250     // 31.25 kHz
#define RX_ATT_PWM_RES  10        // 10-bit resolution (0–1023)
#define RX_ATT_PWM_MAX  ((1 << RX_ATT_PWM_RES) - 1)

// ------------------- Functions ------------------------------
void rxAttPwmInit();
void setRxAttFromUi(uint8_t uiVal);
