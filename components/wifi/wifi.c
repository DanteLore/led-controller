#include "wifi.h"
#include "captive_dns.h"
#include "setup_page.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define CONNECT_ATTEMPTS        5        // before falling back to the setup hotspot
#define RETRY_US          3000000
#define HOTSPOT_LINGER_US  60000000      // after joining, so the phone can show the new address
#define IDLE_CHECK_US      30000000
#define SETUP_IDLE_US     300000000      // an unused hotspot gives up and retries the saved network

// A public-looking address, not the softAP's default 192.168.4.1: Android 11+
// treats a probe that resolves to a private address as "no internet" and never
// offers the sign-in page. WLED uses 4.3.2.1 for the same reason. The hotspot
// has no internet, so the real owner of this address is never reached.
#define HOTSPOT_IP        "4.3.2.1"
#define HOTSPOT_SETUP_URL "http://" HOTSPOT_IP "/wifi"

static const char *TAG = "wifi";

static SemaphoreHandle_t lock;
static esp_timer_handle_t retry_timer, linger_timer, idle_timer;
static httpd_handle_t server;

static wifi_state_t state;
static bool hotspot;
static bool ever_connected;   // since the network was last chosen: drops are then retried forever
static int attempts;
static char ip[16];
static const char *error;

static void take(void) { xSemaphoreTake(lock, portMAX_DELAY); }
static void give(void) { xSemaphoreGive(lock); }

static void saved_ssid(char *out, size_t size)
{
    wifi_config_t config;
    esp_wifi_get_config(WIFI_IF_STA, &config);
    strlcpy(out, (char *)config.sta.ssid, size);
}

static bool has_saved_network(void)
{
    char ssid[33];
    saved_ssid(ssid, sizeof(ssid));
    return ssid[0] != 0;
}

static void start_hotspot(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    wifi_config_t config = { .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
    config.ap.ssid_len = snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid), "LED-Setup-%02X%02X", mac[4], mac[5]);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &config);
    captive_dns_enable(true);
    hotspot = true;
    ESP_LOGI(TAG, "Setup hotspot '%s' is up: join it and open %s", (char *)config.ap.ssid, HOTSPOT_SETUP_URL);
}

static void stop_hotspot(void)
{
    captive_dns_enable(false);
    esp_wifi_set_mode(WIFI_MODE_STA);
    hotspot = false;
    ESP_LOGI(TAG, "Setup hotspot closed");
}

static void connect_saved(void)
{
    char ssid[33];
    saved_ssid(ssid, sizeof(ssid));
    state = WIFI_CONNECTING;
    attempts = 0;
    ESP_LOGI(TAG, "Connecting to '%s'", ssid);
    esp_wifi_connect();
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return "network not found";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "wrong password?";
        default:
            return "couldn't connect";
    }
}

static void connect_failed(uint8_t reason)
{
    error = reason_text(reason);
    ip[0] = 0;
    if (ever_connected || ++attempts < CONNECT_ATTEMPTS) {
        ESP_LOGW(TAG, "Disconnected (%s, reason %d), retrying", error, reason);
        state = WIFI_CONNECTING;
        esp_timer_start_once(retry_timer, RETRY_US);
        return;
    }
    ESP_LOGW(TAG, "Gave up after %d attempts (%s)", attempts, error);
    if (!hotspot)
        start_hotspot();
    state = WIFI_SETUP;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    take();
    switch (id) {
        case WIFI_EVENT_STA_START:
            if (has_saved_network()) {
                connect_saved();
            } else {
                ESP_LOGI(TAG, "No saved network");
                start_hotspot();
                state = WIFI_SETUP;
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            uint8_t reason = ((wifi_event_sta_disconnected_t *)data)->reason;
            // Setup has given up on this network, or wifi_join() hung up to switch networks
            if (state != WIFI_SETUP && reason != WIFI_REASON_ASSOC_LEAVE)
                connect_failed(reason);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Phone joined the setup hotspot");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "Phone left the setup hotspot");
            break;
    }
    give();
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    take();
    esp_ip4addr_ntoa(&((ip_event_got_ip_t *)data)->ip_info.ip, ip, sizeof(ip));
    state = WIFI_CONNECTED;
    ever_connected = true;
    error = NULL;
    ESP_LOGI(TAG, "Connected, address http://%s/", ip);
    if (hotspot)
        esp_timer_start_once(linger_timer, HOTSPOT_LINGER_US);
    give();
}

static void on_retry(void *arg)
{
    take();
    if (state == WIFI_CONNECTING)
        esp_wifi_connect();
    give();
}

static void on_linger_done(void *arg)
{
    take();
    if (hotspot && state == WIFI_CONNECTED)
        stop_hotspot();
    give();
}

// If the saved network was down (a power cut, say), nobody may ever come to
// the hotspot, so after a while with no phone on it, try the network again
static void on_idle_check(void *arg)
{
    static int64_t idle_us;
    take();
    wifi_sta_list_t phones;
    bool idle = state == WIFI_SETUP && has_saved_network()
             && esp_wifi_ap_get_sta_list(&phones) == ESP_OK && phones.num == 0;
    idle_us = idle ? idle_us + IDLE_CHECK_US : 0;
    if (idle_us >= SETUP_IDLE_US) {
        idle_us = 0;
        ESP_LOGI(TAG, "Setup hotspot unused, trying the saved network again");
        stop_hotspot();
        connect_saved();
    }
    give();
}

// DHCP hands out addresses next to the AP's own, and names it as the DNS server
static void use_hotspot_address(esp_netif_t *ap)
{
    esp_netif_ip_info_t info = {0};
    info.ip.addr = info.gw.addr = esp_ip4addr_aton(HOTSPOT_IP);
    info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(ap);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &info));
    esp_netif_dhcps_start(ap);
}

static esp_timer_handle_t make_timer(esp_timer_cb_t callback, const char *name)
{
    esp_timer_handle_t timer;
    esp_timer_create_args_t args = { .callback = callback, .name = name };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    return timer;
}

void wifi_start(const char *hostname)
{
    lock = xSemaphoreCreateMutex();
    retry_timer  = make_timer(on_retry, "wifi_retry");
    linger_timer = make_timer(on_linger_done, "wifi_linger");
    idle_timer   = make_timer(on_idle_check, "wifi_idle");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    use_hotspot_address(esp_netif_create_default_wifi_ap());
    esp_netif_set_hostname(sta, hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));

    captive_dns_start(HOTSPOT_IP);
    server = setup_page_start(HOTSPOT_SETUP_URL);

    // Station start fires WIFI_EVENT_STA_START, which joins or opens the hotspot
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power saving sleeps between router beacons, adding 100ms+ to every reply
    // and dropping the odd request; not worth it on mains power
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_timer_start_periodic(idle_timer, IDLE_CHECK_US);
}

wifi_state_t wifi_state(void)
{
    return state;
}

void wifi_get_status(wifi_status_t *status)
{
    take();
    status->state = state;
    status->hotspot = hotspot;
    saved_ssid(status->ssid, sizeof(status->ssid));
    strlcpy(status->ip, ip, sizeof(status->ip));
    status->error = error;
    give();
}

void wifi_join(const char *ssid, const char *password)
{
    take();
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    esp_timer_stop(retry_timer);
    esp_timer_stop(linger_timer);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &config);
    ever_connected = false;
    error = NULL;
    ip[0] = 0;
    connect_saved();
    give();
}

httpd_handle_t wifi_web_server(void)
{
    return server;
}
