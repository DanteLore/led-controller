#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

typedef enum { WIFI_CONNECTING, WIFI_CONNECTED, WIFI_SETUP } wifi_state_t;

typedef struct {
    wifi_state_t state;
    bool hotspot;        // setup hotspot is up
    char ssid[33];       // network being joined or joined
    char ip[16];         // empty until connected
    char hotspot_ssid[33];   // the setup hotspot's name, while it's up
    const char *error;   // why the last attempt failed, or NULL
} wifi_status_t;

// Joins the saved network. With no saved network, or if it can't be joined,
// starts an open setup hotspot whose page lets a phone pick a network. Needs
// NVS initialised first; the network is saved by the WiFi driver itself.
void wifi_start(const char *hostname);

wifi_state_t wifi_state(void);
void wifi_get_status(wifi_status_t *status);

// Saves a new network and joins it; the hotspot stays up until it succeeds
void wifi_join(const char *ssid, const char *password);

// The web server the setup page runs on, for other pages to register on
httpd_handle_t wifi_web_server(void);
