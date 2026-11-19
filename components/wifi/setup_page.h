#pragma once

#include "esp_http_server.h"

// Starts the web server with the WiFi setup page at /wifi; other pages, such
// as the root, are registered by their owners. While the setup
// hotspot is up, unknown URLs redirect to setup_url so phones show it as a
// captive portal.
httpd_handle_t setup_page_start(const char *setup_url);
