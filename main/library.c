#include "library.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

#define NVS_NAMESPACE "leds"
#define NVS_KEY       "patches"

static const char *TAG = "library";

extern const char presets_start[] asm("_binary_presets_json_start");
extern const char presets_end[]   asm("_binary_presets_json_end");

static SemaphoreHandle_t lock;
static patch_set_t patches, draft;
static bool draft_playing;
static int draft_index;
static uint32_t version;
static char *json_text;   // what patches was read from; NULL = the presets

void library_lock(void)   { xSemaphoreTake(lock, portMAX_DELAY); }
void library_unlock(void) { xSemaphoreGive(lock); }

const patch_set_t *library_patches(void) { return &patches; }
const patch_set_t *library_draft(void)   { return draft_playing ? &draft : NULL; }
int library_draft_index(void)            { return draft_index; }
uint32_t library_version(void)           { return version; }

const char *library_presets(size_t *length)
{
    *length = presets_end - presets_start;
    return presets_start;
}

// Embedded files aren't NUL-terminated
static char *presets_copy(void)
{
    size_t n;
    const char *p = library_presets(&n);
    char *s = malloc(n + 1);
    if (s) {
        memcpy(s, p, n);
        s[n] = 0;
    }
    return s;
}

static char *load_saved(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return NULL;
    size_t size = 0;
    char *s = NULL;
    if (nvs_get_blob(nvs, NVS_KEY, NULL, &size) == ESP_OK && size > 0 && (s = malloc(size + 1))) {
        if (nvs_get_blob(nvs, NVS_KEY, s, &size) == ESP_OK) {
            s[size] = 0;
        } else {
            free(s);
            s = NULL;
        }
    }
    nvs_close(nvs);
    return s;
}

static esp_err_t store(const char *json)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(nvs, NVS_KEY, json, strlen(json));
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

void library_init(void)
{
    lock = xSemaphoreCreateMutex();
    char error[160];
    char *saved = load_saved();
    if (saved && patch_parse(saved, &patches, error, sizeof(error))) {
        json_text = saved;
        ESP_LOGI(TAG, "Loaded %d saved patches", patches.count);
        return;
    }
    if (saved) {
        ESP_LOGW(TAG, "Saved patches won't read (%s), using the presets", error);
        free(saved);
    }
    char *presets = presets_copy();
    if (!presets || !patch_parse(presets, &patches, error, sizeof(error))) {
        // Only possible if presets.json itself is broken: caught on the bench
        ESP_LOGE(TAG, "Presets won't read: %s", presets ? error : "out of memory");
        abort();
    }
    free(presets);
    ESP_LOGI(TAG, "Using the %d preset patches", patches.count);
}

static bool check(const char *json, patch_set_t **out, char *error, size_t error_size)
{
    *out = malloc(sizeof(patch_set_t));
    if (!*out) {
        strlcpy(error, "out of memory", error_size);
        return false;
    }
    if (patch_parse(json, *out, error, error_size))
        return true;
    free(*out);
    return false;
}

bool library_save(const char *json, char *error, size_t error_size)
{
    patch_set_t *set;
    char *copy = strdup(json);
    if (!copy || !check(json, &set, error, error_size)) {
        if (!copy) strlcpy(error, "out of memory", error_size);
        free(copy);
        return false;
    }
    // Flash first, so the library in use always matches what's saved
    esp_err_t err = store(json);
    if (err != ESP_OK) {
        snprintf(error, error_size, "couldn't save to flash (%s)", esp_err_to_name(err));
        free(set);
        free(copy);
        return false;
    }
    library_lock();
    patches = *set;
    draft_playing = false;
    free(json_text);
    json_text = copy;
    version++;
    library_unlock();
    free(set);
    ESP_LOGI(TAG, "Saved %d patches", patches.count);
    return true;
}

bool library_play_draft(const char *json, int index, char *error, size_t error_size)
{
    patch_set_t *set;
    if (!check(json, &set, error, error_size))
        return false;
    if (index < 0 || index >= set->count) {
        snprintf(error, error_size, "no patch %d to play", index + 1);
        free(set);
        return false;
    }
    library_lock();
    draft = *set;
    draft_index = index;
    draft_playing = true;
    library_unlock();
    free(set);
    return true;
}

void library_stop_draft(void)
{
    library_lock();
    draft_playing = false;
    library_unlock();
}

char *library_json(void)
{
    library_lock();
    char *s = json_text ? strdup(json_text) : presets_copy();
    library_unlock();
    return s;
}
