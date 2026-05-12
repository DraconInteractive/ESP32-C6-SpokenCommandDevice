idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_driver_i2s esp_event esp_http_client esp_lcd esp_lcd_sh8601 esp_lcd_touch esp_lcd_touch_ft5x06 esp_netif esp_wifi es8311 nvs_flash spi_flash)
