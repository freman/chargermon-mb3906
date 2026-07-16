idf_component_register(SRCS "wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES appcfg esp_wifi esp_netif esp_event esp_http_server lwip nvs_flash)
