#pragma once

#include "mereader-tui/common.h"

typedef struct MereaderTuiColorScheme {
    uint32_t background;
    uint32_t foreground;
    uint32_t accent;
} MereaderTuiColorScheme;

typedef struct MereaderTuiKeyList {
    char **items;
    size_t length;
} MereaderTuiKeyList;

typedef struct MereaderTuiKeymaps {
    MereaderTuiKeyList toggle_dark;
    MereaderTuiKeyList toggle_pdf_view;
    MereaderTuiKeyList scroll_down;
    MereaderTuiKeyList scroll_up;
    MereaderTuiKeyList page_down;
    MereaderTuiKeyList page_up;
    MereaderTuiKeyList home;
    MereaderTuiKeyList end;
    MereaderTuiKeyList open_toc;
    MereaderTuiKeyList add_bookmark;
    MereaderTuiKeyList open_bookmarks;
    MereaderTuiKeyList open_metadata;
    MereaderTuiKeyList open_help;
    MereaderTuiKeyList search_forward;
    MereaderTuiKeyList search_backward;
    MereaderTuiKeyList next_match;
    MereaderTuiKeyList previous_match;
    MereaderTuiKeyList confirm;
    MereaderTuiKeyList close;
    MereaderTuiKeyList screenshot;
} MereaderTuiKeymaps;

typedef struct MereaderTuiLibraryKeymaps {
    MereaderTuiKeyList move_down;
    MereaderTuiKeyList move_up;
    MereaderTuiKeyList page_down;
    MereaderTuiKeyList page_up;
    MereaderTuiKeyList first;
    MereaderTuiKeyList last;
    MereaderTuiKeyList open;
    MereaderTuiKeyList back;
    MereaderTuiKeyList toggle_view;
    MereaderTuiKeyList toggle_shelf;
    MereaderTuiKeyList choose_format;
    MereaderTuiKeyList sort;
    MereaderTuiKeyList refresh;
    MereaderTuiKeyList open_path;
    MereaderTuiKeyList filter;
    MereaderTuiKeyList find;
    MereaderTuiKeyList help;
    MereaderTuiKeyList confirm;
    MereaderTuiKeyList close;
    MereaderTuiKeyList quit;
} MereaderTuiLibraryKeymaps;

typedef enum MereaderTuiJustification : uint8_t {
    MEREADER_TUI_JUSTIFY_LEFT = 0,
    MEREADER_TUI_JUSTIFY_FULL,
    MEREADER_TUI_JUSTIFY_CENTER,
    MEREADER_TUI_JUSTIFY_RIGHT,
} MereaderTuiJustification;

typedef enum MereaderTuiImageMode : uint8_t {
    MEREADER_TUI_IMAGE_MODE_AUTO = 0,
    MEREADER_TUI_IMAGE_MODE_KITTY,
    MEREADER_TUI_IMAGE_MODE_ANSI,
    MEREADER_TUI_IMAGE_MODE_PLACEHOLDER,
} MereaderTuiImageMode;

typedef struct MereaderTuiConfig {
    char *preferred_image_viewer;
    char *library_path;
    int max_text_width;
    bool max_text_width_percent;
    MereaderTuiJustification justification;
    bool pretty;
    double page_scroll_duration;
    MereaderTuiImageMode image_mode;
    bool image_mode_explicit;
    bool show_image_as_ansi;
    MereaderTuiColorScheme dark;
    MereaderTuiColorScheme light;
    MereaderTuiKeymaps keymaps;
    MereaderTuiLibraryKeymaps library_keymaps;
} MereaderTuiConfig;

[[nodiscard]] bool mereader_tui_config_load(MereaderTuiConfig *config, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_config_load_path(MereaderTuiConfig *config, const char *path, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_config_save_library_path(const char *library_path, MereaderTuiError *error);
void mereader_tui_config_free(MereaderTuiConfig *config);
[[nodiscard]] int mereader_tui_config_content_width(const MereaderTuiConfig *config, int terminal_width);
[[nodiscard]] const char *mereader_tui_config_default_text(void);
