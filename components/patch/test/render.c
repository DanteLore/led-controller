// Renders every patch in a file at a spread of times and prints each LED, so
// the output can be compared with render.js. Usage: render <file.json> <leds>
#include "patch.h"
#include <stdio.h>
#include <stdlib.h>

static const uint32_t TIMES_MS[] = { 0, 1, 250, 999, 1000, 4321, 12345, 29999, 30000, 61234, 89999, 123456, 3600000, 86400017 };

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: render <file.json> <leds>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static char json[PATCH_JSON_MAX + 1];
    size_t n = fread(json, 1, PATCH_JSON_MAX, f);
    json[n] = 0;
    fclose(f);

    static patch_set_t set;
    char error[160];
    if (!patch_parse(json, &set, error, sizeof(error))) {
        printf("error %s\n", error);
        return 1;
    }
    int count = atoi(argv[2]);
    rgb16_t *frame = calloc(count, sizeof(*frame));
    for (int p = 0; p < set.count; p++)
        for (size_t t = 0; t < sizeof(TIMES_MS) / sizeof(TIMES_MS[0]); t++) {
            patch_render(&set, p, TIMES_MS[t], frame, count);
            for (int i = 0; i < count; i++)
                printf("%s %lu %d %u %u %u\n", set.patches[p].name, (unsigned long)TIMES_MS[t], i,
                       frame[i].r, frame[i].g, frame[i].b);
        }
    return 0;
}
