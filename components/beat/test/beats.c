// Runs WAV files through the beat tracker. Usage:
//   beats <in.wav> [clicks.wav]
// The input must be 16-bit PCM at 22050Hz (test/run.sh converts with afconvert).
// Prints tempo and confidence every second and the beat times; with a second
// file, writes the input mixed with a click on every beat, to listen to.
#include "beat.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t *read_wav(const char *path, int *count, int *rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    uint8_t header[12];
    fread(header, 1, 12, f);
    int channels = 1, bits = 16;
    int16_t *out = NULL;
    for (;;) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, f) != 8) break;
        uint32_t size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (uint32_t)chunk[7] << 24;
        if (!memcmp(chunk, "fmt ", 4)) {
            uint8_t fmt[64];
            fread(fmt, 1, size < 64 ? size : 64, f);
            if (size > 64) fseek(f, size - 64, SEEK_CUR);
            channels = fmt[2] | fmt[3] << 8;
            *rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | fmt[7] << 24;
            bits = fmt[14] | fmt[15] << 8;
        } else if (!memcmp(chunk, "data", 4)) {
            if (bits != 16) { fprintf(stderr, "need 16-bit PCM\n"); exit(2); }
            int frames = size / (2 * channels);
            int16_t *raw = malloc(size);
            fread(raw, 1, size, f);
            out = malloc(frames * sizeof(int16_t));
            for (int i = 0; i < frames; i++) {
                int sum = 0;
                for (int c = 0; c < channels; c++) sum += raw[i * channels + c];
                out[i] = sum / channels;
            }
            free(raw);
            *count = frames;
            break;
        } else {
            fseek(f, size + (size & 1), SEEK_CUR);
        }
    }
    fclose(f);
    if (!out) { fprintf(stderr, "%s: no audio\n", path); exit(2); }
    return out;
}

static void put32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }

static void write_wav(const char *path, const int16_t *s, int n, int rate)
{
    FILE *f = fopen(path, "wb");
    fwrite("RIFF", 1, 4, f); put32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, rate); put32(f, rate * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, n * 2); fwrite(s, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: beats <in.wav> [clicks.wav]\n"); return 2; }
    int n, rate;
    int16_t *audio = read_wav(argv[1], &n, &rate);
    if (rate != 22050) { fprintf(stderr, "need 22050Hz, got %d\n", rate); return 2; }
    beat_init(rate);
    int16_t *clicks = argc > 2 ? calloc(n, sizeof(int16_t)) : NULL;
    float block[BEAT_HOP];
    double last_beat = 0;
    int next_report = rate;
    for (int start = 0; start + BEAT_HOP <= n; start += BEAT_HOP) {
        for (int i = 0; i < BEAT_HOP; i++) block[i] = audio[start + i] / 32768.0f;
        beat_process(block);
        beat_state_t st;
        beat_get(&st);
        double t = (start + BEAT_HOP) / (double)rate;
        if (floor(st.position) != floor(last_beat) && !st.holding) {
            printf("beat %.3f\n", t);
            if (clicks)   // a short 2kHz tick
                for (int i = 0; i < 400 && start + BEAT_HOP + i < n; i++)
                    clicks[start + BEAT_HOP + i] = (int16_t)(12000 * sinf(2 * (float)M_PI * 2000 * i / rate) * expf(-i / 80.0f));
        }
        last_beat = st.position;
        if (start >= next_report) {
            printf("time %.0f bpm %.1f confidence %.2f %s\n", t, st.bpm, st.confidence, st.holding ? "holding" : "");
            next_report += rate;
        }
    }
    if (clicks) {
        for (int i = 0; i < n; i++) {
            int v = audio[i] / 2 + clicks[i];
            clicks[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        write_wav(argv[2], clicks, n, rate);
    }
    return 0;
}
