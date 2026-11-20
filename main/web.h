#pragma once

// The patch editor page and its JSON API, on the WiFi component's web server:
//   GET  /               the editor (WiFi setup while the setup hotspot is up)
//   GET  /api/patches    the library     POST: check and save a new one
//   GET  /api/presets    the built-in presets
//   POST /api/draft?index=N   play patch N of an unsaved library; DELETE stops
//   GET  /api/state      modes, mode playing, brightness   POST {"mode":N} or {"brightness":N}
//   GET  /api/audio?secs=N   N seconds of what the mic hears, as a WAV file

#include "esp_http_server.h"

void web_start(httpd_handle_t server);
