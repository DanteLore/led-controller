#pragma once

// Hardware tests, kept in C so they work whatever is in the patch library

#include "patch.h"

typedef enum { TEST_RGB, TEST_DIM, TEST_COUNT } test_t;

// Padded to 8 characters
const char *effects_test_name(test_t test);

void effects_render_test(test_t test, uint32_t ms, rgb16_t *frame, int count);

// Describes the current phase of DIM TEST, or NULL. Returns the same pointer
// for the whole phase, so callers can log changes.
const char *effects_test_phase(test_t test, uint32_t ms);

// Whether the test ignores the brightness setting
bool effects_test_full_scale(test_t test);
