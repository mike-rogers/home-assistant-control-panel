idf_component_register(SRCS "test_main.c"
                            "test_menu.c"
                            "test_protocol.c"
                            "test_ui_sm.c"
                       INCLUDE_DIRS "."
                       REQUIRES panel_core unity)
