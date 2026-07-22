#pragma once

#include "baca/common.h"

#include <stdbool.h>
#include <stddef.h>

/* Begin before curses setup, activate when setup is complete, and finish after
 * terminal cleanup. */
bool baca_terminal_runtime_begin(BacaError *error);
bool baca_terminal_runtime_activate(BacaError *error);
bool baca_terminal_runtime_interrupted(void);
int baca_terminal_runtime_finish(int result, BacaError *error);

double baca_terminal_monotonic_seconds(void);
bool baca_terminal_graphics_write(void *user_data, const void *data,
                                  size_t length);
void baca_terminal_probe_cell_pixels(bool pixel_mode, int *width, int *height);
