#pragma once

// What the LCD shows, as the switch picks it: Play shows what's playing,
// Setup the address as a QR code, Edit a placeholder. Only what's changed is
// redrawn. Does nothing on boards without an LCD.

#include "beat.h"
#include "inputs.h"
#include "player.h"

void ui_init(void);
void ui_update(int64_t now_us, panel_t panel, const player_status_t *player, const beat_state_t *beat,
               const rgb16_t *frame, int count);
