# Pure C panel logic: no IDF dependencies beyond cJSON, so it builds for the
# device and for the linux target (test/host) alike.
idf_component_register(SRCS "menu.c"
                            "entity_store.c"
                            "protocol.c"
                            "ui_sm.c"
                       INCLUDE_DIRS "include")
# cJSON comes from idf_component.yml (espressif/cjson); the component
# manager adds it to this component's requirements.
