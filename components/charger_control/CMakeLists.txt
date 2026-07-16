idf_component_register(SRCS "charger_control.c"
                       INCLUDE_DIRS "include"
                       REQUIRES charger_proto charger_ble charger_state appcfg)
