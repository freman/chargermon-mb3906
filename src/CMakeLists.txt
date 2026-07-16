# "main" component (PlatformIO treats ./src as main).
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES charger_ble charger_proto charger_state charger_control nvs_flash
             esp_driver_usb_serial_jtag appcfg wifi mqtt_ha web ota ui
)
