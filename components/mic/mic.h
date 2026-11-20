#pragma once

// INMP441 I2S MEMS microphone. Reads in a background task and keeps a sound
// level for meters. Logs both I2S channels' levels once a second, which shows
// whether sound is arriving and which channel the mic talks on.

#define MIC_SAMPLE_RATE 22050

// Starts the reader task on the given core
void mic_start(int pin_sck, int pin_ws, int pin_sd, int core);

// Sound level for a meter: 0 = quiet room (about -70dBFS), 1 = very loud (-10dBFS).
// Rises at once and falls back gently, like a VU meter.
float mic_level(void);
