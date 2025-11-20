idf_component_register(SRCS
                    "main.c"
                    "mesh_init.c"
                    "BLE_init.c"
                    PRIV_REQUIRES esp_wifi esp_driver_gpio esp_driver_ledc nvs_flash bt
                    INCLUDE_DIRS "." "include")