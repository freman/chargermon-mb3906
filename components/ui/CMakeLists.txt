idf_component_register(SRCS "ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES charger_state charger_proto wifi
                                esp_driver_i2c esp_driver_gpio)
