#include "test_support.h"

#include "mereader-tui/config.h"
#include "mereader-tui/keymap.h"

#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static MereaderTuiTestResult load_config_text(const char *name, const char *text, MereaderTuiConfig *config) {
    MereaderTuiString relative = {0};
    MereaderTuiError error = {0};
    if (!mereader_tui_string_append(&relative, "config/", &error) || !mereader_tui_string_append(&relative, name, &error) ||
        !mereader_tui_string_append(&relative, ".ini", &error)) {
        mereader_tui_string_free(&relative);
        return mereader_tui_test_fail_at(__FILE__, __LINE__, "could not build config fixture path: %s", error.message);
    }
    if (!mereader_tui_test_write_text(relative.data, text)) {
        mereader_tui_string_free(&relative);
        return mereader_tui_test_fail_at(__FILE__, __LINE__, "could not write config fixture");
    }
    char *path = mereader_tui_test_path(relative.data);
    mereader_tui_string_free(&relative);
    if (path == NULL) {
        return mereader_tui_test_fail_at(__FILE__, __LINE__, "could not resolve config fixture path");
    }
    bool loaded = mereader_tui_config_load_path(config, path, &error);
    free(path);
    if (!loaded) {
        return mereader_tui_test_fail_at(__FILE__, __LINE__, "config load failed: %s", error.message);
    }
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_defaults(void) {
    MereaderTuiConfig config = {0};
    MereaderTuiTestResult result = load_config_text("defaults", "", &config);
    TEST_ASSERT(result == MEREADER_TUI_TEST_PASS);
    TEST_ASSERT_STR(config.preferred_image_viewer, "auto");
    TEST_ASSERT_STR(config.library_path, "auto");
    TEST_ASSERT_INT(config.max_text_width, 80);
    TEST_ASSERT(!config.max_text_width_percent);
    TEST_ASSERT_INT(config.justification, MEREADER_TUI_JUSTIFY_FULL);
    TEST_ASSERT(!config.pretty);
    TEST_ASSERT_DOUBLE(config.page_scroll_duration, 0.2, 1e-12);
    TEST_ASSERT_INT(config.image_mode, MEREADER_TUI_IMAGE_MODE_AUTO);
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
    TEST_ASSERT_SIZE(config.library_keymaps.move_down.length, 2U);
    TEST_ASSERT_STR(config.library_keymaps.move_down.items[1], "j");
    TEST_ASSERT_SIZE(config.library_keymaps.first.length, 2U);
    TEST_ASSERT_STR(config.library_keymaps.first.items[1], "gg");
    TEST_ASSERT_SIZE(config.library_keymaps.find.length, 1U);
    TEST_ASSERT_STR(config.library_keymaps.find.items[0], "space space");
    TEST_ASSERT_SIZE(config.library_keymaps.quit.length, 1U);
    TEST_ASSERT_STR(config.library_keymaps.quit.items[0], "q");
    TEST_ASSERT_INT(mereader_tui_config_content_width(&config, 120), 80);
    mereader_tui_config_free(&config);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_partial_case_insensitive_overrides(void) {
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
    MereaderTuiConfig config = {0};
    MereaderTuiTestResult result = load_config_text("partial", text, &config);
    TEST_ASSERT(result == MEREADER_TUI_TEST_PASS);
    TEST_ASSERT_STR(config.preferred_image_viewer, "viu");
    TEST_ASSERT_STR(config.library_path, "auto");
    TEST_ASSERT_INT(config.max_text_width, 73);
    TEST_ASSERT_INT(config.justification, MEREADER_TUI_JUSTIFY_RIGHT);
    TEST_ASSERT(config.pretty);
    TEST_ASSERT_DOUBLE(config.page_scroll_duration, 1.25, 1e-12);
    TEST_ASSERT_INT(config.image_mode, MEREADER_TUI_IMAGE_MODE_PLACEHOLDER);
    TEST_ASSERT(!config.image_mode_explicit);
    TEST_ASSERT(!config.show_image_as_ansi);
    TEST_ASSERT_SIZE(config.keymaps.scroll_down.length, 3U);
    TEST_ASSERT_STR(config.keymaps.scroll_down.items[0], "down");
    TEST_ASSERT_STR(config.keymaps.scroll_down.items[1], "J");
    TEST_ASSERT_STR(config.keymaps.scroll_down.items[2], "ctrl+n");
    TEST_ASSERT_SIZE(config.keymaps.scroll_up.length, 2U);
    mereader_tui_config_free(&config);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_percent_width_forms(void) {
    const char *values[] = {"90%", "90%%"};
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(values); index++) {
        MereaderTuiString text = {0};
        MereaderTuiError error = {0};
        TEST_ASSERT(mereader_tui_string_append(&text, "[General]\nMaxTextWidth = ", &error));
        TEST_ASSERT(mereader_tui_string_append(&text, values[index], &error));
        TEST_ASSERT(mereader_tui_string_append_char(&text, '\n', &error));
        MereaderTuiConfig config = {0};
        MereaderTuiTestResult result = load_config_text(index == 0U ? "percent" : "legacy-percent", text.data, &config);
        mereader_tui_string_free(&text);
        TEST_ASSERT(result == MEREADER_TUI_TEST_PASS);
        TEST_ASSERT_INT(config.max_text_width, 90);
        TEST_ASSERT(config.max_text_width_percent);
        TEST_ASSERT_INT(mereader_tui_config_content_width(&config, 101), 90);
        TEST_ASSERT_INT(mereader_tui_config_content_width(&config, 1), 1);
        mereader_tui_config_free(&config);
    }
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_bool_spellings(void) {
    const char *truthy[] = {"1", "yes", "TRUE", "On"};
    const char *falsey[] = {"0", "no", "FALSE", "Off"};
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(truthy); index++) {
        char text[128];
        int length = snprintf(text, sizeof(text), "[General]\nPretty=%s\nShowImageAsANSI=%s\n", truthy[index],
                              falsey[index]);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(text));
        MereaderTuiConfig config = {0};
        MereaderTuiTestResult result = load_config_text(index == 0U ? "bool-1" :
                                                 index == 1U ? "bool-2" :
                                                 index == 2U ? "bool-3" : "bool-4",
                                                 text, &config);
        TEST_ASSERT(result == MEREADER_TUI_TEST_PASS);
        TEST_ASSERT(config.pretty);
        TEST_ASSERT(!config.show_image_as_ansi);
        mereader_tui_config_free(&config);
    }
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_colors_and_invalid_fallback(void) {
    static const char text[] =
        "[Color Dark]\n"
        "Background = #AbC\n"
        "Foreground = ReD\n"
        "Accent = definitely-not-a-color\n"
        "[Color Light]\n"
        "Background = 123456\n"
        "Foreground = grey\n"
        "Accent = #00f\n";
    MereaderTuiConfig config = {0};
    MereaderTuiTestResult result = load_config_text("colors", text, &config);
    TEST_ASSERT(result == MEREADER_TUI_TEST_PASS);
    TEST_ASSERT_INT((int)config.dark.background, 0xaabbcc);
    TEST_ASSERT_INT((int)config.dark.foreground, 0xff0000);
    TEST_ASSERT_INT((int)config.dark.accent, 0xcba6f7);
    TEST_ASSERT_INT((int)config.light.background, 0x123456);
    TEST_ASSERT_INT((int)config.light.foreground, 0x808080);
    TEST_ASSERT_INT((int)config.light.accent, 0x0000ff);
    mereader_tui_config_free(&config);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_key_lists(void) {
    static const char text[] =
        "[Keymaps]\n"
        "ToggleLightDark = t, ctrl+t\n"
        "TogglePdfView = x, ctrl+x\n"
        "PageDown = space, pagedown , ctrl+f\n"
        "CloseOrQuit = q, escape, ctrl+c\n"
        "[Library Keymaps]\n"
        "MoveDown = n, down\n"
        "Find = zz\n"
        "Quit = x\n";
    MereaderTuiConfig config = {0};
    MereaderTuiTestResult result = load_config_text("keys", text, &config);
    TEST_ASSERT(result == MEREADER_TUI_TEST_PASS);
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
    TEST_ASSERT_SIZE(config.library_keymaps.move_down.length, 2U);
    TEST_ASSERT_STR(config.library_keymaps.move_down.items[0], "n");
    TEST_ASSERT_STR(config.library_keymaps.find.items[0], "zz");
    TEST_ASSERT_STR(config.library_keymaps.quit.items[0], "x");
    mereader_tui_config_free(&config);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_library_key_sequences_and_conflicts(void) {
    MereaderTuiConfig config = {0};
    MereaderTuiTestResult result = load_config_text(
        "library-keys",
        "[Library Keymaps]\n"
        "First = zz\n"
        "Find = ctrl+x ctrl+y\n"
        "Quit = X\n",
        &config);
    TEST_ASSERT(result == MEREADER_TUI_TEST_PASS);

    const MereaderTuiKey z = {.character = L'z'};
    const MereaderTuiKey ctrl_x = {.character = 24};
    const MereaderTuiKey ctrl_y = {.character = 25};
    const MereaderTuiKey upper_x = {.character = L'X'};
    const MereaderTuiKey keypad_enter = {.key_code = true, .code = KEY_ENTER};
    TEST_ASSERT(mereader_tui_key_list_starts_sequence(&config.library_keymaps.first, &z));
    TEST_ASSERT(mereader_tui_key_list_matches_sequence(&config.library_keymaps.first, &z, &z));
    TEST_ASSERT(mereader_tui_key_list_starts_sequence(&config.library_keymaps.find, &ctrl_x));
    TEST_ASSERT(mereader_tui_key_list_matches_sequence(&config.library_keymaps.find, &ctrl_x, &ctrl_y));
    TEST_ASSERT(mereader_tui_key_list_matches(&config.library_keymaps.quit, &upper_x));
    TEST_ASSERT(mereader_tui_key_list_matches(&config.library_keymaps.confirm, &keypad_enter));
    mereader_tui_config_free(&config);

    char *path = mereader_tui_test_path("config/library-key-conflict.ini");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT(mereader_tui_test_write_text("config/library-key-conflict.ini",
                                             "[Library Keymaps]\nMoveDown = x\nQuit = x\n"));
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_config_load_path(&config, path, &error));
    TEST_ASSERT(strstr(error.message, "MoveDown conflicts with Library Keymaps.Quit") != NULL);
    free(path);

    path = mereader_tui_test_path("config/library-key-invalid.ini");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT(mereader_tui_test_write_text("config/library-key-invalid.ini",
                                             "[Library Keymaps]\nFind = ctrl+invalid\n"));
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_config_load_path(&config, path, &error));
    TEST_ASSERT(strstr(error.message, "invalid Library Keymaps.Find binding") != NULL);
    free(path);

    path = mereader_tui_test_path("config/library-key-modal-conflict.ini");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT(mereader_tui_test_write_text("config/library-key-modal-conflict.ini",
                                             "[Library Keymaps]\nConfirm = escape\n"));
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_config_load_path(&config, path, &error));
    TEST_ASSERT(strstr(error.message, "Close conflicts with Library Keymaps.Confirm") != NULL);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_image_modes_and_legacy_precedence(void) {
    static const struct {
        const char *value;
        MereaderTuiImageMode mode;
    } modes[] = {
        {"auto", MEREADER_TUI_IMAGE_MODE_AUTO},
        {"KITTY", MEREADER_TUI_IMAGE_MODE_KITTY},
        {"ansi", MEREADER_TUI_IMAGE_MODE_ANSI},
        {"Placeholder", MEREADER_TUI_IMAGE_MODE_PLACEHOLDER},
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(modes); ++index) {
        char text[128] = {0};
        const int length = snprintf(text, sizeof(text),
                                    "[General]\nImageMode=%s\nShowImageAsANSI=definitely-invalid\n",
                                    modes[index].value);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(text));
        MereaderTuiConfig config = {0};
        MereaderTuiTestResult result = load_config_text(index == 0U   ? "mode-auto"
                                                 : index == 1U ? "mode-kitty"
                                                 : index == 2U ? "mode-ansi"
                                                               : "mode-placeholder",
                                                 text, &config);
        TEST_ASSERT(result == MEREADER_TUI_TEST_PASS);
        TEST_ASSERT_INT(config.image_mode, modes[index].mode);
        TEST_ASSERT(config.image_mode_explicit);
        TEST_ASSERT(config.show_image_as_ansi == (modes[index].mode != MEREADER_TUI_IMAGE_MODE_PLACEHOLDER));
        mereader_tui_config_free(&config);
    }
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_default_file_creation_is_isolated(void) {
    char *path = mereader_tui_test_path("xdg-config/mereader-tui/config.ini");
    TEST_ASSERT(path != NULL);
    MereaderTuiError error = {0};
    TEST_ASSERT(mereader_tui_remove_tree(path, &error));
    MereaderTuiConfig config = {0};
    TEST_ASSERT(mereader_tui_config_load(&config, &error));
    TEST_ASSERT(mereader_tui_file_exists(path));
    struct stat status;
    TEST_ASSERT(stat(path, &status) == 0);
    TEST_ASSERT((status.st_mode & 0777U) == 0600U);
    TEST_ASSERT_STR(config.preferred_image_viewer, "auto");
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "MaxTextWidth = 80") != NULL);
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "LibraryPath = auto") != NULL);
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "ImageMode = auto") != NULL);
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "TogglePdfView = v") != NULL);
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "AddBookmark = b") != NULL);
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "OpenBookmarks = B") != NULL);
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "OpenHelp = question_mark,f1") != NULL);
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "[Library Keymaps]") != NULL);
    TEST_ASSERT(strstr(mereader_tui_config_default_text(), "Find = space space") != NULL);
    MereaderTuiBuffer shipped = {0};
    TEST_ASSERT(mereader_tui_read_file("resources/config.ini", &shipped, &error));
    TEST_ASSERT_STR((const char *)shipped.data, mereader_tui_config_default_text());
    mereader_tui_buffer_free(&shipped);
    mereader_tui_config_free(&config);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_save_library_path_preserves_config(void) {
    static const char text[] =
        "[General]\n"
        "Pretty = yes\n"
        "MaxTextWidth = 73\n"
        "[Color Dark]\n"
        "Accent = #abcdef\n"
        "[General]\n"
        "LibraryPath = auto\n";
    TEST_ASSERT(mereader_tui_test_write_text("xdg-config/mereader-tui/config.ini", text));
    MereaderTuiError error = {0};
    TEST_ASSERT(mereader_tui_config_save_library_path("/srv/100% books", &error));

    MereaderTuiConfig config = {0};
    TEST_ASSERT(mereader_tui_config_load(&config, &error));
    TEST_ASSERT_STR(config.library_path, "/srv/100% books");
    TEST_ASSERT(config.pretty);
    TEST_ASSERT_INT(config.max_text_width, 73);
    TEST_ASSERT_INT((int)config.dark.accent, 0xabcdef);
    mereader_tui_config_free(&config);

    char *path = mereader_tui_test_path("xdg-config/mereader-tui/config.ini");
    TEST_ASSERT(path != NULL);
    struct stat status;
    TEST_ASSERT(stat(path, &status) == 0);
    TEST_ASSERT((status.st_mode & 0777U) == 0600U);
    TEST_ASSERT(!mereader_tui_config_save_library_path("bad\npath", &error));
    TEST_ASSERT(!mereader_tui_config_save_library_path("/srv/trailing ", &error));
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_config_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "defaults", .function = test_defaults},
        {.name = "partial_case_insensitive_overrides", .function = test_partial_case_insensitive_overrides},
        {.name = "percent_width_forms", .function = test_percent_width_forms},
        {.name = "bool_spellings", .function = test_bool_spellings},
        {.name = "colors_and_invalid_fallback", .function = test_colors_and_invalid_fallback},
        {.name = "key_lists", .function = test_key_lists},
        {.name = "library_key_sequences_and_conflicts", .function = test_library_key_sequences_and_conflicts},
        {.name = "image_modes_and_legacy_precedence", .function = test_image_modes_and_legacy_precedence},
        {.name = "default_file_creation_is_isolated", .function = test_default_file_creation_is_isolated},
        {.name = "save_library_path_preserves_config", .function = test_save_library_path_preserves_config},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
