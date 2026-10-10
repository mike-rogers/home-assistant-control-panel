#pragma once

#include "ui_sm.h"

// Draws a view model. Called only from the UI task, and only when the view
// or the status text changed.

void render_init(void);

// status: system text for the status bar ("" = none).
void render_view(const ui_view_t *view, const char *status);
