// audio_i2s.cpp - I2S audio output using legacy ESP-IDF I2S driver (PSRAM compatible)
#include <Arduino.h>
#include "audio_i2s.h"
#include "configuration.h"
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#define I2S_NUM I2S_NUM_0

static bool initialized = false;

static float y0_cos = 0.0f;
static float y1_cos = 0.70710678f;  // initial phase
static float k_cos;

static QueueHandle_t i2s_event_queue;

void i2sTask(void *arg)
{
    i2s_event_t event;
    int16_t block[128];  // 64 stereo frames

    while (true)
    {
        if (xQueueReceive(i2s_event_queue, &event, portMAX_DELAY))
        {
            if (event.type == I2S_EVENT_TX_DONE)
            {
                // DMA consumed one block
                // Generate next 64 samples

                for (int i = 0; i < 64; i++)
                {
                    float y = k_cos * y1_cos - y0_cos;
                    y0_cos = y1_cos;
                    y1_cos = y;

                    int16_t s = (int16_t)(y * 2000);

                    block[i*2]   = s;   // Left
                    block[i*2+1] = s;   // Right
                }

                size_t bytes_written;
                i2s_write(I2S_NUM_1,
                          block,
                          sizeof(block),
                          &bytes_written,
                          portMAX_DELAY);
            }
        }
    }
}

bool audio_i2s_setup()
{
    if (initialized)
    {
        Serial.println("I2S already initialized");
        return true;
    }

    // Legacy I2S Configuration (PSRAM compatible)
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = GPIO_I2S_BCLK,
        .ws_io_num = GPIO_I2S_LRC,
        .data_out_num = GPIO_I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE};

    // Install and start I2S driver
    // esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 4, &i2s_event_queue);
    if (err != ESP_OK)
    {
        Serial.printf("Failed to install I2S driver: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_1, &pin_config);
    if (err != ESP_OK)
    {
        Serial.printf("Failed to set I2S pins: %d\n", err);
        i2s_driver_uninstall(I2S_NUM_1);
        return false;
    }

    initialized = true;

    Serial.printf("I2S initialized: %dHz stereo (mono via MAX98357), BCLK=%d, LRC=%d, DOUT=%d\n",
                  AUDIO_I2S_SAMPLE_RATE, GPIO_I2S_BCLK, GPIO_I2S_LRC, GPIO_I2S_DOUT);

    return true;
}


bool audio_i2s_setup_with_dma()
{
    if (initialized)
    {
        Serial.println("I2S already initialized");
        return true;
    }

    // Legacy I2S Configuration (PSRAM compatible)
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = GPIO_I2S_BCLK,
        .ws_io_num = GPIO_I2S_LRC,
        .data_out_num = GPIO_I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE};

    // Install and start I2S driver
    // esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    if (err != ESP_OK)
    {
        Serial.printf("Failed to install I2S driver: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_1, &pin_config);
    if (err != ESP_OK)
    {
        Serial.printf("Failed to set I2S pins: %d\n", err);
        i2s_driver_uninstall(I2S_NUM_1);
        return false;
    }


    audio_i2s_start_task(); // Start I2S task to continuously read from PCM1808 and output to MAX98357  

    Serial.printf("I2S initialized: %dHz stereo (mono via MAX98357), BCLK=%d, LRC=%d, DOUT=%d\n",
                  AUDIO_I2S_SAMPLE_RATE, GPIO_I2S_BCLK, GPIO_I2S_LRC, GPIO_I2S_DOUT);


    initialized = true;

    return true;
}

esp_err_t audio_i2s_start_task()
{
    BaseType_t result = xTaskCreate(i2sTask, "i2sTask", 4096, NULL, 5, NULL);
    return (result == pdPASS) ? ESP_OK : ESP_FAIL  ;
}

bool audio_i2s_write_sample(int16_t sample)
{
    if (!initialized)
    {
        return false;
    }

    // MAX98357 needs stereo format - send same sample to both channels
    int16_t stereoSample[2] = {sample, sample};
    size_t bytes_written = 0;

    // Use short timeout (1ms) instead of blocking indefinitely to keep main loop responsive
    esp_err_t err = i2s_write(I2S_NUM_1, stereoSample, sizeof(stereoSample), &bytes_written, pdMS_TO_TICKS(1));

    return (err == ESP_OK && bytes_written == sizeof(stereoSample));
}

size_t audio_i2s_write_samples(const int16_t *samples, size_t count)
{
    if (!initialized || samples == nullptr || count == 0)
    {
        return 0;
    }

    // Convert mono to stereo
    for (size_t i = 0; i < count; i++)
    {
        if (!audio_i2s_write_sample(samples[i]))
        {
            return i;
        }
    }
    return count;
}

size_t audio_i2s_available()
{
    // Not applicable for output-only I2S
    return 0;
}

void audio_i2s_stop()
{
    if (initialized)
    {
        i2s_driver_uninstall(I2S_NUM_1);
        initialized = false;
        Serial.println("I2S stopped");
    }
}
