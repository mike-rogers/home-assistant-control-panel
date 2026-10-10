#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Adafruit ANO rotary navigation encoder on its I2C seesaw adapter
// (product 5740, ATtiny817, default address 0x49).

// Bits in the mask returned by ano_seesaw_read_buttons(); set = pressed.
#define ANO_BTN_SELECT (1u << 1)
#define ANO_BTN_UP     (1u << 2)
#define ANO_BTN_LEFT   (1u << 3)
#define ANO_BTN_DOWN   (1u << 4)
#define ANO_BTN_RIGHT  (1u << 5)

// Brings up the I2C bus and configures the board. Returns false, with the
// bus released, if no ANO adapter answers; safe to call again to retry.
bool ano_seesaw_init(int sda_pin, int scl_pin);

esp_err_t ano_seesaw_read_buttons(uint32_t *pressed);

// Detents turned since the last call; positive = clockwise.
esp_err_t ano_seesaw_read_delta(int32_t *delta);
