idf_component_register(SRCS "mqtt_ha.c"
                       INCLUDE_DIRS "include"
                       REQUIRES appcfg wifi charger_state charger_control charger_proto mqtt esp_wifi)
