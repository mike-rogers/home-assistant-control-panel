#pragma once

#include <stdint.h>

// Backlight control: the power abstraction (spec §2). A future battery
// option would hang its policy here rather than in the UI.

void backlight_init(void);

// 0..100. No-op when the backlight pin isn't wired (tied high).
void backlight_set(uint8_t percent);
