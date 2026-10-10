#include <stdlib.h>

#include "unity.h"

void run_menu_tests(void);
void run_protocol_tests(void);
void run_ui_sm_tests(void);

void setUp(void) {}
void tearDown(void) {}

void app_main(void)
{
    UNITY_BEGIN();
    run_menu_tests();
    run_protocol_tests();
    run_ui_sm_tests();
    exit(UNITY_END() == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
