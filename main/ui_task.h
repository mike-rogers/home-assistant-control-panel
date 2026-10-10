#pragma once

#include <stdbool.h>

#include "ui_sm.h"

// The UI task owns panel_core's state machine, entity store and menu, and
// is the only caller of the renderer. Everything else talks to it through
// these posting functions, which copy their arguments and never block.

// Start the task; shows the menu cached in NVS (if any) right away.
void ui_task_start(void);

// A logical input event (input drivers).
void ui_task_input(input_event_t evt);

// MQTT session up/down.
void ui_task_online(bool online);

// Incoming payloads (MQTT). Ownership of the heap buffers passes to the UI
// task, which frees them; on a full queue they are freed here.
void ui_task_menu(char *json);
void ui_task_state(char *entity_id, char *json);
void ui_task_result(char *json);

// System status text for the status bar ("Connecting to Wi-Fi...");
// "" clears it.
void ui_task_status(const char *text);
