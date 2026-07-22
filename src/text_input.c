#include "text_input.h"

#include "baca/common.h"

#include <ctype.h>
#include <curses.h>
#include <limits.h>
#include <string.h>
#include <wctype.h>

static size_t previous_utf8_boundary(const char *value, size_t offset) {
  if (offset == 0U) {
    return 0U;
  }
  --offset;
  while (offset > 0U && ((unsigned char)value[offset] & 0xc0U) == 0x80U) {
    --offset;
  }
  return offset;
}

static size_t next_utf8_boundary(const char *value, size_t length,
                                 size_t offset) {
  if (offset >= length) {
    return length;
  }
  int columns = 0;
  const size_t next = baca_utf8_next(value, length, offset, &columns);
  return next > offset ? next : offset + 1U;
}

static bool delete_range(BacaTextInput *input, size_t start, size_t end) {
  if (start >= end || end > input->length) {
    return false;
  }
  memmove(input->value + start, input->value + end, input->length - end + 1U);
  input->length -= end - start;
  input->cursor = start;
  return true;
}

static bool insert_character(BacaTextInput *input, wchar_t character) {
  char encoded[MB_LEN_MAX] = {0};
  mbstate_t conversion = {0};
  const size_t length = wcrtomb(encoded, character, &conversion);
  if (length == (size_t)-1 || input->length + length >= sizeof(input->value)) {
    return false;
  }
  memmove(input->value + input->cursor + length, input->value + input->cursor,
          input->length - input->cursor + 1U);
  memcpy(input->value + input->cursor, encoded, length);
  input->cursor += length;
  input->length += length;
  return true;
}

bool baca_text_input_apply(BacaTextInput *input, bool key_code, int code,
                           wchar_t character) {
  if ((key_code && code == KEY_BACKSPACE) ||
      (!key_code && (character == 8 || character == 127))) {
    const size_t start = previous_utf8_boundary(input->value, input->cursor);
    return delete_range(input, start, input->cursor);
  }
  if (key_code && code == KEY_DC) {
    const size_t end =
        next_utf8_boundary(input->value, input->length, input->cursor);
    return delete_range(input, input->cursor, end);
  }
  if (key_code && code == KEY_LEFT) {
    input->cursor = previous_utf8_boundary(input->value, input->cursor);
  } else if (key_code && code == KEY_RIGHT) {
    input->cursor =
        next_utf8_boundary(input->value, input->length, input->cursor);
  } else if ((key_code && code == KEY_HOME) || (!key_code && character == 1)) {
    input->cursor = 0U;
  } else if ((key_code && code == KEY_END) || (!key_code && character == 5)) {
    input->cursor = input->length;
  } else if (!key_code && character == 23) {
    size_t start = input->cursor;
    while (start > 0U &&
           isspace((unsigned char)input->value[start - 1U]) != 0) {
      --start;
    }
    while (start > 0U &&
           isspace((unsigned char)input->value[start - 1U]) == 0) {
      --start;
    }
    return delete_range(input, start, input->cursor);
  } else if (!key_code && iswprint((wint_t)character) != 0) {
    return insert_character(input, character);
  }
  return false;
}
