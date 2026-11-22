#pragma once

// What's wired to which pin: see HARDWARE.md

#include "led_strip.h"

// Strip — count chips, not LEDs (24V strips usually have 6 LEDs per chip)
#define LED_PIN     13
#define LED_COUNT   10
#define LED_ORDER   LED_ORDER_BRG

// Left knob: brightness; press for the previous patch
#define LEFT_PIN_CLK   33
#define LEFT_PIN_DT    27
#define LEFT_PIN_SW    14
// Right knob: speed; press for the next patch
#define RIGHT_PIN_CLK  19
#define RIGHT_PIN_DT   21
#define RIGHT_PIN_SW   22

// Three-position switch, common to GND. Neither side connected (the middle) is play.
#define SWITCH_PIN_SETUP  16   // position A
#define SWITCH_PIN_EDIT   17   // position B

// INMP441 microphone, L/R tied to GND
#define MIC_PIN_SCK  26
#define MIC_PIN_WS   25
#define MIC_PIN_SD   35
