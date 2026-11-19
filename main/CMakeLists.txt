idf_component_register(
    SRCS "main.c" "effects.c" "library.c" "web.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer nvs_flash esp_http_server json st7789 encoder led_strip wifi patch
    EMBED_FILES "presets.json" "editor.html" "../components/patch/patch.js"
)
