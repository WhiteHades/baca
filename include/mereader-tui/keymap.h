#pragma once

#include "mereader-tui/config.h"

#include <wchar.h>

typedef struct MereaderTuiKey {
    bool key_code;
    int code;
    wchar_t character;
} MereaderTuiKey;

[[nodiscard]] bool mereader_tui_key_name_matches(const char *name, const MereaderTuiKey *key);
[[nodiscard]] bool mereader_tui_key_list_matches(const MereaderTuiKeyList *list, const MereaderTuiKey *key);
[[nodiscard]] bool mereader_tui_key_list_starts_sequence(const MereaderTuiKeyList *list, const MereaderTuiKey *key);
[[nodiscard]] bool mereader_tui_key_list_matches_sequence(const MereaderTuiKeyList *list, const MereaderTuiKey *first,
                                                  const MereaderTuiKey *second);
[[nodiscard]] bool mereader_tui_key_binding_valid(const char *name);
[[nodiscard]] bool mereader_tui_key_lists_conflict(const MereaderTuiKeyList *left, const MereaderTuiKeyList *right);
[[nodiscard]] const char *mereader_tui_key_display_name(const char *name);
