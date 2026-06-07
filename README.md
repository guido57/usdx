This software is derived from https://github.com/threeme3/usdx but it has been completely rewritten for ESP32-S3 N16R8 Devkit C1.

Its hardware is here https://hackaday.io/project/205224-esp32-hf-qrp-ft8-transceiver

# ESP32 SDR Receiver / FT8 Transceiver

This project implements a full **IQ SDR receiver** and **FT8 transceiver** on an ESP32 using external RF front-end circuitry. It includes DSP processing, **FT8 embedded coding and decoding**, audio output via I2S, network audio streaming, and automatic antenna band filtering.  

---

## Features

- True IQ SDR processing: **LSB/USB/AM/CW demodulation**
- High-quality ADC interface: **PCM1808**
- CIC decimation and DC removal
- IQ amplitude/phase calibration
- Selectable audio filters and **AGC**
- Volume control and RIT support
- **Network audio streaming** (websocket) over Wi-Fi or Ethernet
- **Web Server** for remote operations over Wi-Fi or Ethernet
- Automatic **antenna filter selection** via MCP23017 GPIO
- **FT8 TX and RX support** with frequency optimization
- **FT8 embedded coding and decoding** using my library [](https://github.com/guido57/ESP32_ft8_lib) 

---

## Architecture Overview

![](https://cdn.hackaday.io/images/2677411778833313819.png)


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
- Frequency optimization to avoid interferences
- For FT8 decoding, it continuosly streams audio to an external server where a python program:
  - receives audio
  - every 15 seconds runs a command line FT8 decoder (the same used by WSJT-X) to obtain the decoded messages
  - sends back decoded messages to the ESP32

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

- ESP32 (any ESP32 variant with enough flash should work). I used ESP32-S3 N16R8 i.e. 16MB Flash, 8MB PSRAM
- PCM1808 ADC
- MAX98357 DAC (I2S audio)
- SI5351 frequency synthesizer
- MCP23017 for antenna relay switching
- Tayloe / QSD or other IQ front-end
- W5500 (optional) for Ethernet

### 7️⃣ Performance Notes

- Designed for efficient CPU usage on ESP32
- DSP pipeline supports real-time audio at 8 kHz
- Wi-Fi streaming allows network SDR operation
- Built-in calibration tools for IQ imbalance and clipping detection

### 8️⃣ ESP32-N16R8 memory partitioning
I adopted a custom partitioning as you can see in my  mypartitions.csv

```text
 Name,   Type, SubType, Offset,   Size, Flags
nvs,      data, nvs,      0x9000,   0x5000,
otadata,  data, ota,      0xe000,   0x2000,
app0,     app,  ota_0,    0x10000,  0x300000,   
app1,     app,  ota_1,    0x310000, 0x300000,   
logs,     data, spiffs,   0x610000, 0x200000,   
spiffs,   data, spiffs,   0x810000, 0x200000,   
coredump, data, coredump, 0xA10000, 0x20000,
```

I halved "data" into "logs" and "spiffs", both with a length of 0x200000 bytes:
- logs is read/written by the program (see qsostats.cpp) to persist QSO scoring but **it is not overwritten when I run 'pio run --target uploadfs'**
- spiffs contains all the files for the web server (index.html, app.js and so on), it is written with all the files contained in /data using 'pio run --target uploadfs' and it is read (only) by the program   

## Web User Interface

![](https://cdn.hackaday.io/images/1838451778835183107.png)

- answer a CQ double clicking a row
- follow the evolution of your or other QSOs 
- make a CQ or any other manual FT8 message
- set or disable the automatic frequency offset
- change the FT8 band (160, 80, 60, 40, 30, 20, 15, 10 meters)

