#include "mereader-tui/keymap.h"

#include <ctype.h>
#include <curses.h>
#include <stdlib.h>
#include <string.h>

typedef enum MereaderTuiKeyTokenKind {
  MEREADER_TUI_KEY_TOKEN_NONE = 0,
  MEREADER_TUI_KEY_TOKEN_CHARACTER,
  MEREADER_TUI_KEY_TOKEN_CODE,
} MereaderTuiKeyTokenKind;

typedef struct MereaderTuiKeyToken {
  MereaderTuiKeyTokenKind kind;
  int value;
} MereaderTuiKeyToken;

typedef struct MereaderTuiKeyBinding {
  MereaderTuiKeyToken tokens[2];
  size_t length;
} MereaderTuiKeyBinding;

static bool key_name_equals(const char *name, size_t length,
                            const char *expected) {
  if (length != strlen(expected)) {
    return false;
  }
  for (size_t index = 0U; index < length; ++index) {
    if (tolower((unsigned char)name[index]) !=
        tolower((unsigned char)expected[index])) {
      return false;
    }
  }
  return true;
}

static bool key_token_parse(const char *name, size_t length,
                            MereaderTuiKeyToken *token) {
  if (length == 1U && (unsigned char)name[0] >= 0x20U &&
      (unsigned char)name[0] < 0x7fU) {
    *token = (MereaderTuiKeyToken){.kind = MEREADER_TUI_KEY_TOKEN_CHARACTER,
                                   .value = (unsigned char)name[0]};
    return true;
  }
  if (length == 6U && key_name_equals(name, 5U, "ctrl+") &&
      isalpha((unsigned char)name[5]) != 0) {
    *token = (MereaderTuiKeyToken){
        .kind = MEREADER_TUI_KEY_TOKEN_CHARACTER,
        .value = tolower((unsigned char)name[5]) - 'a' + 1,
    };
    return true;
  }
  if (length >= 2U && (name[0] == 'f' || name[0] == 'F')) {
    char number[4] = {0};
    if (length < sizeof(number)) {
      memcpy(number, name + 1, length - 1U);
      char *end = NULL;
      const long parsed = strtol(number, &end, 10);
      if (end != number && *end == '\0' && parsed >= 1L && parsed <= 63L) {
        *token = (MereaderTuiKeyToken){.kind = MEREADER_TUI_KEY_TOKEN_CODE,
                                       .value = KEY_F((int)parsed)};
        return true;
      }
    }
  }

  static const struct {
    const char *name;
    MereaderTuiKeyTokenKind kind;
    int value;
  } names[] = {
      {"down", MEREADER_TUI_KEY_TOKEN_CODE, KEY_DOWN},
      {"up", MEREADER_TUI_KEY_TOKEN_CODE, KEY_UP},
      {"left", MEREADER_TUI_KEY_TOKEN_CODE, KEY_LEFT},
      {"right", MEREADER_TUI_KEY_TOKEN_CODE, KEY_RIGHT},
      {"home", MEREADER_TUI_KEY_TOKEN_CODE, KEY_HOME},
      {"end", MEREADER_TUI_KEY_TOKEN_CODE, KEY_END},
      {"pageup", MEREADER_TUI_KEY_TOKEN_CODE, KEY_PPAGE},
      {"page_up", MEREADER_TUI_KEY_TOKEN_CODE, KEY_PPAGE},
      {"pagedown", MEREADER_TUI_KEY_TOKEN_CODE, KEY_NPAGE},
      {"page_down", MEREADER_TUI_KEY_TOKEN_CODE, KEY_NPAGE},
      {"enter", MEREADER_TUI_KEY_TOKEN_CHARACTER, '\n'},
      {"escape", MEREADER_TUI_KEY_TOKEN_CHARACTER, 27},
      {"esc", MEREADER_TUI_KEY_TOKEN_CHARACTER, 27},
      {"tab", MEREADER_TUI_KEY_TOKEN_CHARACTER, '\t'},
      {"space", MEREADER_TUI_KEY_TOKEN_CHARACTER, ' '},
      {"slash", MEREADER_TUI_KEY_TOKEN_CHARACTER, '/'},
      {"question_mark", MEREADER_TUI_KEY_TOKEN_CHARACTER, '?'},
      {"question", MEREADER_TUI_KEY_TOKEN_CHARACTER, '?'},
      {"backspace", MEREADER_TUI_KEY_TOKEN_CODE, KEY_BACKSPACE},
      {"delete", MEREADER_TUI_KEY_TOKEN_CODE, KEY_DC},
  };
  for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(names); ++index) {
    if (key_name_equals(name, length, names[index].name)) {
      *token = (MereaderTuiKeyToken){.kind = names[index].kind,
                                     .value = names[index].value};
      return true;
    }
  }
  return false;
}

static bool key_binding_parse(const char *name,
                              MereaderTuiKeyBinding *binding) {
  if (name == NULL || name[0] == '\0') {
    return false;
  }
  const size_t length = strlen(name);
  MereaderTuiKeyToken single = {0};
  if (key_token_parse(name, length, &single)) {
    *binding = (MereaderTuiKeyBinding){.tokens = {single}, .length = 1U};
    return true;
  }

  const char *separator = strchr(name, ' ');
  if (separator != NULL) {
    const size_t first_length = (size_t)(separator - name);
    const char *second = separator + 1;
    const size_t second_length = strlen(second);
    if (first_length == 0U || second_length == 0U ||
        strchr(second, ' ') != NULL ||
        !key_token_parse(name, first_length, &binding->tokens[0]) ||
        !key_token_parse(second, second_length, &binding->tokens[1])) {
      return false;
    }
    binding->length = 2U;
    return true;
  }
  if (length == 2U && key_token_parse(name, 1U, &binding->tokens[0]) &&
      key_token_parse(name + 1, 1U, &binding->tokens[1])) {
    binding->length = 2U;
    return true;
  }
  return false;
}

static bool key_token_matches(const MereaderTuiKeyToken *token,
                              const MereaderTuiKey *key) {
  if (token->kind == MEREADER_TUI_KEY_TOKEN_CODE) {
    if (token->value == KEY_BACKSPACE) {
      return (key->key_code && key->code == KEY_BACKSPACE) ||
             (!key->key_code && (key->character == 8 || key->character == 127));
    }
    return key->key_code && key->code == token->value;
  }
  if (token->kind != MEREADER_TUI_KEY_TOKEN_CHARACTER || key->key_code) {
    return token->value == '\n' && key->key_code && key->code == KEY_ENTER;
  }
  if (token->value == '\n') {
    return key->character == L'\n' || key->character == L'\r';
  }
  return key->character == (wchar_t)token->value;
}

bool mereader_tui_key_name_matches(const char *name,
                                   const MereaderTuiKey *key) {
  MereaderTuiKeyBinding binding = {0};
  return key != NULL && key_binding_parse(name, &binding) &&
         binding.length == 1U && key_token_matches(&binding.tokens[0], key);
}

bool mereader_tui_key_list_matches(const MereaderTuiKeyList *list,
                                   const MereaderTuiKey *key) {
  if (list == NULL) {
    return false;
  }
  for (size_t index = 0U; index < list->length; ++index) {
    if (mereader_tui_key_name_matches(list->items[index], key)) {
      return true;
    }
  }
  return false;
}

bool mereader_tui_key_list_starts_sequence(const MereaderTuiKeyList *list,
                                           const MereaderTuiKey *key) {
  if (list == NULL || key == NULL) {
    return false;
  }
  for (size_t index = 0U; index < list->length; ++index) {
    MereaderTuiKeyBinding binding = {0};
    if (key_binding_parse(list->items[index], &binding) &&
        binding.length == 2U && key_token_matches(&binding.tokens[0], key)) {
      return true;
    }
  }
  return false;
}

bool mereader_tui_key_list_matches_sequence(const MereaderTuiKeyList *list,
                                            const MereaderTuiKey *first,
                                            const MereaderTuiKey *second) {
  if (list == NULL || first == NULL || second == NULL) {
    return false;
  }
  for (size_t index = 0U; index < list->length; ++index) {
    MereaderTuiKeyBinding binding = {0};
    if (key_binding_parse(list->items[index], &binding) &&
        binding.length == 2U && key_token_matches(&binding.tokens[0], first) &&
        key_token_matches(&binding.tokens[1], second)) {
      return true;
    }
  }
  return false;
}

bool mereader_tui_key_binding_valid(const char *name) {
  MereaderTuiKeyBinding binding = {0};
  return key_binding_parse(name, &binding);
}

static bool key_tokens_conflict(const MereaderTuiKeyToken *left,
                                const MereaderTuiKeyToken *right) {
  if (left->kind == right->kind && left->value == right->value) {
    return true;
  }
  if (left->kind == MEREADER_TUI_KEY_TOKEN_CODE &&
      left->value == KEY_BACKSPACE &&
      right->kind == MEREADER_TUI_KEY_TOKEN_CHARACTER &&
      (right->value == 8 || right->value == 127)) {
    return true;
  }
  if (right->kind == MEREADER_TUI_KEY_TOKEN_CODE &&
      right->value == KEY_BACKSPACE &&
      left->kind == MEREADER_TUI_KEY_TOKEN_CHARACTER &&
      (left->value == 8 || left->value == 127)) {
    return true;
  }
  return left->kind == MEREADER_TUI_KEY_TOKEN_CHARACTER &&
         right->kind == MEREADER_TUI_KEY_TOKEN_CHARACTER &&
         ((left->value == '\n' && right->value == '\r') ||
          (left->value == '\r' && right->value == '\n'));
}

bool mereader_tui_key_lists_conflict(const MereaderTuiKeyList *left,
                                     const MereaderTuiKeyList *right) {
  if (left == NULL || right == NULL) {
    return false;
  }
  for (size_t left_index = 0U; left_index < left->length; ++left_index) {
    MereaderTuiKeyBinding left_binding = {0};
    if (!key_binding_parse(left->items[left_index], &left_binding)) {
      continue;
    }
    for (size_t right_index = 0U; right_index < right->length; ++right_index) {
      MereaderTuiKeyBinding right_binding = {0};
      if (!key_binding_parse(right->items[right_index], &right_binding)) {
        continue;
      }
      if (key_tokens_conflict(&left_binding.tokens[0],
                              &right_binding.tokens[0]) &&
          (left_binding.length == 1U || right_binding.length == 1U ||
           key_tokens_conflict(&left_binding.tokens[1],
                               &right_binding.tokens[1]))) {
        return true;
      }
    }
  }
  return false;
}

const char *mereader_tui_key_display_name(const char *name) {
  if (name == NULL) {
    return "";
  }
  if (mereader_tui_casecmp(name, "question_mark") == 0 ||
      mereader_tui_casecmp(name, "question") == 0) {
    return "?";
  }
  if (mereader_tui_casecmp(name, "escape") == 0) {
    return "esc";
  }
  if (mereader_tui_casecmp(name, "page_down") == 0) {
    return "pagedown";
  }
  if (mereader_tui_casecmp(name, "page_up") == 0) {
    return "pageup";
  }
  return name;
}
