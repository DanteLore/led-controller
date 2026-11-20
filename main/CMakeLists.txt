idf_component_register(
    SRCS "main.c" "screen.c" "effects.c" "library.c" "web.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer esp_driver_gpio nvs_flash esp_http_server json st7789 encoder led_strip wifi patch qr mic
    EMBED_FILES "presets.json" "editor.html" "../components/patch/patch.js"
)
