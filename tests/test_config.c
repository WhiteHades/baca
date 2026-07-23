#include "test_support.h"

#include "baca/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static BacaTestResult load_config_text(const char *name, const char *text, BacaConfig *config) {
    BacaString relative = {0};
    BacaError error = {0};
    if (!baca_string_append(&relative, "config/", &error) || !baca_string_append(&relative, name, &error) ||
        !baca_string_append(&relative, ".ini", &error)) {
        baca_string_free(&relative);
        return baca_test_fail_at(__FILE__, __LINE__, "could not build config fixture path: %s", error.message);
    }
    if (!baca_test_write_text(relative.data, text)) {
        baca_string_free(&relative);
        return baca_test_fail_at(__FILE__, __LINE__, "could not write config fixture");
    }
    char *path = baca_test_path(relative.data);
    baca_string_free(&relative);
    if (path == NULL) {
        return baca_test_fail_at(__FILE__, __LINE__, "could not resolve config fixture path");
    }
    bool loaded = baca_config_load_path(config, path, &error);
    free(path);
    if (!loaded) {
        return baca_test_fail_at(__FILE__, __LINE__, "config load failed: %s", error.message);
    }
    return BACA_TEST_PASS;
}

static BacaTestResult test_defaults(void) {
    BacaConfig config = {0};
    BacaTestResult result = load_config_text("defaults", "", &config);
    TEST_ASSERT(result == BACA_TEST_PASS);
    TEST_ASSERT_STR(config.preferred_image_viewer, "auto");
    TEST_ASSERT_STR(config.library_path, "auto");
    TEST_ASSERT_INT(config.max_text_width, 80);
    TEST_ASSERT(!config.max_text_width_percent);
    TEST_ASSERT_INT(config.justification, BACA_JUSTIFY_FULL);
    TEST_ASSERT(!config.pretty);
    TEST_ASSERT_DOUBLE(config.page_scroll_duration, 0.2, 1e-12);
    TEST_ASSERT_INT(config.image_mode, BACA_IMAGE_MODE_AUTO);
    TEST_ASSERT(!config.image_mode_explicit);
    TEST_ASSERT(config.show_image_as_ansi);
    TEST_ASSERT_INT((int)config.dark.background, 0x1d1c2b);
    TEST_ASSERT_INT((int)config.dark.foreground, 0xcdd6f4);
    TEST_ASSERT_INT((int)config.dark.accent, 0xcba6f7);
    TEST_ASSERT_INT((int)config.light.foreground, 0x4c4f69);
    TEST_ASSERT_SIZE(config.keymaps.toggle_pdf_view.length, 1U);
    TEST_ASSERT_STR(config.keymaps.toggle_pdf_view.items[0], "v");
    TEST_ASSERT_SIZE(config.keymaps.page_down.length, 4U);
    TEST_ASSERT_STR(config.keymaps.page_down.items[0], "ctrl+f");
    TEST_ASSERT_STR(config.keymaps.page_down.items[3], "space");
    TEST_ASSERT_SIZE(config.keymaps.home.length, 2U);
    TEST_ASSERT_STR(config.keymaps.home.items[1], "gg");
    TEST_ASSERT_SIZE(config.keymaps.add_bookmark.length, 1U);
    TEST_ASSERT_STR(config.keymaps.add_bookmark.items[0], "b");
    TEST_ASSERT_SIZE(config.keymaps.open_bookmarks.length, 1U);
    TEST_ASSERT_STR(config.keymaps.open_bookmarks.items[0], "B");
    TEST_ASSERT_SIZE(config.keymaps.open_help.length, 2U);
    TEST_ASSERT_STR(config.keymaps.open_help.items[0], "question_mark");
    TEST_ASSERT_STR(config.keymaps.search_backward.items[0], "f2");
    TEST_ASSERT_INT(baca_config_content_width(&config, 120), 80);
    baca_config_free(&config);
    return BACA_TEST_PASS;
}

static BacaTestResult test_partial_case_insensitive_overrides(void) {
    static const char text[] =
        "[gEnErAl]\n"
        "preferredimageviewer = viu\n"
        "maxtextwidth = 73\n"
        "textjustification = RIGHT\n"
        "pretty = TrUe\n"
        "pagescrollduration = 1.25\n"
        "showimageasansi = OFF\n"
        "[kEyMaPs]\n"
        "scrolldown = down, J , ctrl+n\n";
    BacaConfig config = {0};
    BacaTestResult result = load_config_text("partial", text, &config);
    TEST_ASSERT(result == BACA_TEST_PASS);
    TEST_ASSERT_STR(config.preferred_image_viewer, "viu");
    TEST_ASSERT_STR(config.library_path, "auto");
    TEST_ASSERT_INT(config.max_text_width, 73);
    TEST_ASSERT_INT(config.justification, BACA_JUSTIFY_RIGHT);
    TEST_ASSERT(config.pretty);
    TEST_ASSERT_DOUBLE(config.page_scroll_duration, 1.25, 1e-12);
    TEST_ASSERT_INT(config.image_mode, BACA_IMAGE_MODE_PLACEHOLDER);
    TEST_ASSERT(!config.image_mode_explicit);
    TEST_ASSERT(!config.show_image_as_ansi);
    TEST_ASSERT_SIZE(config.keymaps.scroll_down.length, 3U);
    TEST_ASSERT_STR(config.keymaps.scroll_down.items[0], "down");
    TEST_ASSERT_STR(config.keymaps.scroll_down.items[1], "J");
    TEST_ASSERT_STR(config.keymaps.scroll_down.items[2], "ctrl+n");
    TEST_ASSERT_SIZE(config.keymaps.scroll_up.length, 2U);
    baca_config_free(&config);
    return BACA_TEST_PASS;
}

static BacaTestResult test_percent_width_forms(void) {
    const char *values[] = {"90%", "90%%"};
    for (size_t index = 0U; index < BACA_ARRAY_LEN(values); index++) {
        BacaString text = {0};
        BacaError error = {0};
        TEST_ASSERT(baca_string_append(&text, "[General]\nMaxTextWidth = ", &error));
        TEST_ASSERT(baca_string_append(&text, values[index], &error));
        TEST_ASSERT(baca_string_append_char(&text, '\n', &error));
        BacaConfig config = {0};
        BacaTestResult result = load_config_text(index == 0U ? "percent" : "legacy-percent", text.data, &config);
        baca_string_free(&text);
        TEST_ASSERT(result == BACA_TEST_PASS);
        TEST_ASSERT_INT(config.max_text_width, 90);
        TEST_ASSERT(config.max_text_width_percent);
        TEST_ASSERT_INT(baca_config_content_width(&config, 101), 90);
        TEST_ASSERT_INT(baca_config_content_width(&config, 1), 1);
        baca_config_free(&config);
    }
    return BACA_TEST_PASS;
}

static BacaTestResult test_bool_spellings(void) {
    const char *truthy[] = {"1", "yes", "TRUE", "On"};
    const char *falsey[] = {"0", "no", "FALSE", "Off"};
    for (size_t index = 0U; index < BACA_ARRAY_LEN(truthy); index++) {
        char text[128];
        int length = snprintf(text, sizeof(text), "[General]\nPretty=%s\nShowImageAsANSI=%s\n", truthy[index],
                              falsey[index]);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(text));
        BacaConfig config = {0};
        BacaTestResult result = load_config_text(index == 0U ? "bool-1" :
                                                 index == 1U ? "bool-2" :
                                                 index == 2U ? "bool-3" : "bool-4",
                                                 text, &config);
        TEST_ASSERT(result == BACA_TEST_PASS);
        TEST_ASSERT(config.pretty);
        TEST_ASSERT(!config.show_image_as_ansi);
        baca_config_free(&config);
    }
    return BACA_TEST_PASS;
}

static BacaTestResult test_colors_and_invalid_fallback(void) {
    static const char text[] =
        "[Color Dark]\n"
        "Background = #AbC\n"
        "Foreground = ReD\n"
        "Accent = definitely-not-a-color\n"
        "[Color Light]\n"
        "Background = 123456\n"
        "Foreground = grey\n"
        "Accent = #00f\n";
    BacaConfig config = {0};
    BacaTestResult result = load_config_text("colors", text, &config);
    TEST_ASSERT(result == BACA_TEST_PASS);
    TEST_ASSERT_INT((int)config.dark.background, 0xaabbcc);
    TEST_ASSERT_INT((int)config.dark.foreground, 0xff0000);
    TEST_ASSERT_INT((int)config.dark.accent, 0xcba6f7);
    TEST_ASSERT_INT((int)config.light.background, 0x123456);
    TEST_ASSERT_INT((int)config.light.foreground, 0x808080);
    TEST_ASSERT_INT((int)config.light.accent, 0x0000ff);
    baca_config_free(&config);
    return BACA_TEST_PASS;
}

static BacaTestResult test_key_lists(void) {
    static const char text[] =
        "[Keymaps]\n"
        "ToggleLightDark = t, ctrl+t\n"
        "TogglePdfView = x, ctrl+x\n"
        "PageDown = space, pagedown , ctrl+f\n"
        "CloseOrQuit = q, escape, ctrl+c\n";
    BacaConfig config = {0};
    BacaTestResult result = load_config_text("keys", text, &config);
    TEST_ASSERT(result == BACA_TEST_PASS);
    TEST_ASSERT_SIZE(config.keymaps.toggle_dark.length, 2U);
    TEST_ASSERT_STR(config.keymaps.toggle_dark.items[1], "ctrl+t");
    TEST_ASSERT_SIZE(config.keymaps.toggle_pdf_view.length, 2U);
    TEST_ASSERT_STR(config.keymaps.toggle_pdf_view.items[0], "x");
    TEST_ASSERT_STR(config.keymaps.toggle_pdf_view.items[1], "ctrl+x");
    TEST_ASSERT_SIZE(config.keymaps.page_down.length, 3U);
    TEST_ASSERT_STR(config.keymaps.page_down.items[0], "space");
    TEST_ASSERT_STR(config.keymaps.page_down.items[2], "ctrl+f");
    TEST_ASSERT_SIZE(config.keymaps.close.length, 3U);
    TEST_ASSERT_STR(config.keymaps.close.items[2], "ctrl+c");
    baca_config_free(&config);
    return BACA_TEST_PASS;
}

static BacaTestResult test_image_modes_and_legacy_precedence(void) {
    static const struct {
        const char *value;
        BacaImageMode mode;
    } modes[] = {
        {"auto", BACA_IMAGE_MODE_AUTO},
        {"KITTY", BACA_IMAGE_MODE_KITTY},
        {"ansi", BACA_IMAGE_MODE_ANSI},
        {"Placeholder", BACA_IMAGE_MODE_PLACEHOLDER},
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(modes); ++index) {
        char text[128] = {0};
        const int length = snprintf(text, sizeof(text),
                                    "[General]\nImageMode=%s\nShowImageAsANSI=definitely-invalid\n",
                                    modes[index].value);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(text));
        BacaConfig config = {0};
        BacaTestResult result = load_config_text(index == 0U   ? "mode-auto"
                                                 : index == 1U ? "mode-kitty"
                                                 : index == 2U ? "mode-ansi"
                                                               : "mode-placeholder",
                                                 text, &config);
        TEST_ASSERT(result == BACA_TEST_PASS);
        TEST_ASSERT_INT(config.image_mode, modes[index].mode);
        TEST_ASSERT(config.image_mode_explicit);
        TEST_ASSERT(config.show_image_as_ansi == (modes[index].mode != BACA_IMAGE_MODE_PLACEHOLDER));
        baca_config_free(&config);
    }
    return BACA_TEST_PASS;
}

static BacaTestResult test_default_file_creation_is_isolated(void) {
    char *path = baca_test_path("xdg-config/mereader-tui/config.ini");
    TEST_ASSERT(path != NULL);
    BacaError error = {0};
    TEST_ASSERT(baca_remove_tree(path, &error));
    BacaConfig config = {0};
    TEST_ASSERT(baca_config_load(&config, &error));
    TEST_ASSERT(baca_file_exists(path));
    struct stat status;
    TEST_ASSERT(stat(path, &status) == 0);
    TEST_ASSERT((status.st_mode & 0777U) == 0600U);
    TEST_ASSERT_STR(config.preferred_image_viewer, "auto");
    TEST_ASSERT(strstr(baca_config_default_text(), "MaxTextWidth = 80") != NULL);
    TEST_ASSERT(strstr(baca_config_default_text(), "LibraryPath = auto") != NULL);
    TEST_ASSERT(strstr(baca_config_default_text(), "ImageMode = auto") != NULL);
    TEST_ASSERT(strstr(baca_config_default_text(), "TogglePdfView = v") != NULL);
    TEST_ASSERT(strstr(baca_config_default_text(), "AddBookmark = b") != NULL);
    TEST_ASSERT(strstr(baca_config_default_text(), "OpenBookmarks = B") != NULL);
    TEST_ASSERT(strstr(baca_config_default_text(), "OpenHelp = question_mark,f1") != NULL);
    baca_config_free(&config);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_save_library_path_preserves_config(void) {
    static const char text[] =
        "[General]\n"
        "Pretty = yes\n"
        "MaxTextWidth = 73\n"
        "[Color Dark]\n"
        "Accent = #abcdef\n"
        "[General]\n"
        "LibraryPath = auto\n";
    TEST_ASSERT(baca_test_write_text("xdg-config/mereader-tui/config.ini", text));
    BacaError error = {0};
    TEST_ASSERT(baca_config_save_library_path("/srv/100% books", &error));

    BacaConfig config = {0};
    TEST_ASSERT(baca_config_load(&config, &error));
    TEST_ASSERT_STR(config.library_path, "/srv/100% books");
    TEST_ASSERT(config.pretty);
    TEST_ASSERT_INT(config.max_text_width, 73);
    TEST_ASSERT_INT((int)config.dark.accent, 0xabcdef);
    baca_config_free(&config);

    char *path = baca_test_path("xdg-config/mereader-tui/config.ini");
    TEST_ASSERT(path != NULL);
    struct stat status;
    TEST_ASSERT(stat(path, &status) == 0);
    TEST_ASSERT((status.st_mode & 0777U) == 0600U);
    TEST_ASSERT(!baca_config_save_library_path("bad\npath", &error));
    TEST_ASSERT(!baca_config_save_library_path("/srv/trailing ", &error));
    free(path);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_config_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "defaults", .function = test_defaults},
        {.name = "partial_case_insensitive_overrides", .function = test_partial_case_insensitive_overrides},
        {.name = "percent_width_forms", .function = test_percent_width_forms},
        {.name = "bool_spellings", .function = test_bool_spellings},
        {.name = "colors_and_invalid_fallback", .function = test_colors_and_invalid_fallback},
        {.name = "key_lists", .function = test_key_lists},
        {.name = "image_modes_and_legacy_precedence", .function = test_image_modes_and_legacy_precedence},
        {.name = "default_file_creation_is_isolated", .function = test_default_file_creation_is_isolated},
        {.name = "save_library_path_preserves_config", .function = test_save_library_path_preserves_config},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
