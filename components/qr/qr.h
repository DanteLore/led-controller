#pragma once

// A minimal QR code encoder: just enough for the controller's web address or
// its setup hotspot, so a phone camera can open or join it from the LCD.
// Versions 2 and 3 (25×25 or 29×29), byte mode, medium error correction,
// fixed mask. Plain C, no dependencies.

#include <stdbool.h>

#define QR_SIZE_MAX  29
#define QR_TEXT_MAX  42   // bytes that fit in the bigger version

// Fills modules (true = dark, [row][column]) and returns the code's width in
// modules, or 0 if text is too long. Leave a light border of 4 modules round
// it when drawing.
int qr_encode(const char *text, bool modules[QR_SIZE_MAX][QR_SIZE_MAX]);
