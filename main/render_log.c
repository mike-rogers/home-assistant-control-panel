// Serial renderer: logs each view. Stands in for the LCD until M3, and
// stays available (CONFIG_CTLPANEL_RENDER_LOG) for debugging without a panel.

#include <stdio.h>

#include "esp_log.h"

#include "render.h"

static const char *TAG = "view";

void render_init(void)
{
}

void render_view(const ui_view_t *view, const char *status)
{
    ESP_LOGI(TAG, "---- %s [%s%s%s] %s", view->title,
             view->online ? "online" : "OFFLINE",
             view->pending ? ", waiting" : "", view->dimmed ? ", dim" : "",
             status);
    switch (view->screen) {
    case UI_SCREEN_MENU:
        for (int i = 0; i < view->row_count; i++) {
            const ui_row_t *row = &view->rows[i];
            ESP_LOGI(TAG, "%s %s%s%s%s", i == view->selected ? ">" : " ",
                     row->kind == UI_ROW_OOPS ? "<- " : "", row->label,
                     row->kind == UI_ROW_SUBMENU ? " ..." : "",
                     row->kind == UI_ROW_DISABLED ? " (n/a)" : "");
            if (row->value[0]) {
                ESP_LOGI(TAG, "      %s", row->value);
            }
        }
        break;
    case UI_SCREEN_EDIT_TIME:
        ESP_LOGI(TAG, "  %s: [ %s ]  (%s)", view->edit_label,
                 view->edit_value, view->edit_hint);
        break;
    case UI_SCREEN_CONFIRM:
        ESP_LOGI(TAG, "  %s   %s No %s   %s Yes %s", view->question,
                 view->confirm_yes ? " " : "[", view->confirm_yes ? " " : "]",
                 view->confirm_yes ? "[" : " ", view->confirm_yes ? "]" : " ");
        break;
    case UI_SCREEN_MESSAGE:
        ESP_LOGI(TAG, "  %s", view->message);
        break;
    }
    if (view->has_toast) {
        ESP_LOGI(TAG, "  ** %s **", view->toast);
    }
}
