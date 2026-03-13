This software is derived from https://github.com/threeme3/usdx but it has been completely rewritten for ESP32-S3 N16R8 Devkit C1.

Its hardware is here https://hackaday.io/project/205224-esp32-hf-qrp-ft8-transceiver

# ESP32 SDR Receiver / FT8 Transceiver

This project implements a full **IQ SDR receiver** and **FT8 transceiver** on an ESP32 using external RF front-end circuitry. It includes DSP processing, audio output via I2S, network audio streaming, and automatic antenna band filtering.  

---

## Features

- True IQ SDR processing: **LSB/USB/AM/CW demodulation**
- High-quality ADC interface: **PCM1808**
- CIC decimation and DC removal
- IQ amplitude/phase calibration
- Selectable audio filters and **AGC**
- Volume control and RIT support
- **Network audio streaming** over Wi-Fi (websocket)
- Automatic **antenna filter selection** via MCP23017 GPIO
- **FT8 TX and RX support** with frequency optimization

---

## Architecture Overview

### 1️⃣ RF Front-End

- Tayloe / QSD detector for I/Q extraction
- Connected to **PCM1808 ADC** via I2S
- Optional analog pre-filtering per HF band

### 2️⃣ DSP / Demodulator Pipeline
PCM1808 ADC → CIC Decimator → DC Removal → IQ Calibration → Demodulator → Audio Filter → AGC → Volume → I2S / Wi-Fi


- **CIC Decimation:** 32 kHz → 8 kHz audio
- **IQ Calibration:** corrects phase and amplitude imbalance
- **Demodulator:** supports LSB, USB, AM, CW
- **Audio Filter:** selectable bandwidth for each mode
- **AGC:** peak limiting, attack/release control
- **Audio Output:** I2S DAC (MAX98357) or Wi-Fi streaming

### 3️⃣ RF Control

- **SI5351 driver** for local oscillator generation
- UI-controlled frequency, mode, and filter selection
- Automatic antenna filter switching via MCP23017
- **Filter selection rules:**
  - `< 4 MHz` → 1.8 / 3.5 MHz
  - `< 10 MHz` → 5 / 7 MHz
  - `< 30 MHz` → 14 / 21 / 28 MHz
  - `> 30 MHz` → bypass all

### 4️⃣ FT8 Subsystem

- Integrated **FT8 TX engine**
- Frequency optimization to avoid interference
- Works alongside SDR receiver

### 5️⃣ Main Loop Execution

```text
loop():
 ├── ui_loop()                (UI interaction)
 ├── processAudioPCM1808()    (DSP pipeline)
 ├── SI5351 update if needed
 ├── antenna filter update
 ├── TX bias control
 └── periodic FT8 frequency optimization
```


### 6️⃣ Hardware Requirements

- ESP32 (any ESP32 variant should work)
- PCM1808 ADC
- MAX98357 DAC (I2S audio)
- SI5351 frequency synthesizer
- MCP23017 for antenna relay switching
- Tayloe / QSD or other IQ front-end

### 7️⃣ Performance Notes

- Designed for efficient CPU usage on ESP32
- DSP pipeline supports real-time audio at 8 kHz
- Wi-Fi streaming allows network SDR operation
- Built-in calibration tools for IQ imbalance and clipping detection
