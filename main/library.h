#pragma once

// The patch library: the patches the button steps through. Saved in flash
// (NVS); the built-in presets are used until something is saved, or if what
// was saved can't be read.
//
// The web editor can also play a draft: an unsaved copy of the whole library
// with one patch picked to show, so edits appear on the strip as they're made.

#include "patch.h"
#include <stdbool.h>
#include <stddef.h>

void library_init(void);

// The LED task holds the lock while it reads the patches and draft
void library_lock(void);
void library_unlock(void);
const patch_set_t *library_patches(void);
const patch_set_t *library_draft(void);   // NULL unless a draft is playing
int library_draft_index(void);
uint32_t library_version(void);           // changes whenever the library is saved

// For the web server. Each checks the JSON and returns false with a message if
// it can't be read; nothing changes then.
bool library_save(const char *json, char *error, size_t error_size);
bool library_play_draft(const char *json, int index, char *error, size_t error_size);
void library_stop_draft(void);

// The library as JSON text, or NULL if out of memory; the caller frees it
char *library_json(void);
const char *library_presets(size_t *length);
