idf_component_register(
    SRCS "main.c" "player.c" "inputs.c" "ui.c" "control.c" "frame_stats.c"
         "screen.c" "test_patterns.c" "library.c" "web.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer esp_driver_gpio nvs_flash esp_http_server json st7789 encoder led_strip wifi patch qr mic
    EMBED_FILES "presets.json" "editor.html" "editor.css" "editor.js" "editor-forms.js" "editor-preview.js"
                "../components/patch/patch.js"
)
