#include <string.h>

#include "unity.h"

#include "fixtures.h"
#include "ui_sm.h"

static ui_sm_t s_sm;
static entity_store_t s_store;
static menu_t s_menu;
static ui_view_t s_view;
static request_t s_req;
static uint32_t s_now;

// ---- Helpers ----

static void setup_sm(void)
{
    ui_sm_config_t cfg = UI_SM_CONFIG_DEFAULT;
    entity_store_init(&s_store);
    s_now = 1000;
    ui_sm_init(&s_sm, &cfg, &s_store, 0xabcd, s_now);
    TEST_ASSERT_EQUAL(MENU_OK, menu_parse(SPEC_MENU, &s_menu, NULL, 0));
    ui_sm_set_menu(&s_sm, &s_menu, s_now);
    ui_sm_set_online(&s_sm, true, s_now);
}

// Advance the clock in small ticks, like the UI task would.
static void advance(uint32_t ms)
{
    while (ms > 0) {
        uint32_t step = ms < 100 ? ms : 100;
        s_now += step;
        ms -= step;
        ui_sm_tick(&s_sm, s_now);
    }
}

static bool press(input_event_t evt)
{
    memset(&s_req, 0, sizeof(s_req));
    return ui_sm_input(&s_sm, evt, s_now, &s_req);
}

static void press_none(input_event_t evt)
{
    TEST_ASSERT_FALSE_MESSAGE(press(evt), "unexpected request");
}

static const ui_view_t *view(void)
{
    ui_sm_view(&s_sm, s_now, &s_view);
    return &s_view;
}

static const char *selected_label(void)
{
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, s_view.screen);
    return s_view.rows[s_view.selected].label;
}

static void reply(bool ok, const char *message, int undo_seconds)
{
    result_t res = {.ok = ok, .undo_seconds = undo_seconds};
    strlcpy(res.request_id, s_sm.pending_req.request_id,
            sizeof(res.request_id));
    strlcpy(res.message, message, sizeof(res.message));
    ui_sm_on_result(&s_sm, &res, s_now);
}

static void set_wake(const char *state)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"state\":\"%s\"}", state);
    TEST_ASSERT_TRUE(entity_store_update(&s_store, WAKE_ENTITY, json));
}

// Run Good night (root row 1) and have HA acknowledge it as undoable.
static void run_goodnight(void)
{
    press(INPUT_HOME);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    reply(true, "", 30);
}

// ---- Startup ----

static void test_waits_for_menu(void)
{
    ui_sm_config_t cfg = UI_SM_CONFIG_DEFAULT;
    entity_store_init(&s_store);
    ui_sm_init(&s_sm, &cfg, &s_store, 1, 0);
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_MESSAGE, s_view.screen);
    TEST_ASSERT_NOT_NULL(strstr(s_view.message, "Waiting"));
    press_none(INPUT_SELECT);

    ui_sm_menu_rejected(&s_sm, "menu schema 2 not supported (need 1)", 0);
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_MESSAGE, s_view.screen);
    TEST_ASSERT_NOT_NULL(strstr(s_view.message, "schema 2"));
}

static void test_first_menu_shows_root(void)
{
    setup_sm();
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, s_view.screen);
    TEST_ASSERT_EQUAL_STRING("Kitchen", s_view.title);
    TEST_ASSERT_EQUAL(3, s_view.row_count);
    TEST_ASSERT_EQUAL(0, s_view.selected);
    TEST_ASSERT_EQUAL(UI_ROW_SUBMENU, s_view.rows[0].kind);
    TEST_ASSERT_EQUAL(UI_ROW_ITEM, s_view.rows[1].kind);
}

// ---- Navigation ----

static void test_navigation_clamps_and_descends(void)
{
    setup_sm();
    press_none(INPUT_NAV_PREV);
    TEST_ASSERT_EQUAL_STRING("Wake-up", selected_label());
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL_STRING("Automations", selected_label());

    press_none(INPUT_SELECT);
    view();
    TEST_ASSERT_EQUAL_STRING("Kitchen > Automations", s_view.title);
    TEST_ASSERT_EQUAL_STRING("Brighten living room", selected_label());

    press_none(INPUT_BACK);
    TEST_ASSERT_EQUAL_STRING("Automations", selected_label()); // remembered
    press_none(INPUT_BACK); // no-op at root
    TEST_ASSERT_EQUAL_STRING("Kitchen", view()->title);

    press_none(INPUT_SELECT);
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_HOME);
    TEST_ASSERT_EQUAL_STRING("Kitchen", view()->title);
    TEST_ASSERT_EQUAL_STRING("Wake-up", selected_label());
}

static void test_unknown_item_is_greyed_and_inert(void)
{
    setup_sm();
    TEST_ASSERT_EQUAL(MENU_OK, menu_parse(
        "{\"schema\":1,\"items\":[{\"id\":\"x\",\"label\":\"Slider\","
        "\"type\":\"slider\"}]}", &s_menu, NULL, 0));
    ui_sm_set_menu(&s_sm, &s_menu, s_now);
    TEST_ASSERT_EQUAL(UI_ROW_DISABLED, view()->rows[0].kind);
    press_none(INPUT_SELECT);
    TEST_ASSERT_TRUE(view()->has_toast);
    TEST_ASSERT_NOT_NULL(strstr(s_view.toast, "firmware"));
}

// ---- Actions, results, Oops ----

static void test_action_round_trip_offers_oops(void)
{
    setup_sm();
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    TEST_ASSERT_EQUAL(OP_RUN, s_req.op);
    TEST_ASSERT_EQUAL_STRING("goodnight", s_req.item_id);
    TEST_ASSERT_EQUAL_STRING("0000abcd-1", s_req.request_id);
    TEST_ASSERT_TRUE(view()->pending);

    reply(true, "", 30);
    view();
    TEST_ASSERT_FALSE(s_view.pending);
    TEST_ASSERT_TRUE(s_view.has_toast);
    TEST_ASSERT_EQUAL_STRING("Done", s_view.toast);
    TEST_ASSERT_EQUAL(4, s_view.row_count);
    TEST_ASSERT_EQUAL(UI_ROW_OOPS, s_view.rows[3].kind);
    TEST_ASSERT_EQUAL_STRING("Oops: undo Good night", s_view.rows[3].label);
    TEST_ASSERT_EQUAL_STRING("30s", s_view.rows[3].value);
    TEST_ASSERT_EQUAL(1, s_view.selected); // the cursor did not jump

    advance(2100);
    TEST_ASSERT_FALSE(view()->has_toast);
    TEST_ASSERT_EQUAL_STRING("28s", s_view.rows[3].value);
}

static void test_oops_row_sends_undo(void)
{
    setup_sm();
    run_goodnight();
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL_STRING("Oops: undo Good night", selected_label());
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    TEST_ASSERT_EQUAL(OP_UNDO, s_req.op);
    TEST_ASSERT_EQUAL_STRING("goodnight", s_req.item_id);

    reply(true, "", 0);
    view();
    TEST_ASSERT_EQUAL_STRING("Undone", s_view.toast);
    TEST_ASSERT_EQUAL(3, s_view.row_count);
    TEST_ASSERT_EQUAL(2, s_view.selected); // clamped off the vanished row
}

static void test_oops_follows_into_submenus_and_expires(void)
{
    setup_sm();
    run_goodnight();
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT); // onto the Oops row at root
    press(INPUT_HOME);
    press_none(INPUT_SELECT); // into Wake-up
    view();
    TEST_ASSERT_EQUAL(3, s_view.row_count);
    TEST_ASSERT_EQUAL(UI_ROW_OOPS, s_view.rows[2].kind);
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL(2, view()->selected);

    // Stay active (the window and the idle return-home are both 30 s).
    advance(20000);
    press_none(INPUT_NAV_NEXT); // already on the last row: stays put
    advance(9000);
    TEST_ASSERT_EQUAL_STRING("1s", view()->rows[2].value);
    advance(1000);
    view();
    TEST_ASSERT_EQUAL(2, s_view.row_count);
    TEST_ASSERT_EQUAL(1, s_view.selected);

    press_none(INPUT_UNDO);
    TEST_ASSERT_EQUAL_STRING("Nothing to undo", view()->toast);
}

static void test_undo_event_works_while_oops_live(void)
{
    setup_sm();
    run_goodnight();
    TEST_ASSERT_TRUE(press(INPUT_UNDO));
    TEST_ASSERT_EQUAL(OP_UNDO, s_req.op);
}

static void test_new_operation_replaces_oops(void)
{
    setup_sm();
    run_goodnight();
    // A non-undoable result clears the previous Oops: HA keeps one snapshot.
    press(INPUT_HOME);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    reply(true, "", 0);
    TEST_ASSERT_EQUAL(3, view()->row_count);
}

static void test_error_result_shows_message(void)
{
    setup_sm();
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    reply(false, "script.good_night not found", 0);
    TEST_ASSERT_EQUAL_STRING("script.good_night not found", view()->toast);

    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    reply(false, "", 0);
    TEST_ASSERT_EQUAL_STRING("Home Assistant reported an error",
                             view()->toast);
}

static void test_mismatched_result_is_ignored(void)
{
    setup_sm();
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    result_t stale = {.request_id = "deadbeef-9", .ok = true,
                      .undo_seconds = 30};
    ui_sm_on_result(&s_sm, &stale, s_now);
    TEST_ASSERT_TRUE(view()->pending);
    TEST_ASSERT_EQUAL(3, s_view.row_count);
}

static void test_one_request_at_a_time(void)
{
    setup_sm();
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    press_none(INPUT_SELECT);
    TEST_ASSERT_EQUAL_STRING("Still waiting for Home Assistant",
                             view()->toast);
    // Navigation stays live while waiting.
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL_STRING("Automations", selected_label());
}

static void test_request_timeout(void)
{
    setup_sm();
    run_goodnight();
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    advance(4900);
    TEST_ASSERT_TRUE(view()->pending);
    advance(100);
    view();
    TEST_ASSERT_FALSE(s_view.pending);
    TEST_ASSERT_EQUAL_STRING("No response from Home Assistant", s_view.toast);
    TEST_ASSERT_EQUAL(3, s_view.row_count); // unknown outcome: no Oops
}

static void test_offline_refuses_without_queueing(void)
{
    setup_sm();
    ui_sm_set_online(&s_sm, false, s_now);
    TEST_ASSERT_FALSE(view()->online);
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_SELECT);
    TEST_ASSERT_EQUAL_STRING("Offline - not sent", view()->toast);
    TEST_ASSERT_FALSE(s_view.pending);

    ui_sm_set_online(&s_sm, true, s_now);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    ui_sm_set_online(&s_sm, false, s_now);
    view();
    TEST_ASSERT_FALSE(s_view.pending);
    TEST_ASSERT_EQUAL_STRING("Connection lost", s_view.toast);
}

// ---- Confirm ----

static void test_confirm_defaults_to_no(void)
{
    setup_sm();
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_SELECT);
    press_none(INPUT_NAV_NEXT); // Summon the kids
    press_none(INPUT_SELECT);
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_CONFIRM, s_view.screen);
    TEST_ASSERT_EQUAL_STRING("Summon the kids?", s_view.question);
    TEST_ASSERT_FALSE(s_view.confirm_yes);

    press_none(INPUT_SELECT); // stray press on No does nothing
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, view()->screen);
    TEST_ASSERT_FALSE(s_view.pending);

    press_none(INPUT_SELECT);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_TRUE(view()->confirm_yes);
    press_none(INPUT_BACK);
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, view()->screen);

    press_none(INPUT_SELECT);
    TEST_ASSERT_FALSE(view()->confirm_yes); // reset to No on reopen
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    TEST_ASSERT_EQUAL_STRING("summon_kids", s_req.item_id);
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, view()->screen);
}

// ---- Time edit ----

static void open_wake_time(void)
{
    press(INPUT_HOME);
    press_none(INPUT_SELECT); // Wake-up
    press_none(INPUT_SELECT); // Tomorrow's wake-up
}

static void test_time_preview_and_formats(void)
{
    setup_sm();
    press_none(INPUT_SELECT);
    TEST_ASSERT_EQUAL_STRING("--", view()->rows[0].value);
    set_wake("06:45:00");
    TEST_ASSERT_EQUAL_STRING("6:45 AM", view()->rows[0].value);

    char buf[16];
    ui_format_time(0, false, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("12:00 AM", buf);
    ui_format_time(12 * 60 + 5, false, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("12:05 PM", buf);
    ui_format_time(18 * 60 + 30, true, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("18:30", buf);

    TEST_ASSERT_EQUAL(6 * 60 + 45, ui_parse_time("2026-10-11 06:45:00"));
    TEST_ASSERT_EQUAL(-1, ui_parse_time("unknown"));
    TEST_ASSERT_EQUAL(-1, ui_parse_time("25:00:00"));
}

static void test_time_edit_commit(void)
{
    setup_sm();
    set_wake("06:45:00");
    open_wake_time();
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_EDIT_TIME, s_view.screen);
    TEST_ASSERT_EQUAL_STRING("6:45 AM", s_view.edit_value);
    TEST_ASSERT_EQUAL_STRING("Tomorrow's wake-up", s_view.edit_label);
    TEST_ASSERT_NOT_NULL(s_view.edit_hint);

    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL_STRING("7:15 AM", view()->edit_value);
    TEST_ASSERT_TRUE(press(INPUT_SELECT));
    TEST_ASSERT_EQUAL(OP_SET, s_req.op);
    TEST_ASSERT_EQUAL_STRING("wake_time", s_req.item_id);
    TEST_ASSERT_EQUAL_STRING("07:15", s_req.value);
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, view()->screen);

    reply(true, "", 30);
    TEST_ASSERT_EQUAL_STRING("Oops: undo Tomorrow's wake-up",
                             view()->rows[2].label);
}

static void test_time_edit_snaps_and_wraps(void)
{
    setup_sm();
    set_wake("06:50:00");
    open_wake_time();
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL_STRING("7:00 AM", view()->edit_value);
    press_none(INPUT_BACK);

    open_wake_time();
    press_none(INPUT_NAV_PREV);
    TEST_ASSERT_EQUAL_STRING("6:45 AM", view()->edit_value);
    press_none(INPUT_BACK);

    set_wake("00:00:00");
    open_wake_time();
    press_none(INPUT_NAV_PREV);
    TEST_ASSERT_EQUAL_STRING("11:45 PM", view()->edit_value);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL_STRING("12:00 AM", view()->edit_value);
}

static void test_time_edit_cancel_and_unchanged(void)
{
    setup_sm();
    set_wake("06:45:00");
    open_wake_time();
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_BACK);
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, view()->screen);
    TEST_ASSERT_FALSE(s_view.pending);

    open_wake_time();
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_PREV);
    press_none(INPUT_SELECT); // back where it started: nothing to send
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, view()->screen);
    TEST_ASSERT_FALSE(s_view.pending);
}

static void test_time_edit_needs_a_value(void)
{
    setup_sm();
    open_wake_time();
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, s_view.screen);
    TEST_ASSERT_EQUAL_STRING("No value from Home Assistant yet", s_view.toast);
}

// ---- Idle behavior ----

static void test_idle_goes_home_then_dims(void)
{
    setup_sm();
    set_wake("06:45:00");
    open_wake_time();
    advance(29900);
    TEST_ASSERT_EQUAL(UI_SCREEN_EDIT_TIME, view()->screen);
    advance(100);
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, s_view.screen);
    TEST_ASSERT_EQUAL_STRING("Kitchen", s_view.title);
    TEST_ASSERT_FALSE(s_view.dimmed);

    press_none(INPUT_NAV_NEXT); // user moves after going home...
    advance(30000);
    TEST_ASSERT_EQUAL(0, view()->selected); // ...idle again: home resets
    advance(30000);
    TEST_ASSERT_TRUE(view()->dimmed);

    // The waking input is swallowed.
    press_none(INPUT_NAV_NEXT);
    view();
    TEST_ASSERT_FALSE(s_view.dimmed);
    TEST_ASSERT_EQUAL(0, s_view.selected);
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL(1, view()->selected);

    advance(60000);
    TEST_ASSERT_TRUE(view()->dimmed);
    press_none(INPUT_SELECT); // a press while dimmed doesn't run anything
    TEST_ASSERT_FALSE(view()->pending);
}

// ---- Menu replacement ----

static void test_menu_update_keeps_place(void)
{
    setup_sm();
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_NAV_NEXT);
    press_none(INPUT_SELECT); // Automations
    press_none(INPUT_NAV_NEXT); // Summon the kids

    // HA inserts an item at the top of both lists.
    TEST_ASSERT_EQUAL(MENU_OK, menu_parse(
        "{\"schema\":1,\"title\":\"Kitchen\",\"items\":["
        "{\"id\":\"new\",\"label\":\"New\",\"type\":\"action\"},"
        "{\"id\":\"automations\",\"label\":\"Automations\","
        "\"type\":\"submenu\",\"items\":["
        "{\"id\":\"new2\",\"label\":\"New 2\",\"type\":\"action\"},"
        "{\"id\":\"summon_kids\",\"label\":\"Summon the kids\","
        "\"type\":\"action\"}]}]}", &s_menu, NULL, 0));
    ui_sm_set_menu(&s_sm, &s_menu, s_now);
    TEST_ASSERT_EQUAL_STRING("Kitchen > Automations", view()->title);
    TEST_ASSERT_EQUAL_STRING("Summon the kids", selected_label());
    press_none(INPUT_BACK);
    TEST_ASSERT_EQUAL_STRING("Automations", selected_label());
}

static void test_menu_update_removing_open_submenu(void)
{
    setup_sm();
    set_wake("06:45:00");
    open_wake_time();
    TEST_ASSERT_EQUAL(MENU_OK, menu_parse(
        "{\"schema\":1,\"items\":[{\"id\":\"goodnight\",\"label\":"
        "\"Good night\",\"type\":\"action\"}]}", &s_menu, NULL, 0));
    ui_sm_set_menu(&s_sm, &s_menu, s_now);
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, s_view.screen);
    TEST_ASSERT_EQUAL_STRING("Menu", s_view.title);
    TEST_ASSERT_EQUAL(1, s_view.row_count);
    TEST_ASSERT_EQUAL(0, s_view.selected);
}

static void test_menu_update_keeps_edit_when_item_survives(void)
{
    setup_sm();
    set_wake("06:45:00");
    open_wake_time();
    press_none(INPUT_NAV_NEXT);
    TEST_ASSERT_EQUAL(MENU_OK, menu_parse(SPEC_MENU, &s_menu, NULL, 0));
    ui_sm_set_menu(&s_sm, &s_menu, s_now);
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_EDIT_TIME, s_view.screen);
    TEST_ASSERT_EQUAL_STRING("7:00 AM", s_view.edit_value);
}

static void test_rejected_update_keeps_menu(void)
{
    setup_sm();
    ui_sm_menu_rejected(&s_sm, "schema 2", s_now);
    view();
    TEST_ASSERT_EQUAL(UI_SCREEN_MENU, s_view.screen);
    TEST_ASSERT_EQUAL(3, s_view.row_count);
    TEST_ASSERT_EQUAL_STRING("Menu rejected: schema 2", s_view.toast);
}

void run_ui_sm_tests(void)
{
    RUN_TEST(test_waits_for_menu);
    RUN_TEST(test_first_menu_shows_root);
    RUN_TEST(test_navigation_clamps_and_descends);
    RUN_TEST(test_unknown_item_is_greyed_and_inert);
    RUN_TEST(test_action_round_trip_offers_oops);
    RUN_TEST(test_oops_row_sends_undo);
    RUN_TEST(test_oops_follows_into_submenus_and_expires);
    RUN_TEST(test_undo_event_works_while_oops_live);
    RUN_TEST(test_new_operation_replaces_oops);
    RUN_TEST(test_error_result_shows_message);
    RUN_TEST(test_mismatched_result_is_ignored);
    RUN_TEST(test_one_request_at_a_time);
    RUN_TEST(test_request_timeout);
    RUN_TEST(test_offline_refuses_without_queueing);
    RUN_TEST(test_confirm_defaults_to_no);
    RUN_TEST(test_time_preview_and_formats);
    RUN_TEST(test_time_edit_commit);
    RUN_TEST(test_time_edit_snaps_and_wraps);
    RUN_TEST(test_time_edit_cancel_and_unchanged);
    RUN_TEST(test_time_edit_needs_a_value);
    RUN_TEST(test_idle_goes_home_then_dims);
    RUN_TEST(test_menu_update_keeps_place);
    RUN_TEST(test_menu_update_removing_open_submenu);
    RUN_TEST(test_menu_update_keeps_edit_when_item_survives);
    RUN_TEST(test_rejected_update_keeps_menu);
}
