#include "baca/config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct BacaIniEntry {
    char *section;
    char *key;
    char *value;
} BacaIniEntry;

typedef struct BacaIni {
    BacaIniEntry *items;
    size_t length;
    size_t capacity;
} BacaIni;

static const char BACA_DEFAULT_CONFIG[] =
    "[General]\n"
    "# pick your favorite image viewer\n"
    "PreferredImageViewer = auto\n"
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
    "# PDF fixed-page rendering follows this mode; press v for reflowable text\n"
    "ImageMode = auto\n"
    "\n"
    "# colors accept #rgb, #rrggbb, or common names\n"
    "[Color Dark]\n"
    "Background = #1e1e1e\n"
    "Foreground = #f5f5f5\n"
    "Accent = #0178d4\n"
    "\n"
    "[Color Light]\n"
    "Background = #f5f5f5\n"
    "Foreground = #1e1e1e\n"
    "Accent = #0178d4\n"
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
    "OpenHelp = f1\n"
    "SearchForward = slash\n"
    "SearchBackward = question_mark\n"
    "NextMatch = n\n"
    "PreviousMatch = N\n"
    "Confirm = enter\n"
    "CloseOrQuit = q,escape\n"
    "Screenshot = f12\n";

static void baca_ini_free(BacaIni *ini) {
    if (ini == nullptr) {
        return;
    }
    for (size_t index = 0U; index < ini->length; ++index) {
        free(ini->items[index].section);
        free(ini->items[index].key);
        free(ini->items[index].value);
    }
    free(ini->items);
    *ini = (BacaIni){0};
}

static char *baca_ini_copy_trimmed(const char *start, const char *end, BacaError *error) {
    while (start < end && isspace((unsigned char)*start)) {
        ++start;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        --end;
    }
    return baca_strndup(start, (size_t)(end - start), error);
}

static char *baca_ini_unescape_percent(const char *value, BacaError *error) {
    BacaString result = {0};
    for (size_t index = 0U; value[index] != '\0'; ++index) {
        if (value[index] == '%' && value[index + 1U] == '%') {
            if (!baca_string_append_char(&result, '%', error)) {
                baca_string_free(&result);
                return nullptr;
            }
            ++index;
        } else if (!baca_string_append_char(&result, value[index], error)) {
            baca_string_free(&result);
            return nullptr;
        }
    }
    if (!baca_string_append_n(&result, "", 0U, error)) {
        baca_string_free(&result);
        return nullptr;
    }
    return baca_string_take(&result);
}

static bool baca_ini_add(BacaIni *ini, const char *section, const char *key, const char *value, size_t line_number,
                         BacaError *error) {
    for (size_t index = 0U; index < ini->length; ++index) {
        if (baca_casecmp(ini->items[index].section, section) == 0 &&
            baca_casecmp(ini->items[index].key, key) == 0) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "duplicate INI option '%s.%s' on line %zu", section, key,
                           line_number);
            return false;
        }
    }

    BacaError reserve_error = {0};
    BacaIniEntry *items = baca_array_reserve(ini->items, &ini->capacity, sizeof(*ini->items), ini->length + 1U,
                                             &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (error != nullptr) {
            *error = reserve_error;
        }
        return false;
    }
    ini->items = items;

    BacaIniEntry entry = {0};
    entry.section = baca_strdup(section, error);
    entry.key = baca_strdup(key, error);
    entry.value = baca_ini_unescape_percent(value, error);
    if (entry.section == nullptr || entry.key == nullptr || entry.value == nullptr) {
        free(entry.section);
        free(entry.key);
        free(entry.value);
        return false;
    }
    ini->items[ini->length++] = entry;
    return true;
}

static bool baca_ini_parse(const unsigned char *data, size_t length, BacaIni *ini, BacaError *error) {
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
            baca_error_set(error, BACA_ERROR_CORRUPT, "INI contains a null byte on line %zu", line_number);
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
                baca_error_set(error, BACA_ERROR_CORRUPT, "unterminated INI section on line %zu", line_number);
                return false;
            }
            const char *trailing = closing + 1;
            while (trailing < end && isspace((unsigned char)*trailing)) {
                ++trailing;
            }
            if (trailing < end && *trailing != '#' && *trailing != ';') {
                free(section);
                baca_error_set(error, BACA_ERROR_CORRUPT, "unexpected text after INI section on line %zu",
                               line_number);
                return false;
            }

            char *new_section = baca_ini_copy_trimmed(start + 1, closing, error);
            if (new_section == nullptr) {
                free(section);
                return false;
            }
            if (new_section[0] == '\0') {
                free(new_section);
                free(section);
                baca_error_set(error, BACA_ERROR_CORRUPT, "empty INI section on line %zu", line_number);
                return false;
            }
            free(section);
            section = new_section;
            continue;
        }

        if (section == nullptr) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "INI option appears before a section on line %zu",
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
            baca_error_set(error, BACA_ERROR_CORRUPT, "INI option has no delimiter on line %zu", line_number);
            return false;
        }

        char *key = baca_ini_copy_trimmed(start, delimiter, error);
        char *value = baca_ini_copy_trimmed(delimiter + 1, end, error);
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
            baca_error_set(error, BACA_ERROR_CORRUPT, "empty INI option on line %zu", line_number);
            return false;
        }
        bool added = baca_ini_add(ini, section, key, value, line_number, error);
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

static const char *baca_ini_get(const BacaIni *ini, const char *section, const char *key,
                                const char *fallback) {
    for (size_t index = 0U; index < ini->length; ++index) {
        if (baca_casecmp(ini->items[index].section, section) == 0 &&
            baca_casecmp(ini->items[index].key, key) == 0) {
            return ini->items[index].value;
        }
    }
    for (size_t index = 0U; index < ini->length; ++index) {
        if (baca_casecmp(ini->items[index].section, "DEFAULT") == 0 &&
            baca_casecmp(ini->items[index].key, key) == 0) {
            return ini->items[index].value;
        }
    }
    return fallback;
}

static bool baca_config_parse_positive_width(const char *value, int *width, bool *percent) {
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

static bool baca_config_parse_justification(const char *value, BacaJustification *justification,
                                            BacaError *error) {
    if (baca_casecmp(value, "justify") == 0 || baca_casecmp(value, "full") == 0) {
        *justification = BACA_JUSTIFY_FULL;
    } else if (baca_casecmp(value, "center") == 0) {
        *justification = BACA_JUSTIFY_CENTER;
    } else if (baca_casecmp(value, "right") == 0) {
        *justification = BACA_JUSTIFY_RIGHT;
    } else if (baca_casecmp(value, "left") == 0 || baca_casecmp(value, "default") == 0) {
        *justification = BACA_JUSTIFY_LEFT;
    } else {
        baca_error_set(error, BACA_ERROR_CORRUPT, "invalid General.TextJustification value '%s'", value);
        return false;
    }
    return true;
}

static bool baca_config_parse_image_mode(const char *value, BacaImageMode *mode, BacaError *error) {
    if (baca_casecmp(value, "auto") == 0) {
        *mode = BACA_IMAGE_MODE_AUTO;
    } else if (baca_casecmp(value, "kitty") == 0) {
        *mode = BACA_IMAGE_MODE_KITTY;
    } else if (baca_casecmp(value, "ansi") == 0) {
        *mode = BACA_IMAGE_MODE_ANSI;
    } else if (baca_casecmp(value, "placeholder") == 0) {
        *mode = BACA_IMAGE_MODE_PLACEHOLDER;
    } else {
        baca_error_set(error, BACA_ERROR_CORRUPT, "invalid General.ImageMode value '%s'", value);
        return false;
    }
    return true;
}

static bool baca_config_parse_bool(const char *section, const char *key, const char *value, bool *result,
                                   BacaError *error) {
    if (baca_casecmp(value, "1") == 0 || baca_casecmp(value, "yes") == 0 ||
        baca_casecmp(value, "true") == 0 || baca_casecmp(value, "on") == 0) {
        *result = true;
        return true;
    }
    if (baca_casecmp(value, "0") == 0 || baca_casecmp(value, "no") == 0 ||
        baca_casecmp(value, "false") == 0 || baca_casecmp(value, "off") == 0) {
        *result = false;
        return true;
    }
    baca_error_set(error, BACA_ERROR_CORRUPT, "invalid %s.%s boolean value '%s'", section, key, value);
    return false;
}

static bool baca_config_parse_duration(const char *value, double *duration, BacaError *error) {
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
        baca_error_set(error, BACA_ERROR_CORRUPT, "invalid General.PageScrollDuration value '%s'", value);
        return false;
    }
    *duration = parsed;
    return true;
}

static int baca_config_hex_digit(unsigned char character) {
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

static bool baca_config_parse_color(const char *value, uint32_t *color) {
    const char *digits = value[0] == '#' ? value + 1 : value;
    size_t digit_count = strlen(digits);
    if (digit_count == 6U || (value[0] == '#' && digit_count == 3U)) {
        uint32_t parsed = 0U;
        bool valid = true;
        for (size_t index = 0U; index < digit_count; ++index) {
            int digit = baca_config_hex_digit((unsigned char)digits[index]);
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
    for (size_t index = 0U; index < BACA_ARRAY_LEN(named_colors); ++index) {
        if (baca_casecmp(value, named_colors[index].name) == 0) {
            *color = named_colors[index].value;
            return true;
        }
    }
    return false;
}

static void baca_key_list_free(BacaKeyList *list) {
    if (list == nullptr) {
        return;
    }
    for (size_t index = 0U; index < list->length; ++index) {
        free(list->items[index]);
    }
    free(list->items);
    *list = (BacaKeyList){0};
}

static bool baca_config_parse_key_list(const char *value, BacaKeyList *list, BacaError *error) {
    size_t count = 1U;
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor == ',') {
            if (count == SIZE_MAX) {
                baca_error_set(error, BACA_ERROR_MEMORY, "keymap item count overflow");
                return false;
            }
            ++count;
        }
    }

    char **items = baca_reallocarray(nullptr, count, sizeof(*items), error);
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
        items[index] = baca_ini_copy_trimmed(start, cursor, error);
        if (items[index] == nullptr) {
            BacaKeyList partial = {.items = items, .length = index};
            baca_key_list_free(&partial);
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

static bool baca_config_build(const BacaIni *ini, BacaConfig *config, BacaError *error) {
    BacaConfig result = {
        .max_text_width = 80,
        .justification = BACA_JUSTIFY_LEFT,
        .dark = {.background = 0x1e1e1eU, .foreground = 0xf5f5f5U, .accent = 0x0178d4U},
        .light = {.background = 0xf5f5f5U, .foreground = 0x1e1e1eU, .accent = 0x0178d4U},
    };
    (void)baca_config_parse_positive_width(baca_ini_get(ini, "General", "MaxTextWidth", "80"),
                                           &result.max_text_width, &result.max_text_width_percent);
    (void)baca_config_parse_color(baca_ini_get(ini, "Color Dark", "Background", "#1e1e1e"),
                                  &result.dark.background);
    (void)baca_config_parse_color(baca_ini_get(ini, "Color Dark", "Foreground", "#f5f5f5"),
                                  &result.dark.foreground);
    (void)baca_config_parse_color(baca_ini_get(ini, "Color Dark", "Accent", "#0178d4"),
                                  &result.dark.accent);
    (void)baca_config_parse_color(baca_ini_get(ini, "Color Light", "Background", "#f5f5f5"),
                                  &result.light.background);
    (void)baca_config_parse_color(baca_ini_get(ini, "Color Light", "Foreground", "#1e1e1e"),
                                  &result.light.foreground);
    (void)baca_config_parse_color(baca_ini_get(ini, "Color Light", "Accent", "#0178d4"),
                                  &result.light.accent);

    const char *image_mode = baca_ini_get(ini, "General", "ImageMode", nullptr);
    if (image_mode != nullptr) {
        result.image_mode_explicit = true;
        if (!baca_config_parse_image_mode(image_mode, &result.image_mode, error)) {
            baca_config_free(&result);
            return false;
        }
        result.show_image_as_ansi = result.image_mode != BACA_IMAGE_MODE_PLACEHOLDER;
    } else if (!baca_config_parse_bool("General", "ShowImageAsANSI",
                                       baca_ini_get(ini, "General", "ShowImageAsANSI", "yes"),
                                       &result.show_image_as_ansi, error)) {
        baca_config_free(&result);
        return false;
    } else {
        result.image_mode = result.show_image_as_ansi ? BACA_IMAGE_MODE_AUTO : BACA_IMAGE_MODE_PLACEHOLDER;
    }

    result.preferred_image_viewer =
        baca_strdup(baca_ini_get(ini, "General", "PreferredImageViewer", "auto"), error);
    result.library_path = baca_strdup(baca_ini_get(ini, "General", "LibraryPath", "auto"), error);
    if (result.preferred_image_viewer == nullptr || result.library_path == nullptr ||
        !baca_config_parse_justification(baca_ini_get(ini, "General", "TextJustification", "justify"),
                                         &result.justification, error) ||
        !baca_config_parse_bool("General", "Pretty", baca_ini_get(ini, "General", "Pretty", "no"),
                                &result.pretty, error) ||
        !baca_config_parse_duration(baca_ini_get(ini, "General", "PageScrollDuration", "0.2"),
                                    &result.page_scroll_duration, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "ToggleLightDark", "c"),
                                    &result.keymaps.toggle_dark, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "TogglePdfView", "v"),
                                    &result.keymaps.toggle_pdf_view, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "ScrollDown", "down,j"),
                                    &result.keymaps.scroll_down, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "ScrollUp", "up,k"),
                                    &result.keymaps.scroll_up, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "PageDown", "ctrl+f,pagedown,l,space"),
                                    &result.keymaps.page_down, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "PageUp", "ctrl+b,pageup,h"),
                                    &result.keymaps.page_up, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "Home", "home,gg"), &result.keymaps.home,
                                    error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "End", "end,G"), &result.keymaps.end,
                                    error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "OpenToc", "tab"),
                                    &result.keymaps.open_toc, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "AddBookmark", "b"),
                                    &result.keymaps.add_bookmark, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "OpenBookmarks", "B"),
                                    &result.keymaps.open_bookmarks, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "OpenMetadata", "M"),
                                    &result.keymaps.open_metadata, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "OpenHelp", "f1"),
                                    &result.keymaps.open_help, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "SearchForward", "slash"),
                                    &result.keymaps.search_forward, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "SearchBackward", "question_mark"),
                                    &result.keymaps.search_backward, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "NextMatch", "n"),
                                    &result.keymaps.next_match, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "PreviousMatch", "N"),
                                    &result.keymaps.previous_match, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "Confirm", "enter"),
                                    &result.keymaps.confirm, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "CloseOrQuit", "q,escape"),
                                    &result.keymaps.close, error) ||
        !baca_config_parse_key_list(baca_ini_get(ini, "Keymaps", "Screenshot", "f12"),
                                    &result.keymaps.screenshot, error)) {
        baca_config_free(&result);
        return false;
    }

    *config = result;
    return true;
}

static bool baca_config_output_empty(const BacaConfig *config) {
    if (config->preferred_image_viewer != nullptr || config->library_path != nullptr || config->max_text_width != 0 ||
        config->max_text_width_percent || config->justification != BACA_JUSTIFY_LEFT || config->pretty ||
        config->page_scroll_duration != 0.0 || config->image_mode != BACA_IMAGE_MODE_AUTO ||
        config->image_mode_explicit || config->show_image_as_ansi || config->dark.background != 0U ||
        config->dark.foreground != 0U || config->dark.accent != 0U || config->light.background != 0U ||
        config->light.foreground != 0U || config->light.accent != 0U) {
        return false;
    }

    const BacaKeyList *lists[] = {
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
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(lists); ++index) {
        if (lists[index]->items != nullptr || lists[index]->length != 0U) {
            return false;
        }
    }
    return true;
}

bool baca_config_load_path(BacaConfig *config, const char *path, BacaError *error) {
    if (config == nullptr || path == nullptr || path[0] == '\0') {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid config path");
        return false;
    }
    if (!baca_config_output_empty(config)) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "config output is not empty");
        return false;
    }

    BacaBuffer contents = {0};
    if (!baca_read_file(path, &contents, error)) {
        return false;
    }

    BacaIni ini = {0};
    bool parsed = baca_ini_parse(contents.data, contents.length, &ini, error);
    baca_buffer_free(&contents);
    if (!parsed) {
        baca_ini_free(&ini);
        return false;
    }

    bool built = baca_config_build(&ini, config, error);
    baca_ini_free(&ini);
    return built;
}

static bool baca_config_create_file(const char *path, BacaError *error) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        if (errno == EEXIST) {
            return true;
        }
        int saved_errno = errno;
        baca_error_set(error, BACA_ERROR_IO, "could not create config '%s': %s", path, strerror(saved_errno));
        return false;
    }

    const unsigned char *cursor = (const unsigned char *)BACA_DEFAULT_CONFIG;
    size_t remaining = strlen(BACA_DEFAULT_CONFIG);
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
        baca_error_set(error, BACA_ERROR_IO, "could not write config '%s': %s", path, strerror(saved_errno));
        return false;
    }
    if (close(descriptor) != 0) {
        int saved_errno = errno;
        (void)unlink(path);
        baca_error_set(error, BACA_ERROR_IO, "could not close config '%s': %s", path, strerror(saved_errno));
        return false;
    }
    return true;
}

bool baca_config_load(BacaConfig *config, BacaError *error) {
    if (config == nullptr) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "config output is null");
        return false;
    }
    if (!baca_config_output_empty(config)) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "config output is not empty");
        return false;
    }

    char *path = baca_xdg_config_path("config.ini", error);
    if (path == nullptr) {
        return false;
    }

    struct stat status;
    if (stat(path, &status) != 0) {
        if (errno != ENOENT && errno != ENOTDIR) {
            int saved_errno = errno;
            baca_error_set(error, BACA_ERROR_IO, "could not inspect config '%s': %s", path,
                           strerror(saved_errno));
            free(path);
            return false;
        }

        char *directory = baca_path_dirname(path, error);
        if (directory == nullptr || !baca_mkdirs(directory, error) || !baca_config_create_file(path, error)) {
            free(directory);
            free(path);
            return false;
        }
        free(directory);
    } else if (!S_ISREG(status.st_mode)) {
        baca_error_set(error, BACA_ERROR_IO, "config path '%s' is not a regular file", path);
        free(path);
        return false;
    }

    bool loaded = baca_config_load_path(config, path, error);
    free(path);
    return loaded;
}

void baca_config_free(BacaConfig *config) {
    if (config == nullptr) {
        return;
    }
    free(config->preferred_image_viewer);
    free(config->library_path);
    BacaKeyList *lists[] = {
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
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(lists); ++index) {
        baca_key_list_free(lists[index]);
    }
    *config = (BacaConfig){0};
}

int baca_config_content_width(const BacaConfig *config, int terminal_width) {
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

const char *baca_config_default_text(void) {
    return BACA_DEFAULT_CONFIG;
}
