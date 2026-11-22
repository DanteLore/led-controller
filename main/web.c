#include "web.h"
#include "library.h"
#include "control.h"
#include "test_patterns.h"
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

#define EMBEDDED(name) \
    extern const char name##_start[] asm("_binary_" #name "_start"); \
    extern const char name##_end[] asm("_binary_" #name "_end");
EMBEDDED(editor_html)
EMBEDDED(editor_css)
EMBEDDED(editor_js)
EMBEDDED(editor_forms_js)
EMBEDDED(editor_preview_js)
EMBEDDED(patch_js)

typedef struct {
    const char *uri, *type, *start, *end;
} static_file_t;

// The editor's style and scripts, served as they are
static const static_file_t STATIC_FILES[] = {
    { "/editor.css",        "text/css",        editor_css_start,        editor_css_end },
    { "/editor.js",         "text/javascript", editor_js_start,         editor_js_end },
    { "/editor-forms.js",   "text/javascript", editor_forms_js_start,   editor_forms_js_end },
    { "/editor-preview.js", "text/javascript", editor_preview_js_start, editor_preview_js_end },
    { "/patch.js",          "text/javascript", patch_js_start,          patch_js_end },
};

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

static esp_err_t out_of_memory(httpd_req_t *req)
{
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
}

// Sends the tree as JSON, and frees it
static esp_err_t send_cjson(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return out_of_memory(req);
    esp_err_t err = send_json(req, json);
    cJSON_free(json);
    return err;
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
        out_of_memory(req);
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

static int query_int(httpd_req_t *req, const char *key, int fallback)
{
    char query[32], value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK)
        return fallback;
    return atoi(value);
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

static esp_err_t static_file_get(httpd_req_t *req)
{
    const static_file_t *f = req->user_ctx;
    return send_file(req, f->type, f->start, f->end);
}

static esp_err_t patches_get(httpd_req_t *req)
{
    char *json = library_json();
    if (!json)
        return out_of_memory(req);
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
    int index = query_int(req, "index", -1);
    if (index < 0)
        return bad_request(req, "Needs ?index=N");
    char *body = read_body(req);
    if (!body)
        return ESP_FAIL;
    char error[160];
    bool ok = library_play_draft(body, index, error, sizeof(error));
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

// The library's patch names, then the test patterns
static void add_modes(cJSON *root)
{
    cJSON *modes = cJSON_AddArrayToObject(root, "modes");
    library_lock();
    const patch_set_t *set = library_patches();
    for (int i = 0; i < set->count; i++)
        cJSON_AddItemToArray(modes, cJSON_CreateString(set->patches[i].name));
    library_unlock();
    for (int t = 0; t < TEST_COUNT; t++)
        cJSON_AddItemToArray(modes, cJSON_CreateString(test_pattern_name(t)));
}

static void add_beat(cJSON *root, const beat_state_t *beat)
{
    cJSON_AddNumberToObject(root, "bpm", beat->bpm);
    cJSON_AddNumberToObject(root, "beat_confidence", beat->confidence);
    cJSON_AddBoolToObject(root, "beat_found", !beat->holding);
    cJSON_AddNumberToObject(root, "beat_energy", beat->energy);
    cJSON_AddNumberToObject(root, "beat_belief", beat->belief);
    cJSON_AddBoolToObject(root, "breakdown", beat->breakdown);
}

static esp_err_t state_get(httpd_req_t *req)
{
    control_state_t s;
    control_get_state(&s);
    cJSON *root = cJSON_CreateObject();
    add_modes(root);
    cJSON_AddNumberToObject(root, "mode", s.mode);
    cJSON_AddBoolToObject(root, "draft", s.draft);
    cJSON_AddNumberToObject(root, "brightness", s.brightness);
    cJSON_AddNumberToObject(root, "speed", s.speed);
    cJSON_AddNumberToObject(root, "leds", s.leds);
    add_beat(root, &s.beat);
    return send_cjson(req, root);
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

// A 16-bit mono PCM header for this many samples
static void wav_header(uint8_t header[44], uint32_t samples)
{
    uint32_t bytes = samples * 2;
    uint32_t riff_size = 36 + bytes;
    uint32_t fmt[] = { 16, 1 | (1 << 16), MIC_SAMPLE_RATE, MIC_SAMPLE_RATE * 2, 2 | (16 << 16) };
    memcpy(header, "RIFF", 4);
    memcpy(header + 4, &riff_size, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    memcpy(header + 16, fmt, sizeof(fmt));
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &bytes, 4);
}

static esp_err_t send_recording(httpd_req_t *req, uint32_t samples)
{
    static int16_t chunk[1024];
    esp_err_t err = ESP_OK;
    uint32_t sent = 0;
    while (err == ESP_OK && sent < samples) {
        size_t want = samples - sent < 1024 ? samples - sent : 1024;
        size_t got = mic_record_read(chunk, want, 1000);
        if (!got) break;
        err = httpd_resp_send_chunk(req, (const char *)chunk, got * 2);
        sent += got;
    }
    ESP_LOGI(TAG, "Recorded %lu samples", (unsigned long)sent);
    return err;
}

// What the mic hears, as a WAV file, for tuning the beat tracker on a computer
static esp_err_t audio_get(httpd_req_t *req)
{
    int secs = query_int(req, "secs", 10);
    if (secs < 1 || secs > 120)
        return bad_request(req, "secs should be from 1 to 120");
    if (!mic_record_start())
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Already recording");

    uint32_t samples = secs * MIC_SAMPLE_RATE;
    uint8_t header[44];
    wav_header(header, samples);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"mic.wav\"");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    if (err == ESP_OK)
        err = send_recording(req, samples);
    mic_record_stop();
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

// Each task's share of one core over a one-second window, plus per-core load
// worked out from the idle tasks
#define CPU_MAX_TASKS 24
#define CPU_CORES      2

typedef struct {
    TaskStatus_t tasks[CPU_MAX_TASKS];
    UBaseType_t count;
    configRUN_TIME_COUNTER_TYPE time;
} task_sample_t;

static void sample_tasks(task_sample_t *s)
{
    s->count = uxTaskGetSystemState(s->tasks, CPU_MAX_TASKS, &s->time);
}

static configRUN_TIME_COUNTER_TYPE run_time_before(const task_sample_t *before, TaskHandle_t task)
{
    for (UBaseType_t i = 0; i < before->count; i++)
        if (before->tasks[i].xHandle == task) return before->tasks[i].ulRunTimeCounter;
    return 0;
}

static double tenths(double v)
{
    return round(v * 10) / 10;
}

static void add_task(cJSON *tasks, const TaskStatus_t *t, BaseType_t core, double percent)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", t->pcTaskName);
    cJSON_AddNumberToObject(item, "core", core < CPU_CORES ? core : -1);
    cJSON_AddNumberToObject(item, "percent", tenths(percent));
    cJSON_AddNumberToObject(item, "stack_free", t->usStackHighWaterMark);
    cJSON_AddItemToArray(tasks, item);
}

// Adds each task's load, and returns each core's from its idle task
static void add_tasks(cJSON *root, const task_sample_t *before, const task_sample_t *after, double idle[CPU_CORES])
{
    cJSON *tasks = cJSON_AddArrayToObject(root, "tasks");
    double window = (double)(after->time - before->time);
    for (UBaseType_t i = 0; i < after->count; i++) {
        const TaskStatus_t *t = &after->tasks[i];
        double percent = window > 0 ? 100.0 * (t->ulRunTimeCounter - run_time_before(before, t->xHandle)) / window : 0;
        BaseType_t core = xTaskGetCoreID(t->xHandle);
        if (!strncmp(t->pcTaskName, "IDLE", 4) && core >= 0 && core < CPU_CORES) idle[core] = percent;
        add_task(tasks, t, core, percent);
    }
}

static void add_frame_timing(cJSON *root)
{
    frame_timing_t timing;
    control_get_timing(&timing);
    cJSON *frame = cJSON_AddObjectToObject(root, "frame");
    cJSON_AddNumberToObject(frame, "fps", tenths(timing.fps));
    cJSON_AddNumberToObject(frame, "render_us", round(timing.render_us));
    cJSON_AddNumberToObject(frame, "strip_us", round(timing.strip_us));
    cJSON_AddNumberToObject(frame, "screen_us", round(timing.screen_us));
    cJSON_AddNumberToObject(frame, "total_us", round(timing.frame_us));
}

static esp_err_t cpu_get(httpd_req_t *req)
{
    static task_sample_t before, after;
    sample_tasks(&before);
    vTaskDelay(pdMS_TO_TICKS(1000));
    sample_tasks(&after);

    cJSON *root = cJSON_CreateObject();
    double idle[CPU_CORES] = { 0, 0 };
    add_tasks(root, &before, &after, idle);
    cJSON *cores = cJSON_AddArrayToObject(root, "core_load");
    for (int c = 0; c < CPU_CORES; c++)
        cJSON_AddItemToArray(cores, cJSON_CreateNumber(tenths(100 - idle[c])));
    add_frame_timing(root);
    cJSON_AddNumberToObject(root, "heap_free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    return send_cjson(req, root);
}

static void add_with(httpd_handle_t server, const char *uri, httpd_method_t method,
                     esp_err_t (*handler)(httpd_req_t *), void *context)
{
    httpd_uri_t route = { .uri = uri, .method = method, .handler = handler, .user_ctx = context };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
}

static void add(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    add_with(server, uri, method, handler, NULL);
}

void web_start(httpd_handle_t server)
{
    add(server, "/",             HTTP_GET,    root_get);
    for (size_t i = 0; i < sizeof(STATIC_FILES) / sizeof(STATIC_FILES[0]); i++)
        add_with(server, STATIC_FILES[i].uri, HTTP_GET, static_file_get, (void *)&STATIC_FILES[i]);
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
