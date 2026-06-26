idf_component_register(SRCS "lldp_cdp_parser.c" "main.c"  
                    INCLUDE_DIRS "."
                    REQUIRES lvgl esp_lcd esp_eth esp_driver_spi esp_driver_gpio)