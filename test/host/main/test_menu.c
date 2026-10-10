#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fixtures.h"
#include "menu.h"

static menu_t s_menu;
static char s_err[96];

static menu_status_t parse(const char *json)
{
    return menu_parse(json, &s_menu, s_err, sizeof(s_err));
}

static void test_spec_menu_parses_with_contiguous_lists(void)
{
    TEST_ASSERT_EQUAL(MENU_OK, parse(SPEC_MENU));
    TEST_ASSERT_EQUAL_STRING("Kitchen", s_menu.title);
    TEST_ASSERT_EQUAL(3, s_menu.root_count);
    TEST_ASSERT_EQUAL(7, s_menu.count);

    // Root list is items[0..2] in document order.
    TEST_ASSERT_EQUAL_STRING("wake", s_menu.items[0].id);
    TEST_ASSERT_EQUAL_STRING("goodnight", s_menu.items[1].id);
    TEST_ASSERT_EQUAL_STRING("automations", s_menu.items[2].id);

    const menu_item_t *wake = &s_menu.items[0];
    TEST_ASSERT_EQUAL(MENU_ITEM_SUBMENU, wake->type);
    TEST_ASSERT_EQUAL(2, wake->child_count);
    const menu_item_t *time = &s_menu.items[wake->first_child];
    TEST_ASSERT_EQUAL_STRING("wake_time", time->id);
    TEST_ASSERT_EQUAL(MENU_ITEM_TIME, time->type);
    TEST_ASSERT_EQUAL_STRING(WAKE_ENTITY, time->entity);
    TEST_ASSERT_EQUAL(15, time->step_minutes);
    TEST_ASSERT_EQUAL_STRING("wake_snow_day",
                             s_menu.items[wake->first_child + 1].id);

    const menu_item_t *autos = &s_menu.items[2];
    TEST_ASSERT_EQUAL_STRING("summon_kids",
                             s_menu.items[autos->first_child + 1].id);
}

static void test_confirm_defaults_to_not_undo(void)
{
    TEST_ASSERT_EQUAL(MENU_OK, parse(
        "{\"schema\":1,\"items\":["
        "{\"id\":\"a\",\"label\":\"A\",\"type\":\"action\"},"
        "{\"id\":\"b\",\"label\":\"B\",\"type\":\"action\",\"undo\":true},"
        "{\"id\":\"c\",\"label\":\"C\",\"type\":\"action\",\"undo\":true,"
        "\"confirm\":true},"
        "{\"id\":\"d\",\"label\":\"D\",\"type\":\"action\",\"confirm\":false}"
        "]}"));
    TEST_ASSERT_TRUE(s_menu.items[0].confirm);  // no undo -> confirm
    TEST_ASSERT_FALSE(s_menu.items[1].confirm); // undoable -> no confirm
    TEST_ASSERT_TRUE(s_menu.items[2].confirm);  // explicit wins
    TEST_ASSERT_FALSE(s_menu.items[3].confirm); // explicit wins
}

static void test_unknown_type_is_kept(void)
{
    TEST_ASSERT_EQUAL(MENU_OK, parse(
        "{\"schema\":1,\"items\":["
        "{\"id\":\"x\",\"label\":\"Future\",\"type\":\"slider\"}]}"));
    TEST_ASSERT_EQUAL(MENU_ITEM_UNKNOWN, s_menu.items[0].type);
}

static void test_failure_leaves_previous_menu_untouched(void)
{
    TEST_ASSERT_EQUAL(MENU_OK, parse(SPEC_MENU));
    TEST_ASSERT_EQUAL(MENU_ERR_SCHEMA,
                      parse("{\"schema\":2,\"items\":[]}"));
    TEST_ASSERT_NOT_EQUAL(0, strlen(s_err));
    TEST_ASSERT_EQUAL(MENU_ERR_JSON, parse("not json"));
    TEST_ASSERT_EQUAL(MENU_ERR_JSON, parse("[1,2]"));
    TEST_ASSERT_EQUAL(MENU_ERR_INVALID, parse("{\"schema\":1}"));
    TEST_ASSERT_EQUAL(7, s_menu.count);
    TEST_ASSERT_EQUAL_STRING("Kitchen", s_menu.title);
}

static void test_rejects_structural_problems(void)
{
    const char *bad[] = {
        // missing id
        "{\"schema\":1,\"items\":[{\"label\":\"A\",\"type\":\"action\"}]}",
        // duplicate id across levels
        "{\"schema\":1,\"items\":[{\"id\":\"a\",\"label\":\"A\","
        "\"type\":\"submenu\",\"items\":[{\"id\":\"a\",\"label\":\"A\","
        "\"type\":\"action\"}]}]}",
        // time without entity
        "{\"schema\":1,\"items\":[{\"id\":\"t\",\"label\":\"T\","
        "\"type\":\"time\"}]}",
        // bad step
        "{\"schema\":1,\"items\":[{\"id\":\"t\",\"label\":\"T\","
        "\"type\":\"time\",\"entity\":\"input_datetime.x\","
        "\"step_minutes\":0}]}",
        // submenu without items
        "{\"schema\":1,\"items\":[{\"id\":\"s\",\"label\":\"S\","
        "\"type\":\"submenu\"}]}",
        // non-object entry
        "{\"schema\":1,\"items\":[42]}",
        // id longer than MENU_ID_LEN - 1
        "{\"schema\":1,\"items\":[{\"id\":\""
        "abcdefghijabcdefghijabcdefghijabcdefghij\",\"label\":\"A\","
        "\"type\":\"action\"}]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(MENU_ERR_INVALID, parse(bad[i]), bad[i]);
    }
}

static void test_depth_limit(void)
{
    // Three list levels are fine...
    TEST_ASSERT_EQUAL(MENU_OK, parse(
        "{\"schema\":1,\"items\":[{\"id\":\"a\",\"label\":\"A\","
        "\"type\":\"submenu\",\"items\":[{\"id\":\"b\",\"label\":\"B\","
        "\"type\":\"submenu\",\"items\":[{\"id\":\"c\",\"label\":\"C\","
        "\"type\":\"action\"}]}]}]}"));
    // ...a fourth is not.
    TEST_ASSERT_EQUAL(MENU_ERR_INVALID, parse(
        "{\"schema\":1,\"items\":[{\"id\":\"a\",\"label\":\"A\","
        "\"type\":\"submenu\",\"items\":[{\"id\":\"b\",\"label\":\"B\","
        "\"type\":\"submenu\",\"items\":[{\"id\":\"c\",\"label\":\"C\","
        "\"type\":\"submenu\",\"items\":[]}]}]}]}"));
}

static void test_list_and_total_limits(void)
{
    char json[4096];
    int n = snprintf(json, sizeof(json), "{\"schema\":1,\"items\":[");
    for (int i = 0; i < MENU_MAX_LIST + 1; i++) {
        n += snprintf(json + n, sizeof(json) - n,
                      "%s{\"id\":\"i%d\",\"label\":\"I\",\"type\":\"action\"}",
                      i ? "," : "", i);
    }
    snprintf(json + n, sizeof(json) - n, "]}");
    TEST_ASSERT_EQUAL(MENU_ERR_INVALID, parse(json));

    // Four full submenus of 20 = 84 items, over the 64 total.
    n = snprintf(json, sizeof(json), "{\"schema\":1,\"items\":[");
    for (int s = 0; s < 4; s++) {
        n += snprintf(json + n, sizeof(json) - n,
                      "%s{\"id\":\"s%d\",\"label\":\"S\",\"type\":\"submenu\","
                      "\"items\":[", s ? "," : "", s);
        for (int i = 0; i < MENU_MAX_LIST; i++) {
            n += snprintf(json + n, sizeof(json) - n,
                          "%s{\"id\":\"s%di%d\",\"label\":\"I\","
                          "\"type\":\"action\"}", i ? "," : "", s, i);
        }
        n += snprintf(json + n, sizeof(json) - n, "]}");
    }
    snprintf(json + n, sizeof(json) - n, "]}");
    TEST_ASSERT_EQUAL(MENU_ERR_INVALID, parse(json));
}

static void test_long_label_is_truncated_not_rejected(void)
{
    TEST_ASSERT_EQUAL(MENU_OK, parse(
        "{\"schema\":1,\"items\":[{\"id\":\"a\",\"label\":\""
        "This label is much longer than forty-eight characters, really\","
        "\"type\":\"action\"}]}"));
    TEST_ASSERT_EQUAL(MENU_LABEL_LEN - 1, strlen(s_menu.items[0].label));
}

void run_menu_tests(void)
{
    RUN_TEST(test_spec_menu_parses_with_contiguous_lists);
    RUN_TEST(test_confirm_defaults_to_not_undo);
    RUN_TEST(test_unknown_type_is_kept);
    RUN_TEST(test_failure_leaves_previous_menu_untouched);
    RUN_TEST(test_rejects_structural_problems);
    RUN_TEST(test_depth_limit);
    RUN_TEST(test_list_and_total_limits);
    RUN_TEST(test_long_label_is_truncated_not_rejected);
}
