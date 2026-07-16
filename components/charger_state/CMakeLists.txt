idf_component_register(SRCS "charger_state.c"
                       INCLUDE_DIRS "include"
                       REQUIRES charger_proto charger_ble esp_timer)
