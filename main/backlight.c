#include "driver/ledc.h"
#include "esp_log.h"

#include "backlight.h"

static const char *TAG = "backlight";

#define LEDC_RES LEDC_TIMER_10_BIT
#define LEDC_MAX ((1 << 10) - 1)

#if CONFIG_KPANEL_PIN_BACKLIGHT >= 0
static bool s_ready;
#endif

void backlight_init(void)
{
#if CONFIG_KPANEL_PIN_BACKLIGHT >= 0
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = CONFIG_KPANEL_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = LEDC_MAX, // full on until told otherwise
    };
    if (ledc_timer_config(&timer) != ESP_OK ||
        ledc_channel_config(&channel) != ESP_OK) {
        ESP_LOGE(TAG, "backlight PWM setup failed; leaving it as is");
        return;
    }
    s_ready = true;
#endif
}

void backlight_set(uint8_t percent)
{
#if CONFIG_KPANEL_PIN_BACKLIGHT >= 0
    if (!s_ready) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                  (uint32_t)LEDC_MAX * percent / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    (void)percent;
#endif
}
