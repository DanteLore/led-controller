#include "web.h"
#include "library.h"
#include "control.h"
#include "effects.h"
#include "wifi.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web";

extern const char editor_html_start[] asm("_binary_editor_html_start");
extern const char editor_html_end[]   asm("_binary_editor_html_end");
extern const char patch_js_start[]    asm("_binary_patch_js_start");
extern const char patch_js_end[]      asm("_binary_patch_js_end");

static esp_err_t send_file(httpd_req_t *req, const char *type, const char *start, const char *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, end - start);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t bad_request(httpd_req_t *req, const char *message)
{
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message);
}

// The request body as a NUL-terminated string, or NULL after sending an error
static char *read_body(httpd_req_t *req)
{
    if (req->content_len > PATCH_JSON_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too big: the limit is 8KB");
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return NULL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0) {
            free(body);
            return NULL;
        }
        received += n;
    }
    body[received] = 0;
    return body;
}

// While a phone is on the setup hotspot, the root is the WiFi setup page
static esp_err_t root_get(httpd_req_t *req)
{
    wifi_status_t s;
    wifi_get_status(&s);
    if (s.hotspot && s.state != WIFI_CONNECTED) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wifi");
        return httpd_resp_send(req, NULL, 0);
    }
    return send_file(req, "text/html", editor_html_start, editor_html_end);
}

static esp_err_t patch_js_get(httpd_req_t *req)
{
    return send_file(req, "text/javascript", patch_js_start, patch_js_end);
}

static esp_err_t patches_get(httpd_req_t *req)
{
    char *json = library_json();
    if (!json)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

static esp_err_t patches_post(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body)
        return ESP_FAIL;
    char error[160];
    bool ok = library_save(body, error, sizeof(error));
    free(body);
    if (!ok)
        return bad_request(req, error);
    return httpd_resp_sendstr(req, "Saved");
}

static esp_err_t presets_get(httpd_req_t *req)
{
    size_t n;
    const char *presets = library_presets(&n);
    return send_file(req, "application/json", presets, presets + n);
}

static esp_err_t draft_post(httpd_req_t *req)
{
    char query[32], value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, "index", value, sizeof(value)) != ESP_OK)
        return bad_request(req, "Needs ?index=N");
    char *body = read_body(req);
    if (!body)
        return ESP_FAIL;
    char error[160];
    bool ok = library_play_draft(body, atoi(value), error, sizeof(error));
    free(body);
    if (!ok)
        return bad_request(req, error);
    return httpd_resp_sendstr(req, "Playing");
}

static esp_err_t draft_delete(httpd_req_t *req)
{
    library_stop_draft();
    return httpd_resp_sendstr(req, "Stopped");
}

static esp_err_t state_get(httpd_req_t *req)
{
    control_state_t s;
    control_get_state(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON *modes = cJSON_AddArrayToObject(root, "modes");
    library_lock();
    const patch_set_t *set = library_patches();
    for (int i = 0; i < set->count; i++)
        cJSON_AddItemToArray(modes, cJSON_CreateString(set->patches[i].name));
    library_unlock();
    for (int t = 0; t < TEST_COUNT; t++)
        cJSON_AddItemToArray(modes, cJSON_CreateString(effects_test_name(t)));
    cJSON_AddNumberToObject(root, "mode", s.mode);
    cJSON_AddBoolToObject(root, "draft", s.draft);
    cJSON_AddNumberToObject(root, "brightness", s.brightness);
    cJSON_AddNumberToObject(root, "speed", s.speed);
    cJSON_AddNumberToObject(root, "leds", s.leds);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    esp_err_t err = send_json(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t state_post(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body)
        return ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (cJSON_IsNumber(mode) && mode->valueint >= 0)
        control_select(mode->valueint);
    if (cJSON_IsNumber(brightness))
        control_set_brightness(brightness->valueint);
    bool ok = cJSON_IsNumber(mode) || cJSON_IsNumber(brightness);
    cJSON_Delete(root);
    return ok ? httpd_resp_sendstr(req, "OK") : bad_request(req, "Expected {\"mode\":N} or {\"brightness\":N}");
}

static void add(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t route = { .uri = uri, .method = method, .handler = handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
}

void web_start(httpd_handle_t server)
{
    add(server, "/",             HTTP_GET,    root_get);
    add(server, "/patch.js",     HTTP_GET,    patch_js_get);
    add(server, "/api/patches",  HTTP_GET,    patches_get);
    add(server, "/api/patches",  HTTP_POST,   patches_post);
    add(server, "/api/presets",  HTTP_GET,    presets_get);
    add(server, "/api/draft",    HTTP_POST,   draft_post);
    add(server, "/api/draft",    HTTP_DELETE, draft_delete);
    add(server, "/api/state",    HTTP_GET,    state_get);
    add(server, "/api/state",    HTTP_POST,   state_post);
    ESP_LOGI(TAG, "Patch editor ready");
}
