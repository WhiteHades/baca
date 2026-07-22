#pragma once

#include "baca/common.h"

typedef struct BacaColorScheme {
    uint32_t background;
    uint32_t foreground;
    uint32_t accent;
} BacaColorScheme;

typedef struct BacaKeyList {
    char **items;
    size_t length;
} BacaKeyList;

typedef struct BacaKeymaps {
    BacaKeyList toggle_dark;
    BacaKeyList toggle_pdf_view;
    BacaKeyList scroll_down;
    BacaKeyList scroll_up;
    BacaKeyList page_down;
    BacaKeyList page_up;
    BacaKeyList home;
    BacaKeyList end;
    BacaKeyList open_toc;
    BacaKeyList add_bookmark;
    BacaKeyList open_bookmarks;
    BacaKeyList open_metadata;
    BacaKeyList open_help;
    BacaKeyList search_forward;
    BacaKeyList search_backward;
    BacaKeyList next_match;
    BacaKeyList previous_match;
    BacaKeyList confirm;
    BacaKeyList close;
    BacaKeyList screenshot;
} BacaKeymaps;

typedef enum BacaJustification : uint8_t {
    BACA_JUSTIFY_LEFT = 0,
    BACA_JUSTIFY_FULL,
    BACA_JUSTIFY_CENTER,
    BACA_JUSTIFY_RIGHT,
} BacaJustification;

typedef enum BacaImageMode : uint8_t {
    BACA_IMAGE_MODE_AUTO = 0,
    BACA_IMAGE_MODE_KITTY,
    BACA_IMAGE_MODE_ANSI,
    BACA_IMAGE_MODE_PLACEHOLDER,
} BacaImageMode;

typedef struct BacaConfig {
    char *preferred_image_viewer;
    char *library_path;
    int max_text_width;
    bool max_text_width_percent;
    BacaJustification justification;
    bool pretty;
    double page_scroll_duration;
    BacaImageMode image_mode;
    bool image_mode_explicit;
    bool show_image_as_ansi;
    BacaColorScheme dark;
    BacaColorScheme light;
    BacaKeymaps keymaps;
} BacaConfig;

[[nodiscard]] bool baca_config_load(BacaConfig *config, BacaError *error);
[[nodiscard]] bool baca_config_load_path(BacaConfig *config, const char *path, BacaError *error);
void baca_config_free(BacaConfig *config);
[[nodiscard]] int baca_config_content_width(const BacaConfig *config, int terminal_width);
[[nodiscard]] const char *baca_config_default_text(void);
