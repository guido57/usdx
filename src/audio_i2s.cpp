// audio_i2s.cpp - I2S audio output using legacy ESP-IDF I2S driver (PSRAM compatible)
#include <Arduino.h>
#include "audio_i2s.h"
#include "configuration.h"
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#define I2S_NUM I2S_NUM_0

namespace {

constexpr size_t kOutputChunkSamples = 64;
constexpr size_t kAudioQueueDepth = 2048;

static QueueHandle_t g_audio_sample_queue = nullptr;
static TaskHandle_t g_audio_writer_task = nullptr;
static volatile bool g_audio_writer_running = false;

static void i2sAudioWriterTask(void* arg)
{
    (void)arg;
    int16_t mono_samples[kOutputChunkSamples];
    int16_t i2s_frames_lr[kOutputChunkSamples * 2];

    g_audio_writer_running = true;
    while (g_audio_writer_running)
    {
        int16_t first = 0;
        if (xQueueReceive(g_audio_sample_queue, &first, pdMS_TO_TICKS(20)) != pdTRUE)
        {
            // No sample available for this chunk window: output silence.
            memset(i2s_frames_lr, 0, sizeof(i2s_frames_lr));
        }
        else
        {
            mono_samples[0] = first;
            size_t n = 1;
            while (n < kOutputChunkSamples)
            {
                if (xQueueReceive(g_audio_sample_queue, &mono_samples[n], 0) != pdTRUE)
                {
                    break;
                }
                n++;
            }

            if (n < kOutputChunkSamples)
            {
                const int16_t hold = mono_samples[n - 1];
                while (n < kOutputChunkSamples)
                {
                    mono_samples[n++] = hold;
                }
            }

            // Content is mono, transport is stereo I2S: duplicate each sample on L and R.
            for (size_t i = 0; i < kOutputChunkSamples; ++i)
            {
                i2s_frames_lr[i * 2] = mono_samples[i];
                i2s_frames_lr[i * 2 + 1] = mono_samples[i];
            }
        }

        size_t bytes_written = 0;
        const size_t bytes_to_write = sizeof(i2s_frames_lr);
        esp_err_t err = i2s_write(I2S_NUM_1,
                                  i2s_frames_lr,
                                  bytes_to_write,
                                  &bytes_written,
                                  portMAX_DELAY);
        if (err == ESP_OK && bytes_written == bytes_to_write)
        {
        }
        else
        {
            (void)bytes_written;
        }
    }

    vTaskDelete(nullptr);
}

} // namespace

static bool initialized = false;

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
    esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    if (err != ESP_OK)
    {
        Serial.printf("Failed to install I2S driver: %d\r\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_1, &pin_config);
    if (err != ESP_OK)
    {
        Serial.printf("Failed to set I2S pins: %d\r\n", err);
        i2s_driver_uninstall(I2S_NUM_1);
        return false;
    }

    if (g_audio_sample_queue == nullptr)
    {
        g_audio_sample_queue = xQueueCreate(kAudioQueueDepth, sizeof(int16_t));
        if (g_audio_sample_queue == nullptr)
        {
            Serial.println("Failed to create I2S sample queue");
            i2s_driver_uninstall(I2S_NUM_1);
            return false;
        }
    }

    if (g_audio_writer_task == nullptr)
    {
        BaseType_t result = xTaskCreate(i2sAudioWriterTask,
                                        "i2sAudioWriter",
                                        4096,
                                        nullptr,
                                        6,
                                        &g_audio_writer_task);
        if (result != pdPASS)
        {
            Serial.println("Failed to create I2S writer task");
            vQueueDelete(g_audio_sample_queue);
            g_audio_sample_queue = nullptr;
            i2s_driver_uninstall(I2S_NUM_1);
            return false;
        }
    }

    initialized = true;

    Serial.printf("I2S initialized: %dHz stereo (mono via MAX98357), BCLK=%d, LRC=%d, DOUT=%d\r\n",
                  AUDIO_I2S_SAMPLE_RATE, GPIO_I2S_BCLK, GPIO_I2S_LRC, GPIO_I2S_DOUT);

    return true;
}


bool audio_i2s_setup_with_dma()
{
    return audio_i2s_setup();
}

esp_err_t audio_i2s_start_task()
{
    return initialized ? ESP_OK : ESP_FAIL;
}

bool audio_i2s_write_sample(int16_t sample)
{
    if (!initialized)
    {
        return false;
    }

    if (g_audio_sample_queue == nullptr)
    {
        return false;
    }

    if (xQueueSend(g_audio_sample_queue, &sample, 0) != pdTRUE)
    {
        return false;
    }
    return true;
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
        g_audio_writer_running = false;
        if (g_audio_writer_task != nullptr)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            if (eTaskGetState(g_audio_writer_task) != eDeleted)
            {
                vTaskDelete(g_audio_writer_task);
            }
            g_audio_writer_task = nullptr;
        }

        if (g_audio_sample_queue != nullptr)
        {
            vQueueDelete(g_audio_sample_queue);
            g_audio_sample_queue = nullptr;
        }

        i2s_driver_uninstall(I2S_NUM_1);
        initialized = false;
        Serial.println("I2S stopped");
    }
}
