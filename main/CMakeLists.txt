idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES "driver" "esp_wifi" "nvs_flash" "esp_driver_i2s")
