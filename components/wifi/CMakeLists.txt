idf_component_register(
    SRCS "wifi.c" "captive_dns.c" "setup_page.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer lwip
    EMBED_FILES "setup.html"
)
