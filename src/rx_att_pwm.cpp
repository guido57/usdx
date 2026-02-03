#include <Arduino.h>
#include "configuration.h"

#define RX_ATT_UI_MAX 30.0f
#define RX_ATT_MAX_V  3.0f   // limit gate voltage

#define RX_ATT_PWM_PIN      GPIO_PWM_RX_ATT
#define RX_ATT_PWM_CH       2
#define RX_ATT_PWM_FREQ     31250     // 31.25 kHz
#define RX_ATT_PWM_RES      10        // 10-bit (0–1023)
#define RX_ATT_PWM_MAX      ((1 << RX_ATT_PWM_RES) - 1)

void rxAttPwmInit() {
  
  ledcSetup(RX_ATT_PWM_CH, RX_ATT_PWM_FREQ, RX_ATT_PWM_RES);
  ledcAttachPin(RX_ATT_PWM_PIN, RX_ATT_PWM_CH);
  // Start with max attenuation (0V on gate)
  ledcWrite(RX_ATT_PWM_PIN, 0);
}


void setRxAttFromUi(uint8_t uiVal)
{
    if (uiVal > 30) uiVal = 30;

    // Convert UI step → voltage (0–3.0 V)
    float vGate = (uiVal / RX_ATT_UI_MAX) * RX_ATT_MAX_V;

    // Convert voltage → PWM duty
    float dutyF = (vGate / 3.3f) * RX_ATT_PWM_MAX;
    uint32_t duty = (uint32_t)(dutyF + 0.5f);

    ledcWrite(RX_ATT_PWM_PIN, duty);

    // Serial.printf("RF ATT UI=%u  Vgate=%.2fV  duty=%u\n", uiVal, vGate, duty);
}