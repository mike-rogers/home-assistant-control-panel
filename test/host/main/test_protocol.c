#include <string.h>

#include "unity.h"

#include "entity_store.h"
#include "protocol.h"

static void test_request_json(void)
{
    request_t req = {.request_id = "0000abcd-1", .item_id = "goodnight",
                     .op = OP_RUN};
    char buf[160];
    TEST_ASSERT_GREATER_THAN(0, request_to_json(&req, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"request_id\":\"0000abcd-1\","
                             "\"item_id\":\"goodnight\",\"op\":\"run\","
                             "\"value\":null}", buf);

    request_t set = {.request_id = "r2", .item_id = "wake_time",
                     .op = OP_SET, .value = "06:45"};
    TEST_ASSERT_GREATER_THAN(0, request_to_json(&set, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"request_id\":\"r2\",\"item_id\":"
                             "\"wake_time\",\"op\":\"set\",\"value\":"
                             "\"06:45\"}", buf);

    TEST_ASSERT_EQUAL(-1, request_to_json(&set, buf, 10));
}

static void test_result_parse(void)
{
    result_t res;
    TEST_ASSERT_TRUE(result_parse("{\"request_id\":\"r1\",\"status\":\"ok\","
                                  "\"message\":\"\",\"undo_seconds\":30}",
                                  &res));
    TEST_ASSERT_EQUAL_STRING("r1", res.request_id);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL(30, res.undo_seconds);

    TEST_ASSERT_TRUE(result_parse("{\"request_id\":\"r2\",\"status\":"
                                  "\"error\",\"message\":\"Script failed\","
                                  "\"undo_seconds\":null}", &res));
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL_STRING("Script failed", res.message);
    TEST_ASSERT_EQUAL(0, res.undo_seconds);

    TEST_ASSERT_FALSE(result_parse("{\"status\":\"ok\"}", &res));
    TEST_ASSERT_FALSE(result_parse("garbage", &res));
}

static void test_entity_store(void)
{
    entity_store_t store;
    entity_store_init(&store);
    TEST_ASSERT_NULL(entity_store_get(&store, "sensor.t"));

    TEST_ASSERT_TRUE(entity_store_update(&store, "sensor.t",
        "{\"state\":\"21.5\",\"attributes\":"
        "{\"unit_of_measurement\":\"\xc2\xb0" "C\"}}"));
    const entity_state_t *s = entity_store_get(&store, "sensor.t");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("21.5", s->state);
    TEST_ASSERT_EQUAL_STRING("\xc2\xb0" "C", s->unit);

    TEST_ASSERT_TRUE(entity_store_update(&store, "sensor.t",
                                         "{\"state\":\"22\"}"));
    TEST_ASSERT_EQUAL(1, store.count);
    TEST_ASSERT_EQUAL_STRING("", entity_store_get(&store, "sensor.t")->unit);

    TEST_ASSERT_FALSE(entity_store_update(&store, "sensor.t", "{}"));
    TEST_ASSERT_TRUE(entity_store_update(&store, "sensor.t", ""));
    TEST_ASSERT_NULL(entity_store_get(&store, "sensor.t"));
}

void run_protocol_tests(void)
{
    RUN_TEST(test_request_json);
    RUN_TEST(test_result_parse);
    RUN_TEST(test_entity_store);
}
