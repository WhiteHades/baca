#include "mereader-tui/config.h"
#include "mereader-tui/keymap.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct MereaderTuiIniEntry {
    char *section;
    char *key;
    char *value;
} MereaderTuiIniEntry;

typedef struct MereaderTuiIni {
    MereaderTuiIniEntry *items;
    size_t length;
    size_t capacity;
} MereaderTuiIni;

static const char MEREADER_TUI_DEFAULT_CONFIG[] =
    "[General]\n"
    "# pick your favorite image viewer\n"
    "PreferredImageViewer = auto\n"
    "# auto checks ~/Calibre Library and ~/Documents/Calibre Library\n"
    "# or set an absolute path to a calibre library or ordinary book folder\n"
    "LibraryPath = auto\n"
    "\n"
    "# int or css value string like 90%%\n"
    "# (escape percent with double percent %%)\n"
    "MaxTextWidth = 80\n"
    "\n"
    "# 'justify', 'center', 'left', 'right'\n"
    "TextJustification = justify\n"
    "\n"
    "# currently using pretty=yes is slow\n"
    "# and taking huge amount of memory\n"
    "Pretty = no\n"
    "\n"
    "PageScrollDuration = 0.2\n"
    "\n"
    "# auto, kitty, ansi, or placeholder\n"
    "# oversized, animated, or corrupt images are shown as placeholders\n"
    "# pdf fixed-page rendering follows this mode; press v for reflowable text\n"
    "ImageMode = auto\n"
    "\n"
    "# colors accept #rgb, #rrggbb, or common names\n"
    "[Color Dark]\n"
    "Background = #1d1c2b\n"
    "Foreground = #cdd6f4\n"
    "Accent = #cba6f7\n"
    "\n"
    "[Color Light]\n"
    "Background = #eff1f5\n"
    "Foreground = #4c4f69\n"
    "Accent = #8839ef\n"
    "\n"
    "[Keymaps]\n"
    "ToggleLightDark = c\n"
    "TogglePdfView = v\n"
    "ScrollDown = down,j\n"
    "ScrollUp = up,k\n"
    "PageDown = ctrl+f,pagedown,l,space\n"
    "PageUp = ctrl+b,pageup,h\n"
    "Home = home,gg\n"
    "End = end,G\n"
    "OpenToc = tab\n"
    "AddBookmark = b\n"
    "OpenBookmarks = B\n"
    "OpenMetadata = M\n"
    "OpenHelp = question_mark,f1\n"
    "SearchForward = slash\n"
    "SearchBackward = f2\n"
    "NextMatch = n\n"
    "PreviousMatch = N\n"
    "Confirm = enter\n"
    "CloseOrQuit = q,escape\n"
    "Screenshot = f12\n"
    "\n"
    "[Library Keymaps]\n"
    "MoveDown = down,j\n"
    "MoveUp = up,k\n"
    "PageDown = ctrl+f,pagedown\n"
    "PageUp = ctrl+b,pageup\n"
    "First = home,gg\n"
    "Last = end,G\n"
    "Open = enter,l\n"
    "Back = backspace,h\n"
    "ToggleView = v\n"
    "ToggleShelf = a\n"
    "ChooseFormat = f\n"
    "Sort = s\n"
    "Refresh = r\n"
    "OpenPath = o\n"
    "Filter = slash\n"
    "Find = space space\n"
    "Help = question_mark,f1\n"
    "Confirm = enter\n"
    "Close = escape\n"
    "Quit = q\n";

static void mereader_tui_ini_free(MereaderTuiIni *ini) {
    if (ini == nullptr) {
        return;
    }
    for (size_t index = 0U; index < ini->length; ++index) {
        free(ini->items[index].section);
        free(ini->items[index].key);
        free(ini->items[index].value);
    }
    free(ini->items);
    *ini = (MereaderTuiIni){0};
}

static char *mereader_tui_ini_copy_trimmed(const char *start, const char *end, MereaderTuiError *error) {
    while (start < end && isspace((unsigned char)*start)) {
        ++start;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        --end;
    }
    return mereader_tui_strndup(start, (size_t)(end - start), error);
}

static char *mereader_tui_ini_unescape_percent(const char *value, MereaderTuiError *error) {
    MereaderTuiString result = {0};
    for (size_t index = 0U; value[index] != '\0'; ++index) {
        if (value[index] == '%' && value[index + 1U] == '%') {
            if (!mereader_tui_string_append_char(&result, '%', error)) {
                mereader_tui_string_free(&result);
                return nullptr;
            }
            ++index;
        } else if (!mereader_tui_string_append_char(&result, value[index], error)) {
            mereader_tui_string_free(&result);
            return nullptr;
        }
    }
    if (!mereader_tui_string_append_n(&result, "", 0U, error)) {
        mereader_tui_string_free(&result);
        return nullptr;
    }
    return mereader_tui_string_take(&result);
}

static bool mereader_tui_ini_add(MereaderTuiIni *ini, const char *section, const char *key, const char *value, size_t line_number,
                         MereaderTuiError *error) {
    for (size_t index = 0U; index < ini->length; ++index) {
        if (mereader_tui_casecmp(ini->items[index].section, section) == 0 &&
            mereader_tui_casecmp(ini->items[index].key, key) == 0) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "duplicate INI option '%s.%s' on line %zu", section, key,
                           line_number);
            return false;
        }
    }

    MereaderTuiError reserve_error = {0};
    MereaderTuiIniEntry *items = mereader_tui_array_reserve(ini->items, &ini->capacity, sizeof(*ini->items), ini->length + 1U,
                                             &reserve_error);
    if (mereader_tui_error_is_set(&reserve_error)) {
        if (error != nullptr) {
            *error = reserve_error;
        }
        return false;
    }
    ini->items = items;

    MereaderTuiIniEntry entry = {0};
    entry.section = mereader_tui_strdup(section, error);
    entry.key = mereader_tui_strdup(key, error);
    entry.value = mereader_tui_ini_unescape_percent(value, error);
    if (entry.section == nullptr || entry.key == nullptr || entry.value == nullptr) {
        free(entry.section);
        free(entry.key);
        free(entry.value);
        return false;
    }
    ini->items[ini->length++] = entry;
    return true;
}

static bool mereader_tui_ini_parse(const unsigned char *data, size_t length, MereaderTuiIni *ini, MereaderTuiError *error) {
    char *section = nullptr;
    size_t offset = 0U;
    size_t line_number = 0U;

    while (offset < length) {
        size_t line_start = offset;
        while (offset < length && data[offset] != '\n') {
            ++offset;
        }
        size_t line_end = offset;
        if (offset < length) {
            ++offset;
        }
        ++line_number;
        if (line_end > line_start && data[line_end - 1U] == '\r') {
            --line_end;
        }
        if (memchr(data + line_start, '\0', line_end - line_start) != nullptr) {
            free(section);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "INI contains a null byte on line %zu", line_number);
            return false;
        }

        const char *start = (const char *)data + line_start;
        const char *end = (const char *)data + line_end;
        while (start < end && isspace((unsigned char)*start)) {
            ++start;
        }
        if (start == end || *start == '#' || *start == ';') {
            continue;
        }

        if (*start == '[') {
            const char *closing = memchr(start + 1, ']', (size_t)(end - start - 1));
            if (closing == nullptr) {
                free(section);
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "unterminated INI section on line %zu", line_number);
                return false;
            }
            const char *trailing = closing + 1;
            while (trailing < end && isspace((unsigned char)*trailing)) {
                ++trailing;
            }
            if (trailing < end && *trailing != '#' && *trailing != ';') {
                free(section);
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "unexpected text after INI section on line %zu",
                               line_number);
                return false;
            }

            char *new_section = mereader_tui_ini_copy_trimmed(start + 1, closing, error);
            if (new_section == nullptr) {
                free(section);
                return false;
            }
            if (new_section[0] == '\0') {
                free(new_section);
                free(section);
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "empty INI section on line %zu", line_number);
                return false;
            }
            free(section);
            section = new_section;
            continue;
        }

        if (section == nullptr) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "INI option appears before a section on line %zu",
                           line_number);
            return false;
        }

        const char *equals = memchr(start, '=', (size_t)(end - start));
        const char *colon = memchr(start, ':', (size_t)(end - start));
        const char *delimiter = equals;
        if (delimiter == nullptr || (colon != nullptr && colon < delimiter)) {
            delimiter = colon;
        }
        if (delimiter == nullptr) {
            free(section);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "INI option has no delimiter on line %zu", line_number);
            return false;
        }

        char *key = mereader_tui_ini_copy_trimmed(start, delimiter, error);
        char *value = mereader_tui_ini_copy_trimmed(delimiter + 1, end, error);
        if (key == nullptr || value == nullptr) {
            free(key);
            free(value);
            free(section);
            return false;
        }
        if (key[0] == '\0') {
            free(key);
            free(value);
            free(section);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "empty INI option on line %zu", line_number);
            return false;
        }
        bool added = mereader_tui_ini_add(ini, section, key, value, line_number, error);
        free(key);
        free(value);
        if (!added) {
            free(section);
            return false;
        }
    }

    free(section);
    return true;
}

static const char *mereader_tui_ini_get(const MereaderTuiIni *ini, const char *section, const char *key,
                                const char *fallback) {
    for (size_t index = 0U; index < ini->length; ++index) {
        if (mereader_tui_casecmp(ini->items[index].section, section) == 0 &&
            mereader_tui_casecmp(ini->items[index].key, key) == 0) {
            return ini->items[index].value;
        }
    }
    for (size_t index = 0U; index < ini->length; ++index) {
        if (mereader_tui_casecmp(ini->items[index].section, "DEFAULT") == 0 &&
            mereader_tui_casecmp(ini->items[index].key, key) == 0) {
            return ini->items[index].value;
        }
    }
    return fallback;
}

static bool mereader_tui_config_parse_positive_width(const char *value, int *width, bool *percent) {
    errno = 0;
    char *end = nullptr;
    long parsed = strtol(value, &end, 10);
    bool is_percent = end != nullptr && end[0] == '%' && end[1] == '\0';
    bool valid = errno == 0 && end != value && (*end == '\0' || is_percent) && parsed > 0 && parsed <= INT_MAX;
    if (!valid) {
        return false;
    }

    *width = (int)parsed;
    *percent = is_percent;
    return true;
}

static bool mereader_tui_config_parse_justification(const char *value, MereaderTuiJustification *justification,
                                            MereaderTuiError *error) {
    if (mereader_tui_casecmp(value, "justify") == 0 || mereader_tui_casecmp(value, "full") == 0) {
        *justification = MEREADER_TUI_JUSTIFY_FULL;
    } else if (mereader_tui_casecmp(value, "center") == 0) {
        *justification = MEREADER_TUI_JUSTIFY_CENTER;
    } else if (mereader_tui_casecmp(value, "right") == 0) {
        *justification = MEREADER_TUI_JUSTIFY_RIGHT;
    } else if (mereader_tui_casecmp(value, "left") == 0 || mereader_tui_casecmp(value, "default") == 0) {
        *justification = MEREADER_TUI_JUSTIFY_LEFT;
    } else {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid General.TextJustification value '%s'", value);
        return false;
    }
    return true;
}

static bool mereader_tui_config_parse_image_mode(const char *value, MereaderTuiImageMode *mode, MereaderTuiError *error) {
    if (mereader_tui_casecmp(value, "auto") == 0) {
        *mode = MEREADER_TUI_IMAGE_MODE_AUTO;
    } else if (mereader_tui_casecmp(value, "kitty") == 0) {
        *mode = MEREADER_TUI_IMAGE_MODE_KITTY;
    } else if (mereader_tui_casecmp(value, "ansi") == 0) {
        *mode = MEREADER_TUI_IMAGE_MODE_ANSI;
    } else if (mereader_tui_casecmp(value, "placeholder") == 0) {
        *mode = MEREADER_TUI_IMAGE_MODE_PLACEHOLDER;
    } else {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid General.ImageMode value '%s'", value);
        return false;
    }
    return true;
}

static bool mereader_tui_config_parse_bool(const char *section, const char *key, const char *value, bool *result,
                                   MereaderTuiError *error) {
    if (mereader_tui_casecmp(value, "1") == 0 || mereader_tui_casecmp(value, "yes") == 0 ||
        mereader_tui_casecmp(value, "true") == 0 || mereader_tui_casecmp(value, "on") == 0) {
        *result = true;
        return true;
    }
    if (mereader_tui_casecmp(value, "0") == 0 || mereader_tui_casecmp(value, "no") == 0 ||
        mereader_tui_casecmp(value, "false") == 0 || mereader_tui_casecmp(value, "off") == 0) {
        *result = false;
        return true;
    }
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid %s.%s boolean value '%s'", section, key, value);
    return false;
}

static bool mereader_tui_config_parse_duration(const char *value, double *duration, MereaderTuiError *error) {
    const unsigned char *cursor = (const unsigned char *)value;
    bool negative = false;
    if (*cursor == '+' || *cursor == '-') {
        negative = *cursor == '-';
        ++cursor;
    }

    double parsed = 0.0;
    bool has_digit = false;
    while (isdigit(*cursor)) {
        parsed = parsed * 10.0 + (double)(*cursor - '0');
        has_digit = true;
        ++cursor;
    }
    if (*cursor == '.') {
        ++cursor;
        double place = 0.1;
        while (isdigit(*cursor)) {
            parsed += (double)(*cursor - '0') * place;
            place *= 0.1;
            has_digit = true;
            ++cursor;
        }
    }

    int exponent = 0;
    bool exponent_negative = false;
    if (*cursor == 'e' || *cursor == 'E') {
        ++cursor;
        if (*cursor == '+' || *cursor == '-') {
            exponent_negative = *cursor == '-';
            ++cursor;
        }
        if (!isdigit(*cursor)) {
            has_digit = false;
        }
        while (isdigit(*cursor)) {
            if (exponent <= 10000) {
                exponent = exponent * 10 + (int)(*cursor - '0');
            }
            ++cursor;
        }
    }
    if (exponent_negative) {
        exponent = -exponent;
    }
    if (exponent != 0 && parsed != 0.0) {
        parsed *= pow(10.0, (double)exponent);
    }
    if (negative) {
        parsed = -parsed;
    }

    if (!has_digit || *cursor != '\0' || !isfinite(parsed) || parsed < 0.0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid General.PageScrollDuration value '%s'", value);
        return false;
    }
    *duration = parsed;
    return true;
}

static int mereader_tui_config_hex_digit(unsigned char character) {
    if (character >= '0' && character <= '9') {
        return (int)(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return (int)(character - 'a') + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return (int)(character - 'A') + 10;
    }
    return -1;
}

static bool mereader_tui_config_parse_color(const char *value, uint32_t *color) {
    const char *digits = value[0] == '#' ? value + 1 : value;
    size_t digit_count = strlen(digits);
    if (digit_count == 6U || (value[0] == '#' && digit_count == 3U)) {
        uint32_t parsed = 0U;
        bool valid = true;
        for (size_t index = 0U; index < digit_count; ++index) {
            int digit = mereader_tui_config_hex_digit((unsigned char)digits[index]);
            if (digit < 0) {
                valid = false;
                break;
            }
            if (digit_count == 3U) {
                parsed = parsed * 256U + (uint32_t)((unsigned)digit * 17U);
            } else {
                parsed = parsed * 16U + (uint32_t)(unsigned)digit;
            }
        }
        if (valid) {
            *color = parsed;
            return true;
        }
    }

    static const struct {
        const char *name;
        uint32_t value;
    } named_colors[] = {
        {"black", 0x000000U},   {"silver", 0xc0c0c0U}, {"gray", 0x808080U},
        {"grey", 0x808080U},    {"white", 0xffffffU},  {"maroon", 0x800000U},
        {"red", 0xff0000U},     {"purple", 0x800080U}, {"fuchsia", 0xff00ffU},
        {"magenta", 0xff00ffU}, {"green", 0x008000U},  {"lime", 0x00ff00U},
        {"olive", 0x808000U},   {"yellow", 0xffff00U}, {"navy", 0x000080U},
        {"blue", 0x0000ffU},    {"teal", 0x008080U},   {"aqua", 0x00ffffU},
        {"cyan", 0x00ffffU},    {"orange", 0xffa500U},
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(named_colors); ++index) {
        if (mereader_tui_casecmp(value, named_colors[index].name) == 0) {
            *color = named_colors[index].value;
            return true;
        }
    }
    return false;
}

static void mereader_tui_key_list_free(MereaderTuiKeyList *list) {
    if (list == nullptr) {
        return;
    }
    for (size_t index = 0U; index < list->length; ++index) {
        free(list->items[index]);
    }
    free(list->items);
    *list = (MereaderTuiKeyList){0};
}

static bool mereader_tui_config_parse_key_list(const char *value, MereaderTuiKeyList *list, MereaderTuiError *error) {
    size_t count = 1U;
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor == ',') {
            if (count == SIZE_MAX) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "keymap item count overflow");
                return false;
            }
            ++count;
        }
    }

    char **items = mereader_tui_reallocarray(nullptr, count, sizeof(*items), error);
    if (items == nullptr) {
        return false;
    }
    memset(items, 0, count * sizeof(*items));

    const char *start = value;
    size_t index = 0U;
    for (const char *cursor = value;; ++cursor) {
        if (*cursor != ',' && *cursor != '\0') {
            continue;
        }
        items[index] = mereader_tui_ini_copy_trimmed(start, cursor, error);
        if (items[index] == nullptr) {
            MereaderTuiKeyList partial = {.items = items, .length = index};
            mereader_tui_key_list_free(&partial);
            return false;
        }
        ++index;
        if (*cursor == '\0') {
            break;
        }
        start = cursor + 1;
    }

    list->items = items;
    list->length = count;
    return true;
}

static bool mereader_tui_config_validate_key_list(const char *name, const MereaderTuiKeyList *list,
                                                  MereaderTuiError *error) {
    for (size_t index = 0U; index < list->length; ++index) {
        if (!mereader_tui_key_binding_valid(list->items[index])) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                                   "invalid Library Keymaps.%s binding '%s'", name, list->items[index]);
            return false;
        }
    }
    return true;
}

static bool mereader_tui_config_validate_library_keymaps(const MereaderTuiLibraryKeymaps *maps,
                                                         MereaderTuiError *error) {
    static const char *const names[] = {
        "MoveDown", "MoveUp", "PageDown", "PageUp", "First", "Last", "Open", "Back", "ToggleView", "ToggleShelf",
        "ChooseFormat", "Sort", "Refresh", "OpenPath", "Filter", "Find", "Help", "Close", "Quit", "Confirm",
    };
    const MereaderTuiKeyList *const lists[] = {
        &maps->move_down, &maps->move_up, &maps->page_down, &maps->page_up, &maps->first, &maps->last, &maps->open,
        &maps->back, &maps->toggle_view, &maps->toggle_shelf, &maps->choose_format, &maps->sort, &maps->refresh,
        &maps->open_path, &maps->filter, &maps->find, &maps->help, &maps->close, &maps->quit, &maps->confirm,
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(lists); ++index) {
        if (!mereader_tui_config_validate_key_list(names[index], lists[index], error)) {
            return false;
        }
    }

    const size_t base_count = MEREADER_TUI_ARRAY_LEN(lists) - 1U;
    for (size_t left = 0U; left < base_count; ++left) {
        for (size_t right = left + 1U; right < base_count; ++right) {
            if (mereader_tui_key_lists_conflict(lists[left], lists[right])) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                                       "Library Keymaps.%s conflicts with Library Keymaps.%s",
                                       names[left], names[right]);
                return false;
            }
        }
    }

    if (mereader_tui_key_lists_conflict(&maps->close, &maps->confirm)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                               "Library Keymaps.Close conflicts with Library Keymaps.Confirm");
        return false;
    }
    return true;
}

static bool mereader_tui_config_build(const MereaderTuiIni *ini, MereaderTuiConfig *config, MereaderTuiError *error) {
    MereaderTuiConfig result = {
        .max_text_width = 80,
        .justification = MEREADER_TUI_JUSTIFY_LEFT,
        .dark = {.background = 0x1d1c2bU, .foreground = 0xcdd6f4U, .accent = 0xcba6f7U},
        .light = {.background = 0xeff1f5U, .foreground = 0x4c4f69U, .accent = 0x8839efU},
    };
    (void)mereader_tui_config_parse_positive_width(mereader_tui_ini_get(ini, "General", "MaxTextWidth", "80"),
                                           &result.max_text_width, &result.max_text_width_percent);
    (void)mereader_tui_config_parse_color(mereader_tui_ini_get(ini, "Color Dark", "Background", "#1d1c2b"),
                                  &result.dark.background);
    (void)mereader_tui_config_parse_color(mereader_tui_ini_get(ini, "Color Dark", "Foreground", "#cdd6f4"),
                                  &result.dark.foreground);
    (void)mereader_tui_config_parse_color(mereader_tui_ini_get(ini, "Color Dark", "Accent", "#cba6f7"),
                                  &result.dark.accent);
    (void)mereader_tui_config_parse_color(mereader_tui_ini_get(ini, "Color Light", "Background", "#eff1f5"),
                                  &result.light.background);
    (void)mereader_tui_config_parse_color(mereader_tui_ini_get(ini, "Color Light", "Foreground", "#4c4f69"),
                                  &result.light.foreground);
    (void)mereader_tui_config_parse_color(mereader_tui_ini_get(ini, "Color Light", "Accent", "#8839ef"),
                                  &result.light.accent);

    const char *image_mode = mereader_tui_ini_get(ini, "General", "ImageMode", nullptr);
    if (image_mode != nullptr) {
        result.image_mode_explicit = true;
        if (!mereader_tui_config_parse_image_mode(image_mode, &result.image_mode, error)) {
            mereader_tui_config_free(&result);
            return false;
        }
        result.show_image_as_ansi = result.image_mode != MEREADER_TUI_IMAGE_MODE_PLACEHOLDER;
    } else if (!mereader_tui_config_parse_bool("General", "ShowImageAsANSI",
                                       mereader_tui_ini_get(ini, "General", "ShowImageAsANSI", "yes"),
                                       &result.show_image_as_ansi, error)) {
        mereader_tui_config_free(&result);
        return false;
    } else {
        result.image_mode = result.show_image_as_ansi ? MEREADER_TUI_IMAGE_MODE_AUTO : MEREADER_TUI_IMAGE_MODE_PLACEHOLDER;
    }

    result.preferred_image_viewer =
        mereader_tui_strdup(mereader_tui_ini_get(ini, "General", "PreferredImageViewer", "auto"), error);
    result.library_path = mereader_tui_strdup(mereader_tui_ini_get(ini, "General", "LibraryPath", "auto"), error);
    if (result.preferred_image_viewer == nullptr || result.library_path == nullptr ||
        !mereader_tui_config_parse_justification(mereader_tui_ini_get(ini, "General", "TextJustification", "justify"),
                                         &result.justification, error) ||
        !mereader_tui_config_parse_bool("General", "Pretty", mereader_tui_ini_get(ini, "General", "Pretty", "no"),
                                &result.pretty, error) ||
        !mereader_tui_config_parse_duration(mereader_tui_ini_get(ini, "General", "PageScrollDuration", "0.2"),
                                    &result.page_scroll_duration, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "ToggleLightDark", "c"),
                                    &result.keymaps.toggle_dark, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "TogglePdfView", "v"),
                                    &result.keymaps.toggle_pdf_view, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "ScrollDown", "down,j"),
                                    &result.keymaps.scroll_down, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "ScrollUp", "up,k"),
                                    &result.keymaps.scroll_up, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "PageDown", "ctrl+f,pagedown,l,space"),
                                    &result.keymaps.page_down, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "PageUp", "ctrl+b,pageup,h"),
                                    &result.keymaps.page_up, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "Home", "home,gg"), &result.keymaps.home,
                                    error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "End", "end,G"), &result.keymaps.end,
                                    error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "OpenToc", "tab"),
                                    &result.keymaps.open_toc, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "AddBookmark", "b"),
                                    &result.keymaps.add_bookmark, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "OpenBookmarks", "B"),
                                    &result.keymaps.open_bookmarks, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "OpenMetadata", "M"),
                                    &result.keymaps.open_metadata, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "OpenHelp", "question_mark,f1"),
                                    &result.keymaps.open_help, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "SearchForward", "slash"),
                                    &result.keymaps.search_forward, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "SearchBackward", "f2"),
                                    &result.keymaps.search_backward, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "NextMatch", "n"),
                                    &result.keymaps.next_match, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "PreviousMatch", "N"),
                                    &result.keymaps.previous_match, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "Confirm", "enter"),
                                    &result.keymaps.confirm, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "CloseOrQuit", "q,escape"),
                                    &result.keymaps.close, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Keymaps", "Screenshot", "f12"),
                                    &result.keymaps.screenshot, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "MoveDown", "down,j"),
                                    &result.library_keymaps.move_down, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "MoveUp", "up,k"),
                                    &result.library_keymaps.move_up, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "PageDown", "ctrl+f,pagedown"),
                                    &result.library_keymaps.page_down, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "PageUp", "ctrl+b,pageup"),
                                    &result.library_keymaps.page_up, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "First", "home,gg"),
                                    &result.library_keymaps.first, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Last", "end,G"),
                                    &result.library_keymaps.last, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Open", "enter,l"),
                                    &result.library_keymaps.open, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Back", "backspace,h"),
                                    &result.library_keymaps.back, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "ToggleView", "v"),
                                    &result.library_keymaps.toggle_view, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "ToggleShelf", "a"),
                                    &result.library_keymaps.toggle_shelf, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "ChooseFormat", "f"),
                                    &result.library_keymaps.choose_format, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Sort", "s"),
                                    &result.library_keymaps.sort, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Refresh", "r"),
                                    &result.library_keymaps.refresh, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "OpenPath", "o"),
                                    &result.library_keymaps.open_path, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Filter", "slash"),
                                    &result.library_keymaps.filter, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Find", "space space"),
                                    &result.library_keymaps.find, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Help", "question_mark,f1"),
                                    &result.library_keymaps.help, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Confirm", "enter"),
                                    &result.library_keymaps.confirm, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Close", "escape"),
                                    &result.library_keymaps.close, error) ||
        !mereader_tui_config_parse_key_list(mereader_tui_ini_get(ini, "Library Keymaps", "Quit", "q"),
                                    &result.library_keymaps.quit, error)) {
        mereader_tui_config_free(&result);
        return false;
    }

    if (!mereader_tui_config_validate_library_keymaps(&result.library_keymaps, error)) {
        mereader_tui_config_free(&result);
        return false;
    }
    *config = result;
    return true;
}

static bool mereader_tui_config_output_empty(const MereaderTuiConfig *config) {
    if (config->preferred_image_viewer != nullptr || config->library_path != nullptr || config->max_text_width != 0 ||
        config->max_text_width_percent || config->justification != MEREADER_TUI_JUSTIFY_LEFT || config->pretty ||
        config->page_scroll_duration != 0.0 || config->image_mode != MEREADER_TUI_IMAGE_MODE_AUTO ||
        config->image_mode_explicit || config->show_image_as_ansi || config->dark.background != 0U ||
        config->dark.foreground != 0U || config->dark.accent != 0U || config->light.background != 0U ||
        config->light.foreground != 0U || config->light.accent != 0U) {
        return false;
    }

    const MereaderTuiKeyList *lists[] = {
        &config->keymaps.toggle_dark,      &config->keymaps.scroll_down,
        &config->keymaps.scroll_up,        &config->keymaps.page_down,
        &config->keymaps.page_up,          &config->keymaps.home,
        &config->keymaps.end,              &config->keymaps.open_toc,
        &config->keymaps.add_bookmark,     &config->keymaps.open_bookmarks,
        &config->keymaps.open_metadata,    &config->keymaps.open_help,
        &config->keymaps.search_forward,   &config->keymaps.search_backward,
        &config->keymaps.next_match,       &config->keymaps.previous_match,
        &config->keymaps.confirm,          &config->keymaps.close,
        &config->keymaps.screenshot,       &config->keymaps.toggle_pdf_view,
        &config->library_keymaps.move_down, &config->library_keymaps.move_up,
        &config->library_keymaps.page_down, &config->library_keymaps.page_up,
        &config->library_keymaps.first,     &config->library_keymaps.last,
        &config->library_keymaps.open,      &config->library_keymaps.back,
        &config->library_keymaps.toggle_view,
        &config->library_keymaps.toggle_shelf,
        &config->library_keymaps.choose_format,
        &config->library_keymaps.sort,      &config->library_keymaps.refresh,
        &config->library_keymaps.open_path, &config->library_keymaps.filter,
        &config->library_keymaps.find,      &config->library_keymaps.help,
        &config->library_keymaps.confirm,   &config->library_keymaps.close,
        &config->library_keymaps.quit,
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(lists); ++index) {
        if (lists[index]->items != nullptr || lists[index]->length != 0U) {
            return false;
        }
    }
    return true;
}

bool mereader_tui_config_load_path(MereaderTuiConfig *config, const char *path, MereaderTuiError *error) {
    if (config == nullptr || path == nullptr || path[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid config path");
        return false;
    }
    if (!mereader_tui_config_output_empty(config)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "config output is not empty");
        return false;
    }

    MereaderTuiBuffer contents = {0};
    if (!mereader_tui_read_file(path, &contents, error)) {
        return false;
    }

    MereaderTuiIni ini = {0};
    bool parsed = mereader_tui_ini_parse(contents.data, contents.length, &ini, error);
    mereader_tui_buffer_free(&contents);
    if (!parsed) {
        mereader_tui_ini_free(&ini);
        return false;
    }

    bool built = mereader_tui_config_build(&ini, config, error);
    mereader_tui_ini_free(&ini);
    return built;
}

static bool mereader_tui_config_create_file(const char *path, MereaderTuiError *error) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        if (errno == EEXIST) {
            return true;
        }
        int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not create config '%s': %s", path, strerror(saved_errno));
        return false;
    }

    const unsigned char *cursor = (const unsigned char *)MEREADER_TUI_DEFAULT_CONFIG;
    size_t remaining = strlen(MEREADER_TUI_DEFAULT_CONFIG);
    while (remaining != 0U) {
        size_t chunk = remaining > 1048576U ? 1048576U : remaining;
        ssize_t written = write(descriptor, cursor, chunk);
        if (written > 0) {
            cursor += (size_t)written;
            remaining -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }

        int saved_errno = written == 0 ? EIO : errno;
        (void)close(descriptor);
        (void)unlink(path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not write config '%s': %s", path, strerror(saved_errno));
        return false;
    }
    if (close(descriptor) != 0) {
        int saved_errno = errno;
        (void)unlink(path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close config '%s': %s", path, strerror(saved_errno));
        return false;
    }
    return true;
}

bool mereader_tui_config_load(MereaderTuiConfig *config, MereaderTuiError *error) {
    if (config == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "config output is null");
        return false;
    }
    if (!mereader_tui_config_output_empty(config)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "config output is not empty");
        return false;
    }

    char *path = mereader_tui_xdg_config_path("config.ini", error);
    if (path == nullptr) {
        return false;
    }

    struct stat status;
    if (stat(path, &status) != 0) {
        if (errno != ENOENT && errno != ENOTDIR) {
            int saved_errno = errno;
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect config '%s': %s", path,
                           strerror(saved_errno));
            free(path);
            return false;
        }

        char *directory = mereader_tui_path_dirname(path, error);
        if (directory == nullptr || !mereader_tui_mkdirs(directory, error) || !mereader_tui_config_create_file(path, error)) {
            free(directory);
            free(path);
            return false;
        }
        free(directory);
    } else if (!S_ISREG(status.st_mode)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "config path '%s' is not a regular file", path);
        free(path);
        return false;
    }

    bool loaded = mereader_tui_config_load_path(config, path, error);
    free(path);
    return loaded;
}

static bool mereader_tui_config_token_equals(const char *start, const char *end, const char *expected) {
    while (start < end && isspace((unsigned char)*start) != 0) {
        ++start;
    }
    while (end > start && isspace((unsigned char)end[-1]) != 0) {
        --end;
    }
    const size_t length = (size_t)(end - start);
    if (length != strlen(expected)) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (tolower((unsigned char)start[index]) != tolower((unsigned char)expected[index])) {
            return false;
        }
    }
    return true;
}

static bool mereader_tui_config_write_atomic(const char *path, const char *data, size_t length, MereaderTuiError *error) {
    MereaderTuiString temporary = {0};
    if (!mereader_tui_string_append(&temporary, path, error) ||
        !mereader_tui_string_append(&temporary, ".tmp.XXXXXX", error)) {
        mereader_tui_string_free(&temporary);
        return false;
    }
    int descriptor = mkstemp(temporary.data);
    if (descriptor < 0) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not create temporary config: %s", strerror(saved_errno));
        mereader_tui_string_free(&temporary);
        return false;
    }
    bool written = fchmod(descriptor, 0600) == 0;
    const unsigned char *cursor = (const unsigned char *)data;
    size_t remaining = length;
    while (written && remaining > 0U) {
        const ssize_t count = write(descriptor, cursor, remaining);
        if (count > 0) {
            cursor += (size_t)count;
            remaining -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            written = false;
        }
    }
    if (written) {
        written = fsync(descriptor) == 0;
    }
    const int close_status = close(descriptor);
    written = written && close_status == 0;
    if (written) {
        written = rename(temporary.data, path) == 0;
    }
    if (!written) {
        const int saved_errno = errno == 0 ? EIO : errno;
        (void)unlink(temporary.data);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not save config '%s': %s", path, strerror(saved_errno));
    }
    mereader_tui_string_free(&temporary);
    return written;
}

static bool mereader_tui_config_append_value(MereaderTuiString *output, const char *value, MereaderTuiError *error) {
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (!mereader_tui_string_append_char(output, *cursor, error) ||
            (*cursor == '%' && !mereader_tui_string_append_char(output, '%', error))) {
            return false;
        }
    }
    return true;
}

bool mereader_tui_config_save_library_path(const char *library_path, MereaderTuiError *error) {
    if (library_path == nullptr || library_path[0] == '\0' || strchr(library_path, '\n') != nullptr ||
        strchr(library_path, '\r') != nullptr || isspace((unsigned char)library_path[0]) != 0 ||
        isspace((unsigned char)library_path[strlen(library_path) - 1U]) != 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid library path");
        return false;
    }
    char *path = mereader_tui_xdg_config_path("config.ini", error);
    if (path == nullptr) {
        return false;
    }
    MereaderTuiBuffer contents = {0};
    if (!mereader_tui_read_file(path, &contents, error)) {
        free(path);
        return false;
    }

    MereaderTuiString output = {0};
    const char *cursor = (const char *)contents.data;
    const char *end = cursor + contents.length;
    bool in_general = false;
    bool replaced = false;
    bool built = true;
    while (built && cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        const char *next = line_end == nullptr ? end : line_end + 1;
        const char *content_end = line_end == nullptr ? end : line_end;
        if (content_end > cursor && content_end[-1] == '\r') {
            --content_end;
        }
        const char *trimmed = cursor;
        while (trimmed < content_end && isspace((unsigned char)*trimmed) != 0) {
            ++trimmed;
        }
        bool section_line = false;
        bool general_line = false;
        if (trimmed < content_end && *trimmed == '[') {
            const char *closing = memchr(trimmed + 1, ']', (size_t)(content_end - trimmed - 1));
            section_line = closing != nullptr;
            general_line = section_line && mereader_tui_config_token_equals(trimmed + 1, closing, "General");
            if (section_line) {
                in_general = general_line;
            }
        }

        bool library_line = false;
        if (in_general && !section_line && trimmed < content_end && *trimmed != '#' && *trimmed != ';') {
            const char *delimiter = memchr(trimmed, '=', (size_t)(content_end - trimmed));
            const char *colon = memchr(trimmed, ':', (size_t)(content_end - trimmed));
            if (delimiter == nullptr || (colon != nullptr && colon < delimiter)) {
                delimiter = colon;
            }
            library_line = delimiter != nullptr && mereader_tui_config_token_equals(trimmed, delimiter, "LibraryPath");
        }
        if (built && library_line && !replaced) {
            built = mereader_tui_string_append(&output, "LibraryPath = ", error) &&
                    mereader_tui_config_append_value(&output, library_path, error) &&
                    mereader_tui_string_append_char(&output, '\n', error);
            replaced = built;
        } else if (built) {
            built = mereader_tui_string_append_n(&output, cursor, (size_t)(next - cursor), error);
        }
        cursor = next;
    }
    if (built && !replaced) {
        if (output.length > 0U && output.data[output.length - 1U] != '\n') {
            built = mereader_tui_string_append_char(&output, '\n', error);
        }
        built = built && mereader_tui_string_append(&output, "[General]\n", error);
        if (built) {
            built = mereader_tui_string_append(&output, "LibraryPath = ", error) &&
                    mereader_tui_config_append_value(&output, library_path, error) &&
                    mereader_tui_string_append_char(&output, '\n', error);
        }
    }
    if (built) {
        built = mereader_tui_config_write_atomic(path, output.data, output.length, error);
    }
    mereader_tui_string_free(&output);
    mereader_tui_buffer_free(&contents);
    free(path);
    return built;
}

void mereader_tui_config_free(MereaderTuiConfig *config) {
    if (config == nullptr) {
        return;
    }
    free(config->preferred_image_viewer);
    free(config->library_path);
    MereaderTuiKeyList *lists[] = {
        &config->keymaps.toggle_dark,      &config->keymaps.scroll_down,
        &config->keymaps.scroll_up,        &config->keymaps.page_down,
        &config->keymaps.page_up,          &config->keymaps.home,
        &config->keymaps.end,              &config->keymaps.open_toc,
        &config->keymaps.add_bookmark,     &config->keymaps.open_bookmarks,
        &config->keymaps.open_metadata,    &config->keymaps.open_help,
        &config->keymaps.search_forward,   &config->keymaps.search_backward,
        &config->keymaps.next_match,       &config->keymaps.previous_match,
        &config->keymaps.confirm,          &config->keymaps.close,
        &config->keymaps.screenshot,       &config->keymaps.toggle_pdf_view,
        &config->library_keymaps.move_down, &config->library_keymaps.move_up,
        &config->library_keymaps.page_down, &config->library_keymaps.page_up,
        &config->library_keymaps.first,     &config->library_keymaps.last,
        &config->library_keymaps.open,      &config->library_keymaps.back,
        &config->library_keymaps.toggle_view,
        &config->library_keymaps.toggle_shelf,
        &config->library_keymaps.choose_format,
        &config->library_keymaps.sort,      &config->library_keymaps.refresh,
        &config->library_keymaps.open_path, &config->library_keymaps.filter,
        &config->library_keymaps.find,      &config->library_keymaps.help,
        &config->library_keymaps.confirm,   &config->library_keymaps.close,
        &config->library_keymaps.quit,
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(lists); ++index) {
        mereader_tui_key_list_free(lists[index]);
    }
    *config = (MereaderTuiConfig){0};
}

int mereader_tui_config_content_width(const MereaderTuiConfig *config, int terminal_width) {
    if (config == nullptr || terminal_width <= 0 || config->max_text_width <= 0) {
        return 0;
    }

    int64_t requested = config->max_text_width;
    if (config->max_text_width_percent) {
        requested = (int64_t)terminal_width * (int64_t)config->max_text_width / 100;
    }
    if (requested < 1) {
        requested = 1;
    }
    if (requested > terminal_width) {
        requested = terminal_width;
    }
    return (int)requested;
}

const char *mereader_tui_config_default_text(void) {
    return MEREADER_TUI_DEFAULT_CONFIG;
}
