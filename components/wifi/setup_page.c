#include "setup_page.h"
#include "wifi.h"
#include "esp_wifi.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "setup_page";

#define MAX_NETWORKS   20
#define MAX_FORM_LEN  512   // room for a 32-byte name and 63-byte password, %-encoded

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[]   asm("_binary_setup_html_end");

static const char *redirect_url;

static const char *state_names[] = {
    [WIFI_CONNECTING] = "connecting",
    [WIFI_CONNECTED]  = "connected",
    [WIFI_SETUP]      = "setup",
};

// ---- JSON output -----------------------------------------------------------

// Network names can hold any bytes, so quotes, backslashes and control
// characters are escaped
static void put_json_string(char *out, size_t size, const char *s)
{
    size_t n = 0;
    out[n++] = '"';
    for (; *s && n < size - 8; s++) {
        if (*s == '"' || *s == '\\')
            n += snprintf(&out[n], size - n, "\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            n += snprintf(&out[n], size - n, "\\u%04x", *s);
        else
            out[n++] = *s;
    }
    out[n++] = '"';
    out[n] = 0;
}

// ---- Form input ------------------------------------------------------------

static int hex_value(char c)
{
    return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
}

static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+')
            *out++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            *out++ = hex_value(s[1]) << 4 | hex_value(s[2]);
            s += 2;
        } else
            *out++ = *s;
    }
    *out = 0;
}

static void form_value(const char *form, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(form, key, out, size) != ESP_OK)
        out[0] = 0;
    url_decode(out);
}

// ---- Handlers --------------------------------------------------------------

static void log_request(httpd_req_t *req)
{
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    ESP_LOGI(TAG, "%s %s%s", http_method_str(req->method), host, req->uri);
}

static esp_err_t redirect(httpd_req_t *req, const char *url)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t page_get(httpd_req_t *req)
{
    log_request(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_status_t s;
    wifi_get_status(&s);
    char ssid[200], error[40];
    put_json_string(ssid, sizeof(ssid), s.ssid);
    put_json_string(error, sizeof(error), s.error ? s.error : "");
    char json[320];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"hotspot\":%s,\"ssid\":%s,\"ip\":\"%s\",\"error\":%s}",
             state_names[s.state], s.hotspot ? "true" : "false", ssid, s.ip, error);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// Blocks for the ~2s a scan takes. The page tidies the list (duplicates, order).
static esp_err_t scan_get(httpd_req_t *req)
{
    log_request(req);
    static wifi_ap_record_t records[MAX_NETWORKS];
    uint16_t count = 0;
    wifi_scan_config_t config = {0};
    if (esp_wifi_scan_start(&config, true) == ESP_OK) {
        count = MAX_NETWORKS;
        esp_wifi_scan_get_ap_records(&count, records);
    } else {
        esp_wifi_clear_ap_list();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < count; i++) {
        char ssid[200], entry[260];
        put_json_string(ssid, sizeof(ssid), (char *)records[i].ssid);
        snprintf(entry, sizeof(entry), "%s{\"ssid\":%s,\"rssi\":%d,\"secure\":%s}", i ? "," : "",
                 ssid, records[i].rssi, records[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
        httpd_resp_sendstr_chunk(req, entry);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t save_post(httpd_req_t *req)
{
    log_request(req);
    // Static: handlers run one at a time on the server's task, whose stack is small
    static char form[MAX_FORM_LEN + 1], ssid[MAX_FORM_LEN], password[MAX_FORM_LEN];
    if (req->content_len > MAX_FORM_LEN)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form too long");
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, &form[received], req->content_len - received);
        if (n <= 0)
            return ESP_FAIL;
        received += n;
    }
    form[received] = 0;

    form_value(form, "ssid", ssid, sizeof(ssid));
    form_value(form, "password", password, sizeof(password));
    size_t password_len = strlen(password);
    if (ssid[0] == 0 || strlen(ssid) > 32)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Network name must be 1 to 32 characters");
    if (password_len > 63 || (password_len > 0 && password_len < 8))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password must be 8 to 63 characters, or empty for an open network");

    wifi_join(ssid, password);
    return httpd_resp_sendstr(req, "Joining");
}

// While the hotspot is up, phones probe URLs like captive.apple.com/hotspot-detect.html
// or connectivitycheck.gstatic.com/generate_204; redirecting them pops up the setup page
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    log_request(req);
    wifi_status_t s;
    wifi_get_status(&s);
    if (s.hotspot)
        return redirect(req, redirect_url);
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

static void add(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t route = { .uri = uri, .method = method, .handler = handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
}

httpd_handle_t setup_page_start(const char *setup_url)
{
    redirect_url = setup_url;
    httpd_handle_t server;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0;               // core 1 is kept for the LEDs
    config.lru_purge_enable = true;   // phones open lots of connections while probing
    config.max_uri_handlers = 16;     // room for the patch editor's routes too
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    add(server, "/wifi",        HTTP_GET,  page_get);
    add(server, "/wifi/status", HTTP_GET,  status_get);
    add(server, "/wifi/scan",   HTTP_GET,  scan_get);
    add(server, "/wifi/save",   HTTP_POST, save_post);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    return server;
}
