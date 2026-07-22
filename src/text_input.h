#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

enum { BACA_TEXT_INPUT_CAPACITY = PATH_MAX };

typedef struct BacaTextInput {
  char value[BACA_TEXT_INPUT_CAPACITY];
  size_t length;
  size_t cursor;
} BacaTextInput;

/* Cursor-only operations return false; content insertions and deletions return
 * true. */
bool baca_text_input_apply(BacaTextInput *input, bool key_code, int code,
                           wchar_t character);
