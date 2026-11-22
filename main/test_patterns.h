#pragma once

// Hardware tests, kept in C so they work whatever is in the patch library

#include "patch.h"

typedef enum { TEST_RGB, TEST_DIM, TEST_COUNT } test_pattern_t;

// Padded to 8 characters
const char *test_pattern_name(test_pattern_t pattern);

void test_pattern_render(test_pattern_t pattern, uint32_t ms, rgb16_t *frame, int count);

// Describes the current phase of DIM TEST, or NULL. Returns the same pointer
// for the whole phase, so callers can log changes.
const char *test_pattern_phase(test_pattern_t pattern, uint32_t ms);

// Whether the test ignores the brightness setting
bool test_pattern_full_scale(test_pattern_t pattern);
