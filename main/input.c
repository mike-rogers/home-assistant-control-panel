#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "ano_seesaw.h"
#include "input.h"
#include "ui_task.h"

static const char *TAG = "input";

#define POLL_MS 10
#define DEBOUNCE_POLLS 3     // consecutive polls a press must hold
#define PROBE_RETRY_MS 2000  // look again for an unplugged encoder

// Default ANO mapping (spec §6). No long-presses, no chords: every
// function is a single, visible press.
typedef struct {
    uint32_t bit;
    input_event_t event;
    int held;
} ano_button_t;

static ano_button_t s_buttons[] = {
    {.bit = ANO_BTN_SELECT, .event = INPUT_SELECT},
    {.bit = ANO_BTN_LEFT, .event = INPUT_BACK},
    {.bit = ANO_BTN_UP, .event = INPUT_NAV_PREV},
    {.bit = ANO_BTN_DOWN, .event = INPUT_NAV_NEXT},
    // ANO_BTN_RIGHT: unassigned.
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void poll_ano(void)
{
    // On a read error, skip this poll rather than inventing a release.
    uint32_t pressed;
    if (ano_seesaw_read_buttons(&pressed) == ESP_OK) {
        for (size_t i = 0; i < COUNT(s_buttons); i++) {
            ano_button_t *b = &s_buttons[i];
            if (pressed & b->bit) {
                if (++b->held == DEBOUNCE_POLLS) {
                    ui_task_input(b->event); // fire on press, once
                }
            } else {
                b->held = 0;
            }
        }
    }

    // Each detent is one step; clockwise moves down the list.
    int32_t delta;
    if (ano_seesaw_read_delta(&delta) == ESP_OK && delta != 0) {
        input_event_t evt = delta > 0 ? INPUT_NAV_NEXT : INPUT_NAV_PREV;
        for (int32_t n = delta > 0 ? delta : -delta; n > 0; n--) {
            ui_task_input(evt);
        }
    }
}

static void input_task(void *arg)
{
    bool have_ano = false;
    bool warned = false;
    for (;;) {
        if (!have_ano) {
            have_ano = ano_seesaw_init(CONFIG_KPANEL_PIN_I2C_SDA,
                                       CONFIG_KPANEL_PIN_I2C_SCL);
            if (!have_ano) {
                if (!warned) {
                    ui_task_status("Wheel not found - check its cable");
                    warned = true;
                }
                vTaskDelay(pdMS_TO_TICKS(PROBE_RETRY_MS));
                continue;
            }
            ESP_LOGI(TAG, "ANO wheel connected");
            if (warned) {
                ui_task_status("");
            }
        }
        poll_ano();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void input_init(void)
{
    xTaskCreate(input_task, "input", 3072, NULL, 7, NULL);
}
