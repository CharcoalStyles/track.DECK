#include <stdio.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "port_power.h"
#include "port_display.h"
#include "port_codec.h"
#include "epaper_config.h"

static const char *TAG = "smoke_test";

static void draw_test_pattern(void) {
    for (int y = 0; y < EPD_HEIGHT; y++) {
        for (int x = 0; x < EPD_WIDTH; x++) {
            bool black = ((x / 20) + (y / 20)) % 2 == 0;
            EPD_DrawColorPixel(x, y, black ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE);
        }
    }
}

static void play_test_tone(void) {
    BoardPower_Audio_ON();
    Codec_StartInit();

    const int sample_rate = 16000;
    const int channels = 2;
    const float freq = 440.0f;
    const int chunk_samples = 320; // 20ms per channel
    int16_t buf[chunk_samples * channels];

    float phase = 0.0f;
    const float phase_step = 2.0f * (float)M_PI * freq / sample_rate;

    const int chunks = 50; // ~1s total
    for (int c = 0; c < chunks; c++) {
        for (int i = 0; i < chunk_samples; i++) {
            int16_t sample = (int16_t)(3000.0f * sinf(phase));
            phase += phase_step;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
            buf[i * channels + 0] = sample;
            buf[i * channels + 1] = sample;
        }
        Codec_PlaybackData((uint8_t *)buf, sizeof(buf));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Board bring-up smoke test: e-ink + audio");

    BoardPower_Init();
    BoardPower_VBAT_ON();
    BoardPower_EPD_ON();
    vTaskDelay(pdMS_TO_TICKS(100));

    PortDisplay_Init();
    EPD_Init();
    EPD_Clear();
    draw_test_pattern();
    EPD_Display();
    ESP_LOGI(TAG, "E-ink checkerboard pattern displayed (full refresh)");

    play_test_tone();
    ESP_LOGI(TAG, "440Hz tone playback complete");

    ESP_LOGI(TAG, "Smoke test done");
}
