#include <Arduino.h>
#include "configuration.h"
#include "tx_bias_pwm.h"

void txBiasPwmInit() {
  
  ledcSetup(TX_BIAS_PWM_CH, TX_BIAS_PWM_FREQ, TX_BIAS_PWM_RES);
  ledcAttachPin(GPIO_PWM_TX_BIAS, TX_BIAS_PWM_CH);
  ledcWrite(TX_BIAS_PWM_CH, TX_BIAS_FOR_RX);
}


void setTxBiasFromUi(uint8_t uiVal)
{
    if (uiVal >  TX_BIAS_MAX) uiVal = TX_BIAS_MAX;

    // Convert UI step → voltage (0–3.3 V)
    float vGate = (uiVal * TX_BIAS_MAX_V) / TX_BIAS_MAX;

    // Convert voltage → PWM duty
    float dutyF = (vGate / 3.3f) * TX_BIAS_PWM_MAX;
    uint32_t duty = (uint32_t)(dutyF + 0.5f);

    ledcWrite(TX_BIAS_PWM_CH, duty);

    //Serial.printf("TX BIAS UI=%u  Vgate=%.2fV  duty=%u\n", uiVal, vGate, duty);
}