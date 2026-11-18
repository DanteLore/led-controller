idf_component_register(
    SRCS "main.c" "effects.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer st7789 encoder led_strip
)
