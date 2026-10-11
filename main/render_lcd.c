// LVGL renderer for the ST7789 panel (Lonely Binary 2"/2.4", 240x320).
//
// Objects are created once; render_view() only changes what differs, so a
// wheel step invalidates a couple of rows instead of the whole frame (the
// SPI link needs ~30 ms for a full 320x240 redraw, most of the 50 ms
// input-to-screen budget).

#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "backlight.h"
#include "render.h"

// Native panel geometry (portrait); landscape is an LVGL rotation.
#define LCD_NATIVE_W 240
#define LCD_NATIVE_H 320
#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_DRAW_BUF_LINES 24 // modest: RAM is shared with Wi-Fi + BLE

#define HEADER_H 34
#define FOOTER_H 24
#define ROW_H 34
#define ROW_GAP 2
#define PAD 8
#define VALUE_W 96
#define MAX_ROWS 8 // portrait fits 7

// Palette
#define C_BG 0x101418
#define C_ROW 0x1C232B
#define C_SELECT 0x1E88E5
#define C_TEXT 0xECEFF1
#define C_MUTED 0x78909C
#define C_DISABLED 0x546E7A
#define C_OOPS 0xFFB300
#define C_OK 0x43A047
#define C_BAD 0xE53935

typedef struct {
    lv_obj_t *box;
    lv_obj_t *label;
    lv_obj_t *value;
} row_obj_t;

static int32_t s_w, s_h;
static int s_rows_visible;
static int s_first; // index of the first visible row

static lv_obj_t *s_title, *s_dot, *s_conn, *s_footer;
static lv_obj_t *s_menu_panel, *s_scrollbar;
static row_obj_t s_row[MAX_ROWS];
static lv_obj_t *s_edit_panel, *s_edit_label, *s_edit_value, *s_edit_hint;
static lv_obj_t *s_confirm_panel, *s_question, *s_btn_no, *s_btn_yes;
static lv_obj_t *s_message_panel, *s_message;
static lv_obj_t *s_toast, *s_toast_label;
static int s_dimmed = -1;

// ---- Small helpers that avoid needless invalidation ----

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_is_hidden(obj) != hidden) {
        lv_obj_set_hidden(obj, hidden);
    }
}

static void set_bg(lv_obj_t *obj, uint32_t hex)
{
    lv_color_t color = lv_color_hex(hex);
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color)) {
        lv_obj_set_style_bg_color(obj, color, 0);
    }
}

static void set_text_color(lv_obj_t *obj, uint32_t hex)
{
    lv_color_t color = lv_color_hex(hex);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color)) {
        lv_obj_set_style_text_color(obj, color, 0);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, s_w, s_h - HEADER_H - FOOTER_H);
    lv_obj_set_pos(panel, 0, HEADER_H);
    lv_obj_set_scrollable(panel, false);
    lv_obj_set_hidden(panel, true);
    return panel;
}

// ---- Panel bring-up (as taskpad, which runs this board and panel) ----

static void init_panel(esp_lcd_panel_io_handle_t *io,
                       esp_lcd_panel_handle_t *panel)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = CONFIG_CTLPANEL_PIN_SCLK,
        .mosi_io_num = CONFIG_CTLPANEL_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_NATIVE_H * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_CTLPANEL_PIN_DC,
        .cs_gpio_num = CONFIG_CTLPANEL_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_CTLPANEL_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(*io, &panel_cfg, panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*panel));
#if CONFIG_CTLPANEL_LCD_INVERT
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(*panel, true));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*panel, true));
}

// ---- Static layout ----

static void build_header(lv_obj_t *scr)
{
    s_title = make_label(scr, &lv_font_montserrat_16, C_TEXT);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_title, s_w - 2 * PAD - 90);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, PAD, (HEADER_H - 18) / 2);

    // Connectivity: a dot, plus words when it matters.
    s_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(C_BAD), 0);
    lv_obj_align(s_dot, LV_ALIGN_TOP_RIGHT, -PAD, (HEADER_H - 10) / 2);

    s_conn = make_label(scr, &lv_font_montserrat_14, C_BAD);
    lv_obj_align(s_conn, LV_ALIGN_TOP_RIGHT, -PAD - 16, (HEADER_H - 16) / 2);

    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, s_w, 1);
    lv_obj_set_pos(rule, 0, HEADER_H - 1);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(C_ROW), 0);
}

static void build_menu_panel(lv_obj_t *scr)
{
    s_menu_panel = make_panel(scr);
    int32_t body_h = s_h - HEADER_H - FOOTER_H;
    s_rows_visible = (body_h - ROW_GAP) / (ROW_H + ROW_GAP);
    if (s_rows_visible > MAX_ROWS) {
        s_rows_visible = MAX_ROWS;
    }

    for (int i = 0; i < s_rows_visible; i++) {
        row_obj_t *row = &s_row[i];
        row->box = lv_obj_create(s_menu_panel);
        lv_obj_remove_style_all(row->box);
        lv_obj_set_size(row->box, s_w - 2 * PAD, ROW_H);
        lv_obj_set_pos(row->box, PAD, ROW_GAP + i * (ROW_H + ROW_GAP));
        lv_obj_set_style_radius(row->box, 6, 0);
        lv_obj_set_style_bg_opa(row->box, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row->box, lv_color_hex(C_ROW), 0);
        lv_obj_set_style_border_color(row->box, lv_color_hex(C_OOPS), 0);
        lv_obj_set_style_border_width(row->box, 0, 0);
        lv_obj_set_scrollable(row->box, false);

        row->label = make_label(row->box, &lv_font_montserrat_20, C_TEXT);
        lv_label_set_long_mode(row->label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(row->label, LV_ALIGN_LEFT_MID, PAD, 0);

        row->value = make_label(row->box, &lv_font_montserrat_16, C_MUTED);
        lv_obj_align(row->value, LV_ALIGN_RIGHT_MID, -PAD, 0);
    }

    s_scrollbar = lv_obj_create(s_menu_panel);
    lv_obj_remove_style_all(s_scrollbar);
    lv_obj_set_style_bg_opa(s_scrollbar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_scrollbar, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_radius(s_scrollbar, 2, 0);
    lv_obj_set_hidden(s_scrollbar, true);
}

static void build_edit_panel(lv_obj_t *scr)
{
    s_edit_panel = make_panel(scr);
    s_edit_label = make_label(s_edit_panel, &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(s_edit_label, LV_ALIGN_TOP_MID, 0, 12);
    s_edit_value = make_label(s_edit_panel, &lv_font_montserrat_48, C_TEXT);
    lv_obj_align(s_edit_value, LV_ALIGN_CENTER, 0, 0);
    s_edit_hint = make_label(s_edit_panel, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(s_edit_hint, s_w - 2 * PAD);
    lv_obj_set_style_text_align(s_edit_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_edit_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int32_t x_offset)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 100, 44);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_ROW), 0);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x_offset, -16);
    lv_obj_t *label = make_label(btn, &lv_font_montserrat_20, C_TEXT);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void build_confirm_panel(lv_obj_t *scr)
{
    s_confirm_panel = make_panel(scr);
    s_question = make_label(s_confirm_panel, &lv_font_montserrat_28, C_TEXT);
    lv_label_set_long_mode(s_question, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_question, s_w - 2 * PAD);
    lv_obj_set_style_text_align(s_question, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_question, LV_ALIGN_TOP_MID, 0, 16);
    s_btn_no = make_button(s_confirm_panel, "No", -60);
    s_btn_yes = make_button(s_confirm_panel, "Yes", 60);
}

static void build_message_panel(lv_obj_t *scr)
{
    s_message_panel = make_panel(scr);
    s_message = make_label(s_message_panel, &lv_font_montserrat_20, C_TEXT);
    lv_label_set_long_mode(s_message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_message, s_w - 4 * PAD);
    lv_obj_set_style_text_align(s_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_message);
}

static void build_footer_and_toast(lv_obj_t *scr)
{
    s_footer = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_footer, s_w - 2 * PAD);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -(FOOTER_H - 16) / 2);

    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, s_w - 6 * PAD, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x263238), 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_all(s_toast, 12, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(FOOTER_H + 8));
    s_toast_label = make_label(s_toast, &lv_font_montserrat_20, C_TEXT);
    lv_label_set_long_mode(s_toast_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_toast_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(s_toast, true);
}

void render_init(void)
{
    backlight_init();

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    init_panel(&io, &panel);

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_NATIVE_W * LCD_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = LCD_NATIVE_W,
        .vres = LCD_NATIVE_H,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    lvgl_port_lock(0);
    // LVGL swaps the resolution and the port applies the panel's
    // swap/mirror for the chosen rotation.
#if CONFIG_CTLPANEL_LCD_LANDSCAPE && CONFIG_CTLPANEL_LCD_ROTATE_180
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
#elif CONFIG_CTLPANEL_LCD_LANDSCAPE
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
#elif CONFIG_CTLPANEL_LCD_ROTATE_180
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
#endif
    s_w = lv_display_get_horizontal_resolution(disp);
    s_h = lv_display_get_vertical_resolution(disp);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_scrollable(scr, false);
    build_header(scr);
    build_menu_panel(scr);
    build_edit_panel(scr);
    build_confirm_panel(scr);
    build_message_panel(scr);
    build_footer_and_toast(scr);
    lvgl_port_unlock();
}

// ---- Per-view updates ----

static void update_header(const ui_view_t *view)
{
    set_text(s_title, view->title);
    uint32_t color = !view->online ? C_BAD : view->pending ? C_OOPS : C_OK;
    set_bg(s_dot, color);
    set_text(s_conn, !view->online ? "Offline" : view->pending ? "Sending" : "");
    set_text_color(s_conn, color);
}

static void update_row(row_obj_t *row, const ui_row_t *src, bool selected)
{
    bool oops = src->kind == UI_ROW_OOPS;
    const char *value = src->value[0]      ? src->value
                        : src->kind == UI_ROW_SUBMENU ? ">"
                                                      : "";
    set_text(row->label, src->label);
    set_text(row->value, value);
    lv_obj_set_width(row->label, s_w - 4 * PAD - (value[0] ? VALUE_W : 0));

    uint32_t bg = selected ? (oops ? C_OOPS : C_SELECT) : C_ROW;
    uint32_t fg = src->kind == UI_ROW_DISABLED ? C_DISABLED
                  : selected && oops          ? C_BG
                  : oops                      ? C_OOPS
                                              : C_TEXT;
    set_bg(row->box, bg);
    set_text_color(row->label, fg);
    set_text_color(row->value, selected ? fg : (oops ? C_OOPS : C_MUTED));
    // Unselected Oops rows get an amber outline so they read as special.
    int32_t border = oops && !selected ? 2 : 0;
    if (lv_obj_get_style_border_width(row->box, LV_PART_MAIN) != border) {
        lv_obj_set_style_border_width(row->box, border, 0);
    }
}

static void update_menu(const ui_view_t *view)
{
    // Scroll just enough to keep the selection visible.
    if (view->selected < s_first) {
        s_first = view->selected;
    } else if (view->selected >= s_first + s_rows_visible) {
        s_first = view->selected - s_rows_visible + 1;
    }
    int max_first = view->row_count > s_rows_visible
                        ? view->row_count - s_rows_visible
                        : 0;
    if (s_first > max_first) {
        s_first = max_first;
    }

    for (int i = 0; i < s_rows_visible; i++) {
        int idx = s_first + i;
        bool shown = idx < view->row_count;
        set_hidden(s_row[i].box, !shown);
        if (shown) {
            update_row(&s_row[i], &view->rows[idx], idx == view->selected);
        }
    }

    bool scrolls = view->row_count > s_rows_visible;
    set_hidden(s_scrollbar, !scrolls);
    if (scrolls) {
        int32_t track = s_rows_visible * (ROW_H + ROW_GAP);
        int32_t len = track * s_rows_visible / view->row_count;
        int32_t pos = ROW_GAP + track * s_first / view->row_count;
        lv_obj_set_size(s_scrollbar, 3, len);
        lv_obj_set_pos(s_scrollbar, s_w - 4, pos);
    }
}

static const char *footer_hint(const ui_view_t *view)
{
    switch (view->screen) {
    case UI_SCREEN_MENU:
        return strchr(view->title, '>') != NULL
                   ? "Turn to choose, press to select, left to go back"
                   : "Turn to choose, press to select";
    case UI_SCREEN_CONFIRM:
        return "Turn to choose, press to confirm";
    default:
        return "";
    }
}

void render_view(const ui_view_t *view, const char *status)
{
    lvgl_port_lock(0);

    update_header(view);
    set_hidden(s_menu_panel, view->screen != UI_SCREEN_MENU);
    set_hidden(s_edit_panel, view->screen != UI_SCREEN_EDIT_TIME);
    set_hidden(s_confirm_panel, view->screen != UI_SCREEN_CONFIRM);
    set_hidden(s_message_panel, view->screen != UI_SCREEN_MESSAGE);

    switch (view->screen) {
    case UI_SCREEN_MENU:
        update_menu(view);
        break;
    case UI_SCREEN_EDIT_TIME:
        set_text(s_edit_label, view->edit_label);
        set_text(s_edit_value, view->edit_value);
        set_text(s_edit_hint, view->edit_hint ? view->edit_hint : "");
        break;
    case UI_SCREEN_CONFIRM:
        set_text(s_question, view->question);
        set_bg(s_btn_no, view->confirm_yes ? C_ROW : C_SELECT);
        set_bg(s_btn_yes, view->confirm_yes ? C_SELECT : C_ROW);
        break;
    case UI_SCREEN_MESSAGE:
        set_text(s_message, view->message);
        break;
    }

    // System status (Wi-Fi setup, updates) outranks the usage hint.
    set_text(s_footer, status[0] ? status : footer_hint(view));

    set_hidden(s_toast, !view->has_toast);
    if (view->has_toast) {
        set_text(s_toast_label, view->toast);
    }

    lvgl_port_unlock();

    if (s_dimmed != view->dimmed) {
        s_dimmed = view->dimmed;
        backlight_set(view->dimmed ? CONFIG_CTLPANEL_BACKLIGHT_DIM_PERCENT : 100);
    }
}
