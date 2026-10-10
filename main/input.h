#pragma once

// Input drivers: turn hardware into logical events (spec §6) for the UI
// task. Today that's the Adafruit ANO wheel; new drivers post the same
// input_event_t values and the UI never knows the difference.
void input_init(void);
