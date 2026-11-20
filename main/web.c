#include "web.h"
#include "library.h"
#include "control.h"
#include "effects.h"
#include "wifi.h"
#include "mic.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
    cJSON_AddNumberToObject(root, "bpm", s.bpm);
    cJSON_AddNumberToObject(root, "beat_confidence", s.beat_confidence);
    cJSON_AddBoolToObject(root, "beat_found", !s.beat_holding);
    cJSON_AddNumberToObject(root, "beat_energy", s.beat_energy);
    cJSON_AddNumberToObject(root, "beat_belief", s.beat_belief);
    cJSON_AddBoolToObject(root, "breakdown", s.breakdown);
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

// What the mic hears, as a WAV file, for tuning the beat tracker on a computer
static esp_err_t audio_get(httpd_req_t *req)
{
    char query[32], value[8];
    int secs = 10;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
        && httpd_query_key_value(query, "secs", value, sizeof(value)) == ESP_OK)
        secs = atoi(value);
    if (secs < 1 || secs > 120)
        return bad_request(req, "secs should be from 1 to 120");
    if (!mic_record_start())
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Already recording");

    uint32_t samples = secs * MIC_SAMPLE_RATE, bytes = samples * 2;
    uint8_t header[44];
    memcpy(header, "RIFF", 4);
    uint32_t fields[] = { 36 + bytes };
    memcpy(header + 4, fields, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    uint32_t fmt[] = { 16, 1 | (1 << 16), MIC_SAMPLE_RATE, MIC_SAMPLE_RATE * 2, 2 | (16 << 16) };
    memcpy(header + 16, fmt, sizeof(fmt));
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &bytes, 4);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"mic.wav\"");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));

    static int16_t chunk[1024];
    uint32_t sent = 0;
    while (err == ESP_OK && sent < samples) {
        size_t want = samples - sent < 1024 ? samples - sent : 1024;
        size_t got = mic_record_read(chunk, want, 1000);
        if (!got) break;
        err = httpd_resp_send_chunk(req, (const char *)chunk, got * 2);
        sent += got;
    }
    mic_record_stop();
    ESP_LOGI(TAG, "Recorded %lu samples", (unsigned long)sent);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

// Each task's share of one core over a one-second window, plus per-core load
// worked out from the idle tasks
#define CPU_MAX_TASKS 24

static esp_err_t cpu_get(httpd_req_t *req)
{
    static TaskStatus_t before[CPU_MAX_TASKS], after[CPU_MAX_TASKS];
    configRUN_TIME_COUNTER_TYPE t0, t1;
    UBaseType_t n0 = uxTaskGetSystemState(before, CPU_MAX_TASKS, &t0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    UBaseType_t n1 = uxTaskGetSystemState(after, CPU_MAX_TASKS, &t1);
    double window = (double)(t1 - t0);

    cJSON *root = cJSON_CreateObject();
    cJSON *tasks = cJSON_AddArrayToObject(root, "tasks");
    double idle[2] = { 0, 0 };
    for (UBaseType_t i = 0; i < n1; i++) {
        configRUN_TIME_COUNTER_TYPE start = 0;
        for (UBaseType_t j = 0; j < n0; j++)
            if (before[j].xHandle == after[i].xHandle) start = before[j].ulRunTimeCounter;
        double pct = window > 0 ? 100.0 * (after[i].ulRunTimeCounter - start) / window : 0;
        BaseType_t core = xTaskGetCoreID(after[i].xHandle);
        if (!strncmp(after[i].pcTaskName, "IDLE", 4) && core >= 0 && core < 2) idle[core] = pct;
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", after[i].pcTaskName);
        cJSON_AddNumberToObject(t, "core", core < 2 ? core : -1);
        cJSON_AddNumberToObject(t, "percent", round(pct * 10) / 10);
        cJSON_AddNumberToObject(t, "stack_free", after[i].usStackHighWaterMark);
        cJSON_AddItemToArray(tasks, t);
    }
    cJSON *cores = cJSON_AddArrayToObject(root, "core_load");
    for (int c = 0; c < 2; c++) cJSON_AddItemToArray(cores, cJSON_CreateNumber(round((100 - idle[c]) * 10) / 10));
    frame_timing_t timing;
    control_get_timing(&timing);
    cJSON *frame = cJSON_AddObjectToObject(root, "frame");
    cJSON_AddNumberToObject(frame, "fps", round(timing.fps * 10) / 10);
    cJSON_AddNumberToObject(frame, "render_us", round(timing.render_us));
    cJSON_AddNumberToObject(frame, "strip_us", round(timing.strip_us));
    cJSON_AddNumberToObject(frame, "screen_us", round(timing.screen_us));
    cJSON_AddNumberToObject(frame, "total_us", round(timing.frame_us));
    cJSON_AddNumberToObject(root, "heap_free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
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
    add(server, "/api/audio",    HTTP_GET,    audio_get);
    add(server, "/api/cpu",      HTTP_GET,    cpu_get);
    ESP_LOGI(TAG, "Patch editor ready");
}
