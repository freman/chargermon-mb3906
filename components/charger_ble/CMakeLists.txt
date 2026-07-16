idf_component_register(
    SRCS "charger_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt charger_proto
)
