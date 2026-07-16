idf_component_register(SRCS "web.c"
                       INCLUDE_DIRS "include"
                       REQUIRES appcfg wifi mqtt_ha charger_state charger_control charger_proto
                                esp_http_server mbedtls ota app_update)
