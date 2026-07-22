#include "baca/app.h"
#include "baca/document_backend.h"
#include "baca/graphics.h"
#include "baca/platform.h"
#include "terminal_runtime.h"
#include "text_input.h"

#include <ctype.h>
#include <curses.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

enum {
  BACA_PAIR_BASE = 1,
  BACA_PAIR_ACCENT,
  BACA_PAIR_SEARCH,
  BACA_PAIR_ALERT,
  BACA_PAIR_IMAGE_FIRST,
  BACA_JUMP_HISTORY_LIMIT = 100,
  BACA_HELP_LINE_COUNT = 26,
  BACA_PDF_HELP_LINE_COUNT = 28,
  BACA_KEY_CTRL_I = KEY_MAX + 1,
};

static const char BACA_IMAGE_PLACEHOLDER[] = "IMAGE";

typedef enum BacaOverlayKind {
  BACA_OVERLAY_NONE = 0,
  BACA_OVERLAY_TOC,
  BACA_OVERLAY_BOOKMARKS,
  BACA_OVERLAY_METADATA,
  BACA_OVERLAY_HELP,
  BACA_OVERLAY_ALERT,
} BacaOverlayKind;

typedef struct BacaNormalizedKey {
  bool valid;
  bool key_code;
  int code;
  wchar_t character;
  BacaCommand command;
} BacaNormalizedKey;

typedef struct BacaSearchState {
  char *pattern;
  BacaSearchMatch *matches;
  size_t match_count;
  size_t current_match;
  double saved_progress;
  bool active;
  bool forward;
} BacaSearchState;

typedef struct BacaPromptState {
  BacaTextInput text;
  double saved_progress;
  bool active;
  bool forward;
} BacaPromptState;

typedef struct BacaReaderCommandState {
  size_t count;
  bool count_active;
  bool g_pending;
} BacaReaderCommandState;

typedef struct BacaJumpHistory {
  double backward[BACA_JUMP_HISTORY_LIMIT];
  size_t backward_count;
  double forward[BACA_JUMP_HISTORY_LIMIT];
  size_t forward_count;
} BacaJumpHistory;

typedef struct BacaAnimation {
  double from;
  double to;
  double started_at;
  double duration;
  bool active;
} BacaAnimation;

typedef struct BacaOverlayBox {
  int y;
  int x;
  int height;
  int width;
} BacaOverlayBox;

typedef struct BacaParserJob {
  const BacaDocument *document;
  int width;
  BacaJustification justification;
  BacaPresentation presentation;
  int cell_pixel_width;
  int cell_pixel_height;
  BacaLayout layout;
  BacaError error;
  atomic_bool cancel;
  atomic_bool done;
  atomic_bool succeeded;
} BacaParserJob;

typedef struct BacaPdfRenderFailure {
  int columns;
  int rows;
  int cell_pixel_width;
  int cell_pixel_height;
  bool valid;
} BacaPdfRenderFailure;

typedef struct BacaTuiState {
  BacaApp *app;
  BacaGraphicsContext *graphics;
  int rows;
  int columns;
  int content_width;
  int content_x;
  int previous_cursor;
  BacaOverlayKind overlay;
  size_t overlay_scroll;
  size_t toc_index;
  BacaBookmarks bookmarks;
  size_t bookmark_index;
  char alert[512];
  BacaPdfRenderFailure *pdf_render_failures;
  size_t pdf_render_failure_count;
  BacaSearchState search;
  BacaPromptState prompt;
  BacaReaderCommandState reader_command;
  BacaJumpHistory jumps;
  BacaAnimation animation;
  char **temp_directories;
  size_t temp_directory_count;
  size_t temp_directory_capacity;
  BacaImageMode image_mode;
  BacaPresentation presentation;
  int cell_pixel_width;
  int cell_pixel_height;
  short image_pair_capacity;
  bool colors;
  bool raw_truecolor;
  bool quit;
  bool dirty;
} BacaTuiState;

static void open_alert(BacaTuiState *state, const char *message);
static void draw_frame(BacaTuiState *state);
static bool follow_link(BacaTuiState *state, const char *link,
                        const char *missing_target_message);

static size_t minimum_size(size_t left, size_t right) {
  return left < right ? left : right;
}

static int size_to_int(size_t value) {
  return value > (size_t)INT_MAX ? INT_MAX : (int)value;
}

static uint32_t current_background(const BacaTuiState *state) {
  return state->app->dark_mode ? state->app->config.dark.background
                               : state->app->config.light.background;
}

static void delete_kitty_images(BacaTuiState *state) {
  if (state->graphics == NULL || state->image_mode != BACA_IMAGE_MODE_KITTY) {
    return;
  }
  BacaError ignored = {0};
  (void)fflush(stdout);
  (void)baca_graphics_kitty_delete_all(
      state->graphics, baca_terminal_graphics_write, state, &ignored);
}

static size_t utf8_bytes_for_columns(const char *text, int columns) {
  if (text == NULL || columns <= 0) {
    return 0U;
  }
  const size_t length = strlen(text);
  size_t offset = 0U;
  int used = 0;
  while (offset < length && text[offset] != '\n' && text[offset] != '\r') {
    int width = 0;
    const size_t next = baca_utf8_next(text, length, offset, &width);
    if (next <= offset || used + width > columns) {
      break;
    }
    used += width;
    offset = next;
  }
  return offset;
}

static void add_clipped(WINDOW *window, int y, int x, const char *text,
                        int columns) {
  if (window == NULL || text == NULL || columns <= 0) {
    return;
  }
  int height = 0;
  int width = 0;
  getmaxyx(window, height, width);
  if (y < 0 || y >= height || x < 0 || x >= width) {
    return;
  }
  if (columns > width - x) {
    columns = width - x;
  }
  const size_t bytes = utf8_bytes_for_columns(text, columns);
  (void)mvwaddnstr(window, y, x, text, size_to_int(bytes));
}

static int content_width_for(const BacaConfig *config, int terminal_width) {
  if (terminal_width <= 0) {
    return 1;
  }
  int available = terminal_width;
  if (available > 4) {
    available -= 4;
  }
  int width = baca_config_content_width(config, available);
  if (width < 1) {
    width = 1;
  }
  if (width > BACA_GRAPHICS_MAX_COLUMNS) {
    width = BACA_GRAPHICS_MAX_COLUMNS;
  }
  if (width > available) {
    width = available;
  }
  return width;
}

static void update_cell_pixels(BacaTuiState *state) {
  int width = 0;
  int height = 0;
  baca_terminal_probe_cell_pixels(state->image_mode == BACA_IMAGE_MODE_KITTY,
                                  &width, &height);
  state->cell_pixel_width = width;
  state->cell_pixel_height = height;
  if (state->graphics != NULL) {
    BacaError ignored = {0};
    (void)baca_graphics_set_cell_pixels(state->graphics, width, height,
                                        &ignored);
  }
}

static void update_dimensions(BacaTuiState *state) {
  const int previous_rows = state->rows;
  const int previous_columns = state->columns;
  getmaxyx(stdscr, state->rows, state->columns);
  if (state->rows < 1) {
    state->rows = 1;
  }
  if (state->columns < 1) {
    state->columns = 1;
  }
  state->content_width = content_width_for(&state->app->config, state->columns);
  state->content_x = (state->columns - state->content_width) / 2;
  if (state->content_x < 0) {
    state->content_x = 0;
  }
  if (state->graphics != NULL &&
      (previous_rows != state->rows || previous_columns != state->columns)) {
    delete_kitty_images(state);
    BacaError ignored = {0};
    (void)baca_graphics_resize(state->graphics, state->columns, state->rows,
                               &ignored);
  }
  update_cell_pixels(state);
}

static short terminal_color(uint32_t rgb) {
  return (short)baca_graphics_rgb_to_palette(rgb, (unsigned)COLORS);
}

static void apply_theme(BacaTuiState *state) {
  if (state->graphics != NULL) {
    delete_kitty_images(state);
    BacaError ignored = {0};
    (void)baca_graphics_set_background(state->graphics,
                                       current_background(state), &ignored);
  }
  if (!state->colors) {
    return;
  }
  const BacaColorScheme *theme = state->app->dark_mode
                                     ? &state->app->config.dark
                                     : &state->app->config.light;
  const short background = terminal_color(theme->background);
  const short foreground = terminal_color(theme->foreground);
  const short accent = terminal_color(theme->accent);
  if (init_pair(BACA_PAIR_BASE, foreground, background) == ERR ||
      init_pair(BACA_PAIR_ACCENT, accent, background) == ERR ||
      init_pair(BACA_PAIR_SEARCH, background, accent) == ERR ||
      init_pair(BACA_PAIR_ALERT, COLOR_RED, background) == ERR) {
    state->colors = false;
    if (state->image_mode == BACA_IMAGE_MODE_ANSI) {
      state->image_mode = BACA_IMAGE_MODE_PLACEHOLDER;
    }
    (void)wbkgd(stdscr, A_NORMAL);
    state->dirty = true;
    return;
  }
  (void)wbkgd(stdscr, (chtype)COLOR_PAIR(BACA_PAIR_BASE));
  state->dirty = true;
}

static void change_style(BacaTuiState *state, int row, int x, int width,
                         attr_t attributes, short pair) {
  if (row < 0 || row >= state->rows || x < 0 || x >= state->columns ||
      width <= 0) {
    return;
  }
  if (width > state->columns - x) {
    width = state->columns - x;
  }
  (void)mvwchgat(stdscr, row, x, width, attributes, state->colors ? pair : 0,
                 NULL);
}

static bool named_key_matches(const BacaNormalizedKey *key, const char *name) {
  if (name == NULL || name[0] == '\0') {
    return false;
  }
  if (name[1] == '\0') {
    return !key->key_code && key->character == (wchar_t)(unsigned char)name[0];
  }

  if (strncmp(name, "ctrl+", 5U) == 0 && name[5] != '\0' && name[6] == '\0') {
    const unsigned char letter = (unsigned char)tolower((unsigned char)name[5]);
    return !key->key_code && letter >= (unsigned char)'a' &&
           letter <= (unsigned char)'z' &&
           key->character == (wchar_t)(letter - (unsigned char)'a' + 1U);
  }
  if ((name[0] == 'f' || name[0] == 'F') &&
      isdigit((unsigned char)name[1]) != 0) {
    char *end = NULL;
    const long number = strtol(name + 1, &end, 10);
    return key->key_code && end != name + 1 && *end == '\0' && number >= 1L &&
           number <= 63L && key->code == KEY_F((int)number);
  }

  if (baca_casecmp(name, "down") == 0) {
    return key->key_code && key->code == KEY_DOWN;
  }
  if (baca_casecmp(name, "up") == 0) {
    return key->key_code && key->code == KEY_UP;
  }
  if (baca_casecmp(name, "left") == 0) {
    return key->key_code && key->code == KEY_LEFT;
  }
  if (baca_casecmp(name, "right") == 0) {
    return key->key_code && key->code == KEY_RIGHT;
  }
  if (baca_casecmp(name, "home") == 0) {
    return key->key_code && key->code == KEY_HOME;
  }
  if (baca_casecmp(name, "end") == 0) {
    return key->key_code && key->code == KEY_END;
  }
  if (baca_casecmp(name, "pageup") == 0 || baca_casecmp(name, "page_up") == 0) {
    return key->key_code && key->code == KEY_PPAGE;
  }
  if (baca_casecmp(name, "pagedown") == 0 ||
      baca_casecmp(name, "page_down") == 0) {
    return key->key_code && key->code == KEY_NPAGE;
  }
  if (baca_casecmp(name, "enter") == 0) {
    return (key->key_code && key->code == KEY_ENTER) ||
           (!key->key_code &&
            (key->character == L'\n' || key->character == L'\r'));
  }
  if (baca_casecmp(name, "escape") == 0 || baca_casecmp(name, "esc") == 0) {
    return !key->key_code && key->character == 27;
  }
  if (baca_casecmp(name, "tab") == 0) {
    return !key->key_code && key->character == L'\t';
  }
  if (baca_casecmp(name, "space") == 0) {
    return !key->key_code && key->character == L' ';
  }
  if (baca_casecmp(name, "slash") == 0) {
    return !key->key_code && key->character == L'/';
  }
  if (baca_casecmp(name, "question_mark") == 0 ||
      baca_casecmp(name, "question") == 0) {
    return !key->key_code && key->character == L'?';
  }
  if (baca_casecmp(name, "backspace") == 0) {
    return (key->key_code && key->code == KEY_BACKSPACE) ||
           (!key->key_code && (key->character == 8 || key->character == 127));
  }
  if (baca_casecmp(name, "delete") == 0) {
    return key->key_code && key->code == KEY_DC;
  }
  return false;
}

static bool key_list_matches(const BacaKeyList *list,
                             const BacaNormalizedKey *key) {
  for (size_t index = 0U; index < list->length; ++index) {
    if (named_key_matches(key, list->items[index])) {
      return true;
    }
  }
  return false;
}

static BacaCommand normalize_command(const BacaConfig *config,
                                     const BacaNormalizedKey *key) {
  const BacaKeymaps *maps = &config->keymaps;
  if (key_list_matches(&maps->close, key)) {
    return BACA_COMMAND_QUIT;
  }
  if (key_list_matches(&maps->scroll_down, key)) {
    return BACA_COMMAND_SCROLL_DOWN;
  }
  if (key_list_matches(&maps->scroll_up, key)) {
    return BACA_COMMAND_SCROLL_UP;
  }
  if (key_list_matches(&maps->page_down, key)) {
    return BACA_COMMAND_PAGE_DOWN;
  }
  if (key_list_matches(&maps->page_up, key)) {
    return BACA_COMMAND_PAGE_UP;
  }
  if (key_list_matches(&maps->home, key)) {
    return BACA_COMMAND_HOME;
  }
  if (key_list_matches(&maps->end, key)) {
    return BACA_COMMAND_END;
  }
  if (key_list_matches(&maps->open_toc, key)) {
    return BACA_COMMAND_TOC;
  }
  if (key_list_matches(&maps->add_bookmark, key)) {
    return BACA_COMMAND_ADD_BOOKMARK;
  }
  if (key_list_matches(&maps->open_bookmarks, key)) {
    return BACA_COMMAND_BOOKMARKS;
  }
  if (key_list_matches(&maps->open_metadata, key)) {
    return BACA_COMMAND_METADATA;
  }
  if (key_list_matches(&maps->open_help, key)) {
    return BACA_COMMAND_HELP;
  }
  if (key_list_matches(&maps->toggle_dark, key)) {
    return BACA_COMMAND_TOGGLE_THEME;
  }
  if (key_list_matches(&maps->search_forward, key)) {
    return BACA_COMMAND_SEARCH_FORWARD;
  }
  if (key_list_matches(&maps->search_backward, key)) {
    return BACA_COMMAND_SEARCH_BACKWARD;
  }
  if (key_list_matches(&maps->next_match, key)) {
    return BACA_COMMAND_NEXT_MATCH;
  }
  if (key_list_matches(&maps->previous_match, key)) {
    return BACA_COMMAND_PREVIOUS_MATCH;
  }
  if (key_list_matches(&maps->confirm, key)) {
    return BACA_COMMAND_CONFIRM;
  }
  if (key_list_matches(&maps->screenshot, key)) {
    return BACA_COMMAND_SCREENSHOT;
  }
  if (key_list_matches(&maps->toggle_pdf_view, key)) {
    return BACA_COMMAND_TOGGLE_PDF_VIEW;
  }
  return BACA_COMMAND_NONE;
}

static BacaNormalizedKey read_normalized_key(const BacaConfig *config,
                                             int status, wint_t input) {
  BacaNormalizedKey key = {0};
  if (status == ERR) {
    return key;
  }
  key.valid = true;
  if (status == KEY_CODE_YES) {
    key.key_code = true;
    key.code = (int)input;
  } else {
    key.character = (wchar_t)input;
  }
  key.command = normalize_command(config, &key);
  return key;
}

static void define_extended_keys(void) {
  /* Ghostty uses Kitty's CSI-u encoding to distinguish Ctrl-i from Tab. */
  (void)define_key("\033[105;5u", BACA_KEY_CTRL_I);
}

static size_t maximum_scroll(const BacaTuiState *state) {
  const size_t visible = state->rows > 0 ? (size_t)state->rows : 1U;
  return state->app->layout.line_count > visible
             ? state->app->layout.line_count - visible
             : 0U;
}

static size_t pdf_page_start(const BacaTuiState *state, size_t page) {
  if (page >= state->app->document.section_count ||
      state->app->layout.section_first_line == NULL) {
    return state->app->layout.line_count;
  }
  return state->app->layout.section_first_line[page];
}

static void pdf_position(const BacaTuiState *state, size_t *page,
                         double *intra_page) {
  const size_t page_count = state->app->document.section_count;
  *page = 0U;
  *intra_page = 0.0;
  if (page_count == 0U || state->app->layout.line_count == 0U) {
    return;
  }
  const size_t section = baca_layout_section_for_line(
      &state->app->layout, state->app->scroll_line, NULL);
  if (section != SIZE_MAX && section < page_count) {
    *page = section;
  }
  const size_t start = pdf_page_start(state, *page);
  const size_t end = *page + 1U < page_count ? pdf_page_start(state, *page + 1U)
                                             : state->app->layout.line_count;
  if (end > start + 1U && state->app->scroll_line > start) {
    const size_t offset =
        minimum_size(state->app->scroll_line - start, end - start - 1U);
    *intra_page = (double)offset / (double)(end - start - 1U);
  }
}

static size_t pdf_restore_line(const BacaTuiState *state, double progress) {
  const size_t page_count = state->app->document.section_count;
  if (page_count == 0U || !isfinite(progress) || !(progress > 0.0)) {
    return 0U;
  }
  if (progress > 1.0) {
    progress = 1.0;
  }
  long double scaled = (long double)progress * (long double)page_count;
  size_t page =
      scaled >= (long double)page_count ? page_count - 1U : (size_t)scaled;
  double intra_page = scaled >= (long double)page_count
                          ? 1.0
                          : (double)(scaled - (long double)page);
  const size_t start = pdf_page_start(state, page);
  const size_t end = page + 1U < page_count ? pdf_page_start(state, page + 1U)
                                            : state->app->layout.line_count;
  if (end <= start + 1U) {
    return start;
  }
  const size_t span = end - start - 1U;
  const size_t offset = (size_t)llround(intra_page * (double)span);
  return start + minimum_size(offset, span);
}

static size_t restore_progress_line(const BacaTuiState *state,
                                    double progress) {
  return state->app->document.format == BACA_FORMAT_PDF
             ? pdf_restore_line(state, progress)
             : baca_layout_restore_progress(progress, maximum_scroll(state));
}

static void remember_progress(BacaTuiState *state) {
  const size_t maximum = maximum_scroll(state);
  if (maximum == 0U) {
    return;
  }
  if (state->app->scroll_line >= maximum) {
    state->app->saved_progress = 1.0;
    return;
  }
  if (state->app->document.format == BACA_FORMAT_PDF &&
      state->app->document.section_count > 0U) {
    size_t page = 0U;
    double intra_page = 0.0;
    pdf_position(state, &page, &intra_page);
    state->app->saved_progress = ((double)page + intra_page) /
                                 (double)state->app->document.section_count;
    return;
  }
  state->app->saved_progress =
      baca_layout_progress(state->app->scroll_line, maximum);
}

static void set_scroll_line(BacaTuiState *state, size_t line) {
  const size_t maximum = maximum_scroll(state);
  state->app->scroll_line = line > maximum ? maximum : line;
  remember_progress(state);
  state->dirty = true;
}

static void cancel_animation(BacaTuiState *state) {
  state->animation.active = false;
}

static void jump_stack_push(double positions[BACA_JUMP_HISTORY_LIMIT],
                            size_t *count, double progress) {
  if (*count > 0U && positions[*count - 1U] == progress) {
    return;
  }
  if (*count == BACA_JUMP_HISTORY_LIMIT) {
    memmove(positions, positions + 1U,
            (BACA_JUMP_HISTORY_LIMIT - 1U) * sizeof(*positions));
    --*count;
  }
  positions[(*count)++] = progress;
}

static bool jump_to_line(BacaTuiState *state, size_t line) {
  const size_t target = minimum_size(line, maximum_scroll(state));
  if (target == state->app->scroll_line) {
    return false;
  }
  remember_progress(state);
  jump_stack_push(state->jumps.backward, &state->jumps.backward_count,
                  state->app->saved_progress);
  state->jumps.forward_count = 0U;
  cancel_animation(state);
  set_scroll_line(state, target);
  return true;
}

static bool traverse_jump_history(BacaTuiState *state, bool forward) {
  double *source = forward ? state->jumps.forward : state->jumps.backward;
  size_t *source_count =
      forward ? &state->jumps.forward_count : &state->jumps.backward_count;
  double *destination = forward ? state->jumps.backward : state->jumps.forward;
  size_t *destination_count =
      forward ? &state->jumps.backward_count : &state->jumps.forward_count;
  if (*source_count == 0U) {
    return false;
  }
  remember_progress(state);
  jump_stack_push(destination, destination_count, state->app->saved_progress);
  const double progress = source[--*source_count];
  cancel_animation(state);
  set_scroll_line(state, restore_progress_line(state, progress));
  return true;
}

static void scroll_lines(BacaTuiState *state, int lines) {
  cancel_animation(state);
  size_t target = state->app->scroll_line;
  if (lines < 0) {
    const size_t amount = (size_t)(-(long)lines);
    target = amount > target ? 0U : target - amount;
  } else {
    const size_t amount = (size_t)lines;
    const size_t maximum = maximum_scroll(state);
    target = amount > maximum - minimum_size(target, maximum) ? maximum
                                                              : target + amount;
  }
  set_scroll_line(state, target);
}

static void start_scroll_animation_with_duration(BacaTuiState *state,
                                                 size_t target,
                                                 double duration) {
  const size_t maximum = maximum_scroll(state);
  if (target > maximum) {
    target = maximum;
  }
  if (!isfinite(duration) || duration <= 0.0 ||
      target == state->app->scroll_line) {
    cancel_animation(state);
    set_scroll_line(state, target);
    return;
  }
  state->animation.from = (double)state->app->scroll_line;
  state->animation.to = (double)target;
  state->animation.started_at = baca_terminal_monotonic_seconds();
  state->animation.duration = duration;
  state->animation.active = true;
}

static void start_scroll_animation(BacaTuiState *state, size_t target) {
  start_scroll_animation_with_duration(state, target,
                                       state->app->config.page_scroll_duration);
}

static size_t pending_scroll_target(const BacaTuiState *state) {
  if (!state->animation.active || !isfinite(state->animation.to) ||
      state->animation.to < 0.0) {
    return state->app->scroll_line;
  }
  if (state->animation.to >= (double)SIZE_MAX) {
    return SIZE_MAX;
  }
  return (size_t)llround(state->animation.to);
}

static void page_scroll(BacaTuiState *state, bool down, size_t count) {
  const size_t amount = state->rows > 1 ? (size_t)(state->rows - 1) : 1U;
  const size_t maximum = maximum_scroll(state);
  const size_t distance = amount > maximum / count ? maximum : amount * count;
  size_t target = minimum_size(pending_scroll_target(state), maximum);
  if (down) {
    target = distance > maximum - minimum_size(target, maximum)
                 ? maximum
                 : target + distance;
  } else {
    target = distance > target ? 0U : target - distance;
  }
  start_scroll_animation(state, target);
}

static void update_animation(BacaTuiState *state) {
  if (!state->animation.active) {
    return;
  }
  const double elapsed =
      baca_terminal_monotonic_seconds() - state->animation.started_at;
  double amount = elapsed / state->animation.duration;
  if (amount >= 1.0) {
    state->animation.active = false;
    set_scroll_line(state, (size_t)llround(state->animation.to));
    return;
  }
  if (amount < 0.0) {
    amount = 0.0;
  }
  const double inverse = 1.0 - amount;
  const double eased = 1.0 - inverse * inverse * inverse;
  const double line = state->animation.from +
                      (state->animation.to - state->animation.from) * eased;
  set_scroll_line(state, (size_t)llround(line));
}

static void clear_search(BacaSearchState *search) {
  free(search->pattern);
  free(search->matches);
  memset(search, 0, sizeof(*search));
}

static bool select_initial_match(BacaTuiState *state, bool forward) {
  if (state->search.match_count == 0U) {
    return false;
  }
  if (forward) {
    for (size_t index = 0U; index < state->search.match_count; ++index) {
      if (state->search.matches[index].line >= state->app->scroll_line) {
        state->search.current_match = index;
        return true;
      }
    }
  } else {
    for (size_t index = state->search.match_count; index > 0U; --index) {
      if (state->search.matches[index - 1U].line <= state->app->scroll_line) {
        state->search.current_match = index - 1U;
        return true;
      }
    }
  }
  return false;
}

static void reveal_current_match(BacaTuiState *state, bool record_jump) {
  if (!state->search.active ||
      state->search.current_match >= state->search.match_count) {
    return;
  }
  const size_t line = state->search.matches[state->search.current_match].line;
  const size_t visible = state->rows > 0 ? (size_t)state->rows : 1U;
  if (line < state->app->scroll_line) {
    if (record_jump) {
      (void)jump_to_line(state, line);
    } else {
      set_scroll_line(state, line);
    }
  } else if (line >= state->app->scroll_line + visible) {
    const size_t target = line >= visible ? line - visible + 1U : 0U;
    if (record_jump) {
      (void)jump_to_line(state, target);
    } else {
      set_scroll_line(state, target);
    }
  }
}

static void submit_search(BacaTuiState *state) {
  BacaSearchMatch *matches = NULL;
  size_t match_count = 0U;
  BacaError error = {0};
  if (!baca_layout_search(&state->app->layout, state->prompt.text.value,
                          &matches, &match_count, &error)) {
    open_alert(state, error.message[0] != '\0' ? error.message
                                               : "Invalid regular expression");
    return;
  }

  clear_search(&state->search);
  state->search.pattern = baca_strdup(state->prompt.text.value, &error);
  if (state->search.pattern == NULL) {
    free(matches);
    open_alert(state,
               error.message[0] != '\0' ? error.message : "Out of memory");
    return;
  }
  state->search.matches = matches;
  state->search.match_count = match_count;
  state->search.saved_progress = state->prompt.saved_progress;
  state->search.forward = state->prompt.forward;
  state->search.active = true;
  if (!select_initial_match(state, state->search.forward)) {
    char message[512] = {0};
    (void)snprintf(message, sizeof(message), "Found no match: '%s'",
                   state->search.pattern);
    clear_search(&state->search);
    open_alert(state, message);
    return;
  }
  reveal_current_match(state, true);
}

static bool repeat_search(BacaTuiState *state, bool same_direction) {
  if (!state->search.active || state->search.match_count == 0U) {
    return false;
  }
  const bool forward =
      same_direction ? state->search.forward : !state->search.forward;
  if (forward) {
    if (state->search.current_match + 1U >= state->search.match_count) {
      open_alert(state, "No further match");
      return false;
    }
    ++state->search.current_match;
  } else {
    if (state->search.current_match == 0U) {
      open_alert(state, "No further match");
      return false;
    }
    --state->search.current_match;
  }
  reveal_current_match(state, true);
  return true;
}

static void cancel_search(BacaTuiState *state) {
  if (!state->search.active) {
    return;
  }
  const size_t target =
      restore_progress_line(state, state->search.saved_progress);
  clear_search(&state->search);
  start_scroll_animation(state, target);
}

static bool refresh_search_after_layout(BacaTuiState *state) {
  if (!state->search.active) {
    return true;
  }
  BacaSearchMatch selected = {0};
  const bool had_selected =
      state->search.current_match < state->search.match_count;
  if (had_selected) {
    selected = state->search.matches[state->search.current_match];
  }
  free(state->search.matches);
  state->search.matches = NULL;
  state->search.match_count = 0U;
  BacaError error = {0};
  if (!baca_layout_search(&state->app->layout, state->search.pattern,
                          &state->search.matches, &state->search.match_count,
                          &error)) {
    open_alert(state, error.message[0] != '\0'
                          ? error.message
                          : "Search match disappeared after resize");
    clear_search(&state->search);
    return false;
  }

  bool restored = !had_selected;
  if (had_selected) {
    const size_t selected_length = selected.byte_end - selected.byte_start;
    for (size_t index = 0U; index < state->search.match_count; ++index) {
      const BacaSearchMatch *candidate = &state->search.matches[index];
      if (candidate->block_index == selected.block_index &&
          candidate->block_offset == selected.block_offset &&
          candidate->byte_end - candidate->byte_start == selected_length) {
        state->search.current_match = index;
        restored = true;
        break;
      }
    }
  } else {
    restored = select_initial_match(state, state->search.forward);
  }
  if (!restored) {
    open_alert(state, "Search match disappeared after resize");
    clear_search(&state->search);
    return false;
  }
  reveal_current_match(state, false);
  return true;
}

static void *parser_thread(void *argument) {
  BacaParserJob *job = argument;
  const bool succeeded = baca_layout_build_presentation(
      &job->layout, job->document, job->width, job->justification,
      job->presentation, job->cell_pixel_width, job->cell_pixel_height,
      &job->cancel, &job->error);
  atomic_store_explicit(&job->succeeded, succeeded, memory_order_relaxed);
  atomic_store_explicit(&job->done, true, memory_order_release);
  return NULL;
}

static void draw_loader(BacaTuiState *state, unsigned phase) {
  static const char frames[] = {'|', '/', '-', '\\'};
  char text[16] = {0};
  (void)snprintf(text, sizeof(text), "Loading %c",
                 frames[phase % BACA_ARRAY_LEN(frames)]);
  (void)werase(stdscr);
  (void)wbkgd(stdscr,
              state->colors ? (chtype)COLOR_PAIR(BACA_PAIR_BASE) : A_NORMAL);
  const int y = state->rows / 2;
  int x = (state->columns - (int)strlen(text)) / 2;
  if (x < 0) {
    x = 0;
  }
  add_clipped(stdscr, y, x, text, state->columns - x);
  (void)refresh();
}

static bool build_layout_in_background(BacaTuiState *state, BacaLayout *layout,
                                       BacaError *error) {
  for (;;) {
    BacaParserJob job = {
        .document = &state->app->document,
        .width = state->content_width,
        .justification = state->app->config.justification,
        .presentation = state->presentation,
        .cell_pixel_width = state->cell_pixel_width,
        .cell_pixel_height = state->cell_pixel_height,
    };
    atomic_init(&job.cancel, false);
    atomic_init(&job.done, false);
    atomic_init(&job.succeeded, false);

    pthread_t thread = {0};
    const int create_result =
        pthread_create(&thread, NULL, parser_thread, &job);
    if (create_result != 0) {
      baca_error_set(error, BACA_ERROR_INTERNAL,
                     "cannot start parser thread: %s", strerror(create_result));
      return false;
    }

    unsigned phase = 0U;
    draw_loader(state, phase++);
    while (!atomic_load_explicit(&job.done, memory_order_acquire)) {
      if (baca_terminal_runtime_interrupted()) {
        state->quit = true;
        atomic_store_explicit(&job.cancel, true, memory_order_relaxed);
      }
      update_dimensions(state);
      if (job.width != state->content_width ||
          job.cell_pixel_width != state->cell_pixel_width ||
          job.cell_pixel_height != state->cell_pixel_height ||
          job.presentation != state->presentation) {
        atomic_store_explicit(&job.cancel, true, memory_order_relaxed);
      }
      draw_loader(state, phase++);
      wtimeout(stdscr, 80);
      wint_t input = 0;
      const int status = wget_wch(stdscr, &input);
      const BacaNormalizedKey key =
          read_normalized_key(&state->app->config, status, input);
      if (baca_terminal_runtime_interrupted()) {
        state->quit = true;
        atomic_store_explicit(&job.cancel, true, memory_order_relaxed);
      }
      if (key.valid && key.key_code && key.code == KEY_RESIZE) {
        update_dimensions(state);
        atomic_store_explicit(&job.cancel, true, memory_order_relaxed);
      } else if (key.valid && key.command == BACA_COMMAND_QUIT) {
        state->quit = true;
        atomic_store_explicit(&job.cancel, true, memory_order_relaxed);
      }
    }

    const int join_result = pthread_join(thread, NULL);
    if (join_result != 0) {
      baca_layout_free(&job.layout);
      baca_error_set(error, BACA_ERROR_INTERNAL,
                     "cannot join parser thread: %s", strerror(join_result));
      return false;
    }
    const bool cancelled =
        atomic_load_explicit(&job.cancel, memory_order_relaxed);
    if (!atomic_load_explicit(&job.succeeded, memory_order_relaxed)) {
      baca_layout_free(&job.layout);
      if (cancelled) {
        if (state->quit) {
          return true;
        }
        continue;
      }
      *error = job.error;
      return false;
    }
    if (state->quit) {
      baca_layout_free(&job.layout);
      return true;
    }
    if (job.width != state->content_width ||
        job.cell_pixel_width != state->cell_pixel_width ||
        job.cell_pixel_height != state->cell_pixel_height ||
        job.presentation != state->presentation) {
      baca_layout_free(&job.layout);
      continue;
    }
    *layout = job.layout;
    return true;
  }
}

static bool rebuild_layout(BacaTuiState *state, BacaError *error) {
  double progress = state->app->saved_progress;
  if (state->app->layout.line_count > 0U) {
    remember_progress(state);
    progress = state->app->saved_progress;
  }

  BacaLayout layout = {0};
  if (!build_layout_in_background(state, &layout, error)) {
    return false;
  }
  if (state->quit) {
    return true;
  }

  baca_layout_free(&state->app->layout);
  state->app->layout = layout;
  const size_t restored = restore_progress_line(state, progress);
  state->app->scroll_line = minimum_size(restored, maximum_scroll(state));
  remember_progress(state);
  cancel_animation(state);
  (void)refresh_search_after_layout(state);
  state->dirty = true;
  return true;
}

static bool open_external(BacaTuiState *state, const char *target,
                          const char *preferred, BacaError *error) {
  delete_kitty_images(state);
  if (state->image_mode == BACA_IMAGE_MODE_ANSI && state->raw_truecolor) {
    (void)clearok(stdscr, true);
    (void)refresh();
  }
  if (def_prog_mode() == ERR) {
    baca_error_set(error, BACA_ERROR_EXTERNAL,
                   "cannot suspend terminal for external opener");
    return false;
  }

  const int previous_rows = state->rows;
  const int previous_content_width = state->content_width;
  (void)endwin();
  bool opened =
      state->app->external_opener != NULL
          ? state->app->external_opener(state->app->external_opener_data,
                                        target, preferred, error)
          : baca_platform_open(target, preferred, error);
  if (reset_prog_mode() == ERR) {
    if (opened) {
      baca_error_set(error, BACA_ERROR_EXTERNAL,
                     "cannot resume terminal after external opener");
    }
    opened = false;
  }
  (void)keypad(stdscr, true);
  define_extended_keys();
  (void)clearok(stdscr, true);
  update_dimensions(state);

  if (!baca_terminal_runtime_interrupted() &&
      previous_content_width != state->content_width) {
    BacaError resize_error = {0};
    if (!rebuild_layout(state, &resize_error) && !state->quit) {
      if (opened || !baca_error_is_set(error)) {
        *error = resize_error;
      }
      opened = false;
    }
  } else if (previous_rows != state->rows) {
    state->app->scroll_line =
        minimum_size(restore_progress_line(state, state->app->saved_progress),
                     maximum_scroll(state));
    remember_progress(state);
  }
  state->dirty = true;
  return opened;
}

static int rendered_column_for_raw_byte(const char *raw, const char *rendered,
                                        size_t target) {
  const size_t raw_length = strlen(raw);
  const size_t rendered_length = strlen(rendered);
  size_t raw_offset = 0U;
  size_t rendered_offset = 0U;
  int rendered_columns = 0;
  if (target > raw_length) {
    target = raw_length;
  }

  while (raw_offset < target && rendered_offset < rendered_length) {
    if (raw[raw_offset] == ' ') {
      size_t raw_end = raw_offset;
      size_t rendered_end = rendered_offset;
      while (raw_end < raw_length && raw[raw_end] == ' ') {
        ++raw_end;
      }
      while (rendered_end < rendered_length && rendered[rendered_end] == ' ') {
        ++rendered_end;
      }
      if (target <= raw_end) {
        if (target == raw_end) {
          return rendered_columns + size_to_int(rendered_end - rendered_offset);
        }
        return rendered_columns + size_to_int(target - raw_offset);
      }
      rendered_columns += size_to_int(rendered_end - rendered_offset);
      raw_offset = raw_end;
      rendered_offset = rendered_end;
      continue;
    }

    int raw_columns = 0;
    const size_t raw_next =
        baca_utf8_next(raw, raw_length, raw_offset, &raw_columns);
    int display_columns = 0;
    size_t rendered_next = baca_utf8_next(rendered, rendered_length,
                                          rendered_offset, &display_columns);
    const size_t raw_bytes = raw_next - raw_offset;
    while (rendered_next > rendered_offset &&
           (rendered_next - rendered_offset != raw_bytes ||
            memcmp(raw + raw_offset, rendered + rendered_offset, raw_bytes) !=
                0)) {
      rendered_columns += display_columns;
      rendered_offset = rendered_next;
      if (rendered_offset >= rendered_length) {
        break;
      }
      rendered_next = baca_utf8_next(rendered, rendered_length, rendered_offset,
                                     &display_columns);
    }
    if (rendered_offset >= rendered_length || raw_next <= raw_offset ||
        rendered_next <= rendered_offset) {
      break;
    }
    rendered_columns += display_columns;
    rendered_offset = rendered_next;
    raw_offset = raw_next;
  }
  return rendered_columns;
}

static void style_text_line(BacaTuiState *state, int row, int x,
                            size_t line_index, const char *raw,
                            const char *rendered) {
  const BacaLayoutLine *line = &state->app->layout.lines[line_index];
  if (line->block_index >= state->app->document.block_count) {
    return;
  }
  const BacaBlock *block = &state->app->document.blocks[line->block_index];
  if (block->kind != BACA_BLOCK_TEXT) {
    return;
  }
  const BacaTextBlock *text = &block->value.text;
  if (text->heading_level > 0U) {
    const int width = size_to_int(baca_utf8_width(rendered, strlen(rendered)));
    change_style(state, row, x, width, A_BOLD, BACA_PAIR_BASE);
  }

  for (size_t index = 0U; index < text->span_count; ++index) {
    const BacaTextSpan *span = &text->spans[index];
    const size_t start =
        span->start > line->byte_start ? span->start : line->byte_start;
    const size_t end = span->end < line->byte_end ? span->end : line->byte_end;
    if (start >= end) {
      continue;
    }
    const size_t indent = line->indent > 0 ? (size_t)line->indent : 0U;
    const int start_column = rendered_column_for_raw_byte(
        raw, rendered, indent + start - line->byte_start);
    const int end_column = rendered_column_for_raw_byte(
        raw, rendered, indent + end - line->byte_start);
    if (end_column <= start_column) {
      continue;
    }
    attr_t attributes = A_NORMAL;
    if ((span->style & (BACA_STYLE_BOLD | BACA_STYLE_HEADING)) != 0U) {
      attributes |= A_BOLD;
    }
    if ((span->style & BACA_STYLE_UNDERLINE) != 0U || span->link != NULL) {
      attributes |= A_UNDERLINE;
    }
#ifdef A_ITALIC
    if ((span->style & BACA_STYLE_ITALIC) != 0U) {
      attributes |= A_ITALIC;
    }
#endif
    const short pair = span->link != NULL ? BACA_PAIR_ACCENT : BACA_PAIR_BASE;
    change_style(state, row, x + start_column, end_column - start_column,
                 attributes, pair);
  }
}

static void highlight_search_match(BacaTuiState *state, int row, int x,
                                   size_t line_index, const char *raw,
                                   const char *rendered) {
  if (!state->search.active ||
      state->search.current_match >= state->search.match_count) {
    return;
  }
  const BacaSearchMatch *match =
      &state->search.matches[state->search.current_match];
  if (match->line != line_index) {
    return;
  }
  const int start =
      rendered_column_for_raw_byte(raw, rendered, match->byte_start);
  const int end = rendered_column_for_raw_byte(raw, rendered, match->byte_end);
  if (end > start) {
    const attr_t attributes = A_BOLD | (state->colors ? A_NORMAL : A_REVERSE);
    change_style(state, row, x + start, end - start, attributes,
                 BACA_PAIR_SEARCH);
  }
}

static bool first_visible_image_line(const BacaTuiState *state, int row,
                                     size_t line_index) {
  if (row == 0 || line_index == 0U) {
    return true;
  }
  const BacaLayoutLine *line = &state->app->layout.lines[line_index];
  const BacaLayoutLine *previous = &state->app->layout.lines[line_index - 1U];
  return previous->kind != BACA_LAYOUT_IMAGE ||
         previous->block_index != line->block_index;
}

static bool pdf_render_failure_matches(const BacaTuiState *state,
                                       size_t block_index,
                                       const BacaLayoutLine *line) {
  if (state->app->document.format != BACA_FORMAT_PDF ||
      block_index >= state->pdf_render_failure_count ||
      state->pdf_render_failures == NULL) {
    return false;
  }
  const BacaPdfRenderFailure *failure =
      &state->pdf_render_failures[block_index];
  return failure->valid && failure->columns == state->content_width &&
         failure->rows == line->image_rows &&
         failure->cell_pixel_width == state->cell_pixel_width &&
         failure->cell_pixel_height == state->cell_pixel_height;
}

static bool image_has_visible_render(const BacaTuiState *state,
                                     const BacaLayoutLine *line) {
  if (line->kind != BACA_LAYOUT_IMAGE ||
      line->block_index >= state->app->document.block_count ||
      line->image_placeholder ||
      state->image_mode == BACA_IMAGE_MODE_PLACEHOLDER ||
      state->graphics == NULL) {
    return false;
  }
  const BacaBlock *block = &state->app->document.blocks[line->block_index];
  return block->kind == BACA_BLOCK_IMAGE && !block->value.image.broken &&
         !pdf_render_failure_matches(state, line->block_index, line);
}

static void clear_pdf_render_failure(BacaTuiState *state, size_t block_index) {
  if (block_index < state->pdf_render_failure_count &&
      state->pdf_render_failures != NULL) {
    state->pdf_render_failures[block_index] = (BacaPdfRenderFailure){0};
  }
}

static bool mark_image_failure(BacaTuiState *state, size_t block_index,
                               const BacaLayoutLine *line,
                               const BacaError *error) {
  bool rendered_pdf_page = false;
  if (block_index < state->app->document.block_count &&
      state->app->document.blocks[block_index].kind == BACA_BLOCK_IMAGE) {
    BacaImageBlock *image =
        &state->app->document.blocks[block_index].value.image;
    rendered_pdf_page = state->app->document.format == BACA_FORMAT_PDF &&
                        image->page_index >= 0;
    if (rendered_pdf_page &&
        (error == NULL || (error->code != BACA_ERROR_CORRUPT &&
                           error->code != BACA_ERROR_UNSUPPORTED))) {
      if (block_index >= state->pdf_render_failure_count ||
          state->pdf_render_failures == NULL || line == NULL) {
        return false;
      }
      BacaPdfRenderFailure *failure = &state->pdf_render_failures[block_index];
      const bool repeated =
          pdf_render_failure_matches(state, block_index, line);
      *failure = (BacaPdfRenderFailure){
          .columns = state->content_width,
          .rows = line->image_rows,
          .cell_pixel_width = state->cell_pixel_width,
          .cell_pixel_height = state->cell_pixel_height,
          .valid = true,
      };
      return !repeated;
    }
    image->intrinsic_width = 0;
    image->intrinsic_height = 0;
    image->broken = true;
  }
  state->dirty = true;
  return rendered_pdf_page;
}

static bool
prepare_image_placement(BacaTuiState *state, const BacaLayoutLine *line,
                        int row, const BacaGraphicsRect *occlusions,
                        size_t occlusion_count, BacaGraphicsSurface *surface,
                        BacaGraphicsPlacement *placement, BacaError *error) {
  if (state->graphics == NULL || line->image_placeholder ||
      line->block_index >= state->app->document.block_count) {
    return false;
  }
  if (pdf_render_failure_matches(state, line->block_index, line)) {
    return false;
  }
  if (!baca_graphics_prepare(state->graphics, &state->app->document,
                             line->block_index, state->content_width,
                             line->image_rows, surface, error)) {
    return false;
  }
  clear_pdf_render_failure(state, line->block_index);
  *placement = (BacaGraphicsPlacement){
      .row = row - line->image_row,
      .column = state->content_x,
      .viewport_rows = state->rows,
      .viewport_columns = state->columns,
      .occlusions = occlusions,
      .occlusion_count = occlusion_count,
  };
  return true;
}

static bool draw_image_cell(void *user_data, int row, int column,
                            uint32_t foreground, uint32_t background) {
  BacaTuiState *state = user_data;
  const unsigned foreground_index =
      baca_graphics_rgb_to_palette(foreground, (unsigned)COLORS);
  const unsigned background_index =
      baca_graphics_rgb_to_palette(background, (unsigned)COLORS);
  bool created = false;
  const short pair = baca_graphics_pair(state->graphics, foreground_index,
                                        background_index, BACA_PAIR_IMAGE_FIRST,
                                        state->image_pair_capacity, &created);
  if (pair <= 0 || (created && init_pair(pair, (short)foreground_index,
                                         (short)background_index) == ERR)) {
    return false;
  }
  cchar_t character = {0};
  return setcchar(&character, L"\u2580", A_NORMAL, pair, NULL) != ERR &&
         mvwadd_wch(stdscr, row, column, &character) != ERR;
}

static bool draw_ncurses_image(BacaTuiState *state, const BacaLayoutLine *line,
                               int row, BacaError *error) {
  BacaGraphicsSurface surface = {0};
  BacaGraphicsPlacement placement = {0};
  if (!prepare_image_placement(state, line, row, NULL, 0U, &surface, &placement,
                               error)) {
    return false;
  }
  const bool drawn = baca_graphics_render_cells(&surface, &placement,
                                                draw_image_cell, state, error);
  baca_graphics_surface_release(&surface);
  return drawn;
}

static bool image_content_bounds(const BacaTuiState *state,
                                 const BacaLayoutLine *line, size_t line_index,
                                 int *x, int *width) {
  if (line->kind != BACA_LAYOUT_IMAGE ||
      line->block_index >= state->app->document.block_count) {
    return false;
  }
  if (image_has_visible_render(state, line)) {
    int image_x = state->content_x;
    int visible_width = state->content_width;
    if (image_x < 0) {
      visible_width += image_x;
      image_x = 0;
    }
    if (image_x >= state->columns) {
      return false;
    }
    if (visible_width > state->columns - image_x) {
      visible_width = state->columns - image_x;
    }
    if (visible_width <= 0) {
      return false;
    }
    *x = image_x;
    *width = visible_width;
    return true;
  }
  const size_t image_row = line->image_row > 0 ? (size_t)line->image_row : 0U;
  const size_t image_rows =
      line->image_rows > 0 ? (size_t)line->image_rows : 1U;
  const size_t image_start =
      line_index >= image_row ? line_index - image_row : 0U;
  const size_t image_end =
      image_rows > SIZE_MAX - image_start ? SIZE_MAX : image_start + image_rows;
  const size_t viewport_end =
      (size_t)state->rows > SIZE_MAX - state->app->scroll_line
          ? SIZE_MAX
          : state->app->scroll_line + (size_t)state->rows;
  const size_t visible_start = image_start > state->app->scroll_line
                                   ? image_start
                                   : state->app->scroll_line;
  const size_t visible_end =
      image_end < viewport_end ? image_end : viewport_end;
  if (visible_start >= visible_end ||
      line_index != visible_start + (visible_end - visible_start - 1U) / 2U) {
    return false;
  }
  const int label_width = (int)(sizeof(BACA_IMAGE_PLACEHOLDER) - 1U);
  int image_x = state->content_x + (state->content_width - label_width) / 2;
  if (image_x < 0) {
    image_x = 0;
  }
  if (image_x >= state->columns) {
    return false;
  }
  int visible_width = label_width;
  if (visible_width > state->columns - image_x) {
    visible_width = state->columns - image_x;
  }
  if (visible_width <= 0) {
    return false;
  }
  *x = image_x;
  *width = visible_width;
  return true;
}

static void draw_image_placeholder(const BacaTuiState *state,
                                   const BacaLayoutLine *line, int row,
                                   size_t line_index) {
  const size_t image_row = line->image_row > 0 ? (size_t)line->image_row : 0U;
  const size_t image_rows =
      line->image_rows > 0 ? (size_t)line->image_rows : 1U;
  const size_t image_start =
      line_index >= image_row ? line_index - image_row : 0U;
  const size_t image_end =
      image_rows > SIZE_MAX - image_start ? SIZE_MAX : image_start + image_rows;
  const size_t viewport_end =
      (size_t)state->rows > SIZE_MAX - state->app->scroll_line
          ? SIZE_MAX
          : state->app->scroll_line + (size_t)state->rows;
  const size_t visible_start = image_start > state->app->scroll_line
                                   ? image_start
                                   : state->app->scroll_line;
  const size_t visible_end =
      image_end < viewport_end ? image_end : viewport_end;
  if (visible_start >= visible_end ||
      line_index != visible_start + (visible_end - visible_start - 1U) / 2U) {
    return;
  }
  const int label_width = (int)(sizeof(BACA_IMAGE_PLACEHOLDER) - 1U);
  int x = state->content_x + (state->content_width - label_width) / 2;
  if (x < 0) {
    x = 0;
  }
  add_clipped(stdscr, row, x, BACA_IMAGE_PLACEHOLDER, label_width);
}

static void draw_document_line(BacaTuiState *state, int row,
                               size_t line_index) {
  if (line_index >= state->app->layout.line_count) {
    return;
  }
  const BacaLayoutLine *line = &state->app->layout.lines[line_index];
  int x = state->content_x;
  if (x < 0) {
    x = 0;
  }
  if (line->kind == BACA_LAYOUT_IMAGE) {
    const BacaImageBlock *image =
        line->block_index < state->app->document.block_count
            ? &state->app->document.blocks[line->block_index].value.image
            : NULL;
    if (image != NULL && !image->broken && !line->image_placeholder &&
        state->image_mode == BACA_IMAGE_MODE_ANSI && !state->raw_truecolor &&
        first_visible_image_line(state, row, line_index)) {
      BacaError image_error = {0};
      if (!draw_ncurses_image(state, line, row, &image_error)) {
        if (mark_image_failure(state, line->block_index, line, &image_error) &&
            image_error.message[0] != '\0') {
          open_alert(state, image_error.message);
        }
      }
    }
    if (image == NULL || image->broken || line->image_placeholder ||
        state->image_mode == BACA_IMAGE_MODE_PLACEHOLDER ||
        state->image_mode == BACA_IMAGE_MODE_KITTY) {
      draw_image_placeholder(state, line, row, line_index);
    }
    return;
  }
  if (line->kind != BACA_LAYOUT_TEXT) {
    return;
  }

  BacaError ignored = {0};
  char *rendered =
      baca_layout_line_text(&state->app->layout, line_index, true, &ignored);
  if (rendered == NULL) {
    return;
  }
  add_clipped(stdscr, row, x, rendered, state->columns - x);

  char *raw =
      baca_layout_line_text(&state->app->layout, line_index, false, &ignored);
  if (raw != NULL) {
    style_text_line(state, row, x, line_index, raw, rendered);
    highlight_search_match(state, row, x, line_index, raw, rendered);
  }
  free(raw);
  free(rendered);
}

static void draw_scrollbar(BacaTuiState *state) {
  if (state->columns < 2 || state->rows < 1 ||
      state->app->layout.line_count <= (size_t)state->rows) {
    return;
  }
  const int x = state->columns - 1;
  (void)wattron(stdscr,
                A_DIM | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
  (void)mvvline(0, x, ACS_VLINE, state->rows);
  (void)wattroff(stdscr,
                 A_DIM | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));

  int thumb_height = (int)(((double)state->rows * (double)state->rows) /
                           (double)state->app->layout.line_count);
  if (thumb_height < 1) {
    thumb_height = 1;
  }
  if (thumb_height > state->rows) {
    thumb_height = state->rows;
  }
  const size_t maximum = maximum_scroll(state);
  int thumb_y = 0;
  if (maximum > 0U) {
    thumb_y =
        (int)llround((double)state->app->scroll_line *
                     (double)(state->rows - thumb_height) / (double)maximum);
  }
  (void)wattron(stdscr, state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : A_BOLD);
  (void)mvvline(thumb_y, x, ACS_CKBOARD, thumb_height);
  (void)wattroff(stdscr, state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : A_BOLD);
}

static void draw_reader_help_hint(BacaTuiState *state) {
  const BacaKeyList *keys = &state->app->config.keymaps.open_help;
  if (state->rows < 1 || keys->length == 0U || keys->items[0] == NULL ||
      state->overlay != BACA_OVERLAY_NONE || state->prompt.active) {
    return;
  }
  const char *configured = keys->items[0];
  const char *key =
      baca_casecmp(configured, "question_mark") == 0 ? "?" : configured;
  char hint[64] = {0};
  (void)snprintf(hint, sizeof(hint), "%s help", key);
  const int width = size_to_int(strlen(hint));
  const int x = state->content_x + state->content_width + 1;
  if (x < 0 || x + width >= state->columns - 1) {
    return;
  }
  (void)wattron(stdscr,
                A_DIM | (state->colors ? COLOR_PAIR(BACA_PAIR_BASE) : 0));
  (void)mvwhline(stdscr, state->rows - 1, x, ' ', width);
  add_clipped(stdscr, state->rows - 1, x, hint, width);
  (void)wattroff(stdscr,
                 A_DIM | (state->colors ? COLOR_PAIR(BACA_PAIR_BASE) : 0));
}

static BacaOverlayBox overlay_box(const BacaTuiState *state) {
  int width = state->columns - state->columns / 5;
  int height = state->rows - state->rows / 5;
  if (width > 84) {
    width = 84;
  }
  if (width < 3) {
    width = state->columns;
  }
  if (height < 3) {
    height = state->rows;
  }
  return (BacaOverlayBox){
      .y = (state->rows - height) / 2,
      .x = (state->columns - width) / 2,
      .height = height,
      .width = width,
  };
}

static const char *overlay_title(BacaOverlayKind overlay) {
  switch (overlay) {
  case BACA_OVERLAY_TOC:
    return "Table of Contents";
  case BACA_OVERLAY_BOOKMARKS:
    return "Bookmarks (Delete removes)";
  case BACA_OVERLAY_METADATA:
    return "Metadata";
  case BACA_OVERLAY_HELP:
    return "Help";
  case BACA_OVERLAY_ALERT:
    return "!";
  case BACA_OVERLAY_NONE:
    break;
  }
  return "";
}

static void draw_overlay_title(WINDOW *window, int width, const char *title) {
  const int title_width = size_to_int(baca_utf8_width(title, strlen(title)));
  int x = (width - title_width) / 2;
  if (x < 1) {
    x = 1;
  }
  add_clipped(window, 0, x, title, width - x - 1);
}

static void draw_toc_overlay(BacaTuiState *state, WINDOW *window, int height,
                             int width) {
  const int visible = height > 2 ? height - 2 : 0;
  if (visible <= 0) {
    return;
  }
  if (state->toc_index < state->overlay_scroll) {
    state->overlay_scroll = state->toc_index;
  } else if (state->toc_index >= state->overlay_scroll + (size_t)visible) {
    state->overlay_scroll = state->toc_index - (size_t)visible + 1U;
  }

  for (int row = 0; row < visible; ++row) {
    const size_t index = state->overlay_scroll + (size_t)row;
    if (index >= state->app->document.toc_count) {
      break;
    }
    const BacaTocEntry *entry = &state->app->document.toc[index];
    int x = 2 + (int)minimum_size((size_t)entry->depth * 2U,
                                  (size_t)(width > 4 ? width - 4 : 0));
    if (index == state->toc_index) {
      (void)wattron(window,
                    state->colors ? COLOR_PAIR(BACA_PAIR_SEARCH) : A_REVERSE);
      (void)mvwhline(window, row + 1, 1, ' ', width > 2 ? width - 2 : 0);
    }
    add_clipped(window, row + 1, x, entry->label != NULL ? entry->label : "",
                width - x - 1);
    if (index == state->toc_index) {
      (void)wattroff(window,
                     state->colors ? COLOR_PAIR(BACA_PAIR_SEARCH) : A_REVERSE);
    }
  }
}

static void draw_bookmarks_overlay(BacaTuiState *state, WINDOW *window,
                                   int height, int width) {
  const int visible = height > 2 ? height - 2 : 0;
  if (visible <= 0) {
    return;
  }
  if (state->bookmark_index < state->overlay_scroll) {
    state->overlay_scroll = state->bookmark_index;
  } else if (state->bookmark_index >= state->overlay_scroll + (size_t)visible) {
    state->overlay_scroll = state->bookmark_index - (size_t)visible + 1U;
  }

  for (int row = 0; row < visible; ++row) {
    const size_t index = state->overlay_scroll + (size_t)row;
    if (index >= state->bookmarks.length) {
      break;
    }
    const BacaBookmark *bookmark = &state->bookmarks.items[index];
    char label[96] = {0};
    (void)snprintf(label, sizeof(label), "%6.2f%%  %s",
                   bookmark->reading_progress * 100.0,
                   bookmark->created_at != NULL ? bookmark->created_at : "");
    if (index == state->bookmark_index) {
      (void)wattron(window,
                    state->colors ? COLOR_PAIR(BACA_PAIR_SEARCH) : A_REVERSE);
      (void)mvwhline(window, row + 1, 1, ' ', width > 2 ? width - 2 : 0);
    }
    add_clipped(window, row + 1, 2, label, width - 3);
    if (index == state->bookmark_index) {
      (void)wattroff(window,
                     state->colors ? COLOR_PAIR(BACA_PAIR_SEARCH) : A_REVERSE);
    }
  }
}

static void draw_metadata_overlay(BacaTuiState *state, WINDOW *window,
                                  int height, int width) {
  static const char *const names[] = {
      "Title",    "Author",     "Creator", "Description", "Publisher",
      "Producer", "Date",       "Created", "Modified",    "Language",
      "Format",   "Identifier", "Source",
  };
  const BacaMetadata *metadata = &state->app->document.metadata;
  const char *const values[] = {
      metadata->title,
      metadata->author,
      metadata->creator,
      metadata->description,
      metadata->publisher,
      metadata->producer,
      metadata->date,
      metadata->creation_date,
      metadata->modification_date,
      metadata->language,
      metadata->format,
      metadata->identifier,
      metadata->source,
  };
  const int visible = height > 2 ? height - 2 : 0;
  for (int row = 0; row < visible; ++row) {
    const size_t index = state->overlay_scroll + (size_t)row;
    if (index >= BACA_ARRAY_LEN(names)) {
      break;
    }
    (void)wattron(window, A_BOLD);
    add_clipped(window, row + 1, 2, names[index], 13);
    (void)wattroff(window, A_BOLD);
    add_clipped(window, row + 1, 16, values[index] != NULL ? values[index] : "",
                width - 18);
  }
}

static void draw_key_list(WINDOW *window, int row, int width, const char *name,
                          const BacaKeyList *keys) {
  (void)wattron(window, A_BOLD);
  const int key_end = width < 23 ? width - 1 : 22;
  int x = 2;
  for (size_t index = 0U; index < keys->length && x < key_end; ++index) {
    if (index > 0U) {
      add_clipped(window, row, x, ",", 1);
      ++x;
    }
    const char *key = keys->items[index];
    add_clipped(window, row, x, key, key_end - x);
    x += size_to_int(baca_utf8_width(key, strlen(key)));
  }
  (void)wattroff(window, A_BOLD);
  add_clipped(window, row, 23, name, width - 24);
}

static void draw_help_overlay(BacaTuiState *state, WINDOW *window, int height,
                              int width) {
  char *jump_back_items[] = {"ctrl+o"};
  char *jump_forward_items[] = {"ctrl+i"};
  const BacaKeyList jump_back = {
      .items = jump_back_items,
      .length = BACA_ARRAY_LEN(jump_back_items),
  };
  const BacaKeyList jump_forward = {
      .items = jump_forward_items,
      .length = BACA_ARRAY_LEN(jump_forward_items),
  };
  const BacaKeymaps *maps = &state->app->config.keymaps;
  typedef struct BacaReaderHelpLine {
    const char *description;
    const BacaKeyList *keys;
  } BacaReaderHelpLine;
  BacaReaderHelpLine lines[BACA_PDF_HELP_LINE_COUNT] = {0};
  size_t count = 0U;
#define BACA_HELP_APPEND(value, key_list)                                      \
  do {                                                                         \
    if (count < BACA_ARRAY_LEN(lines)) {                                       \
      lines[count++] =                                                         \
          (BacaReaderHelpLine){.description = (value), .keys = (key_list)};    \
    }                                                                          \
  } while (false)
#define BACA_HELP_SECTION(value) BACA_HELP_APPEND((value), NULL)
#define BACA_HELP_ENTRY(value, key_list) BACA_HELP_APPEND((value), (key_list))
  BACA_HELP_SECTION("reading");
  BACA_HELP_ENTRY("switch between light and dark", &maps->toggle_dark);
  BACA_HELP_ENTRY("scroll down", &maps->scroll_down);
  BACA_HELP_ENTRY("scroll up", &maps->scroll_up);
  BACA_HELP_ENTRY("move down one screen", &maps->page_down);
  BACA_HELP_ENTRY("move up one screen", &maps->page_up);
  BACA_HELP_ENTRY("go to the start", &maps->home);
  BACA_HELP_ENTRY("go to the end", &maps->end);
  BACA_HELP_ENTRY("open the table of contents", &maps->open_toc);
  if (state->app->document.format == BACA_FORMAT_PDF) {
    BACA_HELP_SECTION("pdf");
    BACA_HELP_ENTRY("switch between fixed and reflow view",
                    &maps->toggle_pdf_view);
  }
  BACA_HELP_SECTION("bookmarks and details");
  BACA_HELP_ENTRY("add a bookmark", &maps->add_bookmark);
  BACA_HELP_ENTRY("open bookmarks", &maps->open_bookmarks);
  BACA_HELP_ENTRY("open document details", &maps->open_metadata);
  BACA_HELP_ENTRY("save a terminal screenshot", &maps->screenshot);
  BACA_HELP_SECTION("search");
  BACA_HELP_ENTRY("search forward", &maps->search_forward);
  BACA_HELP_ENTRY("search backward", &maps->search_backward);
  BACA_HELP_ENTRY("go to the next match", &maps->next_match);
  BACA_HELP_ENTRY("go to the previous match", &maps->previous_match);
  BACA_HELP_ENTRY("clear search highlights", &maps->confirm);
  BACA_HELP_SECTION("jump history");
  BACA_HELP_ENTRY("jump back", &jump_back);
  BACA_HELP_ENTRY("jump forward", &jump_forward);
  BACA_HELP_SECTION("application");
  BACA_HELP_ENTRY("open this help", &maps->open_help);
  BACA_HELP_ENTRY("close a popup or quit", &maps->close);
#undef BACA_HELP_ENTRY
#undef BACA_HELP_SECTION
#undef BACA_HELP_APPEND
  const int visible = height > 2 ? height - 2 : 0;
  for (int row = 0; row < visible; ++row) {
    const size_t index = state->overlay_scroll + (size_t)row;
    if (index >= count) {
      break;
    }
    if (lines[index].keys == NULL) {
      (void)wattron(
          window, A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
      add_clipped(window, row + 1, 2, lines[index].description, width - 3);
      (void)wattroff(
          window, A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
    } else {
      draw_key_list(window, row + 1, width, lines[index].description,
                    lines[index].keys);
    }
  }
}

static void draw_alert_overlay(BacaTuiState *state, WINDOW *window, int height,
                               int width) {
  if (height <= 2 || width <= 4) {
    return;
  }
  (void)wattron(window, state->colors ? COLOR_PAIR(BACA_PAIR_ALERT) : A_BOLD);
  add_clipped(window, 1, 2, state->alert, width - 4);
  (void)wattroff(window, state->colors ? COLOR_PAIR(BACA_PAIR_ALERT) : A_BOLD);
}

static void draw_overlay(BacaTuiState *state) {
  if (state->overlay == BACA_OVERLAY_NONE) {
    return;
  }
  const BacaOverlayBox box_size = overlay_box(state);
  if (box_size.height <= 0 || box_size.width <= 0) {
    return;
  }
  WINDOW *window =
      newwin(box_size.height, box_size.width, box_size.y, box_size.x);
  if (window == NULL) {
    return;
  }
  (void)wbkgd(window,
              state->colors ? (chtype)COLOR_PAIR(BACA_PAIR_BASE) : A_NORMAL);
  (void)werase(window);
  if (box_size.height >= 2 && box_size.width >= 2) {
    (void)box(window, 0, 0);
    draw_overlay_title(window, box_size.width, overlay_title(state->overlay));
  }
  switch (state->overlay) {
  case BACA_OVERLAY_TOC:
    draw_toc_overlay(state, window, box_size.height, box_size.width);
    break;
  case BACA_OVERLAY_BOOKMARKS:
    draw_bookmarks_overlay(state, window, box_size.height, box_size.width);
    break;
  case BACA_OVERLAY_METADATA:
    draw_metadata_overlay(state, window, box_size.height, box_size.width);
    break;
  case BACA_OVERLAY_HELP:
    draw_help_overlay(state, window, box_size.height, box_size.width);
    break;
  case BACA_OVERLAY_ALERT:
    draw_alert_overlay(state, window, box_size.height, box_size.width);
    break;
  case BACA_OVERLAY_NONE:
    break;
  }
  (void)wnoutrefresh(window);
  (void)delwin(window);
}

static void draw_prompt(BacaTuiState *state) {
  if (!state->prompt.active) {
    return;
  }
  const int top = state->rows >= 3 ? state->rows - 3 : 0;
  for (int row = top; row < state->rows; ++row) {
    (void)mvwhline(stdscr, row, 0, ' ', state->columns);
  }
  if (state->rows >= 3) {
    (void)mvaddch(top, 0, ACS_ULCORNER);
    if (state->columns > 2) {
      (void)mvhline(top, 1, ACS_HLINE, state->columns - 2);
    }
    if (state->columns > 1) {
      (void)mvaddch(top, state->columns - 1, ACS_URCORNER);
    }
    const char *title =
        state->prompt.forward ? " Search Forward " : " Search Backward ";
    add_clipped(stdscr, top, 2, title, state->columns - 4);
  }
  const int input_row = state->rows >= 2 ? state->rows - 2 : state->rows - 1;
  const char marker[3] = {state->prompt.forward ? '/' : '?', ' ', '\0'};
  add_clipped(stdscr, input_row, 1, marker, state->columns - 1);
  add_clipped(stdscr, input_row, 3, state->prompt.text.value,
              state->columns - 4);
  int cursor_x = 3 + size_to_int(baca_utf8_width(state->prompt.text.value,
                                                 state->prompt.text.cursor));
  if (cursor_x >= state->columns) {
    cursor_x = state->columns - 1;
  }
  if (cursor_x < 0) {
    cursor_x = 0;
  }
  (void)move(input_row, cursor_x);
}

static size_t terminal_graphics_occlusions(const BacaTuiState *state,
                                           BacaGraphicsRect rects[2]) {
  size_t count = 0U;
  if (state->overlay != BACA_OVERLAY_NONE) {
    const BacaOverlayBox box_size = overlay_box(state);
    rects[count++] = (BacaGraphicsRect){
        .row = box_size.y,
        .column = box_size.x,
        .rows = box_size.height,
        .columns = box_size.width,
    };
  }
  if (state->prompt.active) {
    const int top = state->rows >= 3 ? state->rows - 3 : 0;
    rects[count++] = (BacaGraphicsRect){
        .row = top,
        .column = 0,
        .rows = state->rows - top,
        .columns = state->columns,
    };
  }
  return count;
}

static void begin_terminal_graphics_frame(BacaTuiState *state) {
  if (state->graphics == NULL || state->image_mode != BACA_IMAGE_MODE_KITTY) {
    return;
  }
  BacaError ignored = {0};
  (void)fflush(stdout);
  (void)baca_graphics_kitty_delete_placements(
      state->graphics, baca_terminal_graphics_write, state, &ignored);
}

static void draw_terminal_graphics(BacaTuiState *state) {
  const bool raw_ansi =
      state->image_mode == BACA_IMAGE_MODE_ANSI && state->raw_truecolor;
  if (state->graphics == NULL &&
      state->image_mode != BACA_IMAGE_MODE_PLACEHOLDER) {
    return;
  }
  if (!raw_ansi && state->image_mode != BACA_IMAGE_MODE_KITTY) {
    return;
  }

  BacaGraphicsRect occlusions[2] = {0};
  const size_t occlusion_count =
      terminal_graphics_occlusions(state, occlusions);
  (void)fflush(stdout);
  for (int row = 0; row < state->rows; ++row) {
    const size_t line_index = state->app->scroll_line + (size_t)row;
    if (line_index >= state->app->layout.line_count) {
      break;
    }
    const BacaLayoutLine *line = &state->app->layout.lines[line_index];
    if (line->kind != BACA_LAYOUT_IMAGE || line->image_placeholder ||
        !first_visible_image_line(state, row, line_index) ||
        line->block_index >= state->app->document.block_count ||
        state->app->document.blocks[line->block_index].value.image.broken) {
      continue;
    }

    BacaGraphicsSurface surface = {0};
    BacaGraphicsPlacement placement = {0};
    BacaError ignored = {0};
    if (!prepare_image_placement(state, line, row, occlusions, occlusion_count,
                                 &surface, &placement, &ignored)) {
      const bool newly_failed =
          mark_image_failure(state, line->block_index, line, &ignored);
      if (newly_failed && ignored.message[0] != '\0') {
        open_alert(state, ignored.message);
      }
      continue;
    }
    const bool drawn =
        raw_ansi
            ? baca_graphics_render_ansi(&surface, &placement,
                                        baca_terminal_graphics_write, state,
                                        &ignored)
            : baca_graphics_kitty_draw(state->graphics, &surface, &placement,
                                       baca_terminal_graphics_write, state,
                                       &ignored);
    baca_graphics_surface_release(&surface);
    if (!drawn) {
      if (mark_image_failure(state, line->block_index, line, &ignored) &&
          ignored.message[0] != '\0') {
        open_alert(state, ignored.message);
      }
    }
  }
}

static void draw_frame(BacaTuiState *state) {
  begin_terminal_graphics_frame(state);
  if (state->image_mode == BACA_IMAGE_MODE_ANSI && state->raw_truecolor) {
    (void)clearok(stdscr, true);
  }
  (void)werase(stdscr);
  (void)wbkgd(stdscr,
              state->colors ? (chtype)COLOR_PAIR(BACA_PAIR_BASE) : A_NORMAL);
  for (int row = 0; row < state->rows; ++row) {
    draw_document_line(state, row, state->app->scroll_line + (size_t)row);
  }
  draw_scrollbar(state);
  draw_reader_help_hint(state);
  draw_prompt(state);
  (void)wnoutrefresh(stdscr);
  draw_overlay(state);
  if (state->prompt.active) {
    if (state->previous_cursor != ERR) {
      (void)curs_set(1);
    }
  } else {
    (void)curs_set(0);
  }
  (void)doupdate();
  state->dirty = false;
  draw_terminal_graphics(state);
}

static void open_alert(BacaTuiState *state, const char *message) {
  (void)snprintf(state->alert, sizeof(state->alert), "%s",
                 message != NULL ? message : "Operation failed");
  state->overlay = BACA_OVERLAY_ALERT;
  state->overlay_scroll = 0U;
  state->dirty = true;
}

static size_t current_toc_index(const BacaTuiState *state) {
  size_t selected = 0U;
  size_t selected_line = 0U;
  bool selected_any = false;
  for (size_t index = 0U; index < state->app->document.toc_count; ++index) {
    const char *target = state->app->document.toc[index].target;
    if (target == NULL) {
      continue;
    }
    const size_t line = baca_layout_target_line(&state->app->layout, target);
    if (line != SIZE_MAX && line <= state->app->scroll_line &&
        (!selected_any || line > selected_line)) {
      selected = index;
      selected_line = line;
      selected_any = true;
    }
  }
  return selected;
}

static void open_toc(BacaTuiState *state) {
  if (state->app->document.toc_count == 0U) {
    open_alert(state, "No content navigation for this document");
    return;
  }
  state->toc_index = current_toc_index(state);
  state->overlay_scroll = state->toc_index;
  state->overlay = BACA_OVERLAY_TOC;
  state->dirty = true;
}

static void add_bookmark(BacaTuiState *state) {
  remember_progress(state);
  BacaError error = {0};
  if (!baca_database_add_bookmark(&state->app->database, state->app->source,
                                  state->app->saved_progress, &error)) {
    open_alert(state, error.message);
    return;
  }
  char message[96] = {0};
  (void)snprintf(message, sizeof(message), "Bookmark saved at %.2f%%",
                 state->app->saved_progress * 100.0);
  open_alert(state, message);
}

static void open_bookmarks(BacaTuiState *state) {
  baca_bookmarks_free(&state->bookmarks);
  BacaError error = {0};
  if (!baca_database_bookmarks(&state->app->database, state->app->source,
                               &state->bookmarks, &error)) {
    open_alert(state, error.message);
    return;
  }
  if (state->bookmarks.length == 0U) {
    open_alert(state, "No bookmarks for this document");
    return;
  }

  remember_progress(state);
  state->bookmark_index = 0U;
  double nearest = fabs(state->bookmarks.items[0].reading_progress -
                        state->app->saved_progress);
  for (size_t index = 1U; index < state->bookmarks.length; ++index) {
    const double distance =
        fabs(state->bookmarks.items[index].reading_progress -
             state->app->saved_progress);
    if (distance < nearest) {
      nearest = distance;
      state->bookmark_index = index;
    }
  }
  state->overlay_scroll = state->bookmark_index;
  state->overlay = BACA_OVERLAY_BOOKMARKS;
  state->dirty = true;
}

static size_t overlay_line_count(const BacaTuiState *state) {
  switch (state->overlay) {
  case BACA_OVERLAY_TOC:
    return state->app->document.toc_count;
  case BACA_OVERLAY_BOOKMARKS:
    return state->bookmarks.length;
  case BACA_OVERLAY_METADATA:
    return 13U;
  case BACA_OVERLAY_HELP:
    return state->app->document.format == BACA_FORMAT_PDF
               ? BACA_PDF_HELP_LINE_COUNT
               : BACA_HELP_LINE_COUNT;
  case BACA_OVERLAY_ALERT:
    return 1U;
  case BACA_OVERLAY_NONE:
    break;
  }
  return 0U;
}

static void scroll_overlay(BacaTuiState *state, int amount) {
  const BacaOverlayBox box_size = overlay_box(state);
  const size_t visible =
      box_size.height > 2 ? (size_t)(box_size.height - 2) : 1U;
  const size_t count = overlay_line_count(state);
  const size_t maximum = count > visible ? count - visible : 0U;
  if (state->overlay_scroll > maximum) {
    state->overlay_scroll = maximum;
  }
  if (amount < 0) {
    const size_t delta = (size_t)(-(long)amount);
    state->overlay_scroll =
        delta > state->overlay_scroll ? 0U : state->overlay_scroll - delta;
  } else {
    const size_t delta = (size_t)amount;
    state->overlay_scroll =
        delta > maximum - minimum_size(state->overlay_scroll, maximum)
            ? maximum
            : state->overlay_scroll + delta;
  }
  state->dirty = true;
}

static void move_toc_selection(BacaTuiState *state, int amount) {
  const size_t count = state->app->document.toc_count;
  if (count == 0U) {
    return;
  }
  if (amount < 0) {
    state->toc_index =
        state->toc_index == 0U ? count - 1U : state->toc_index - 1U;
  } else {
    state->toc_index =
        state->toc_index + 1U >= count ? 0U : state->toc_index + 1U;
  }
  state->dirty = true;
}

static void move_bookmark_selection(BacaTuiState *state, int amount) {
  const size_t count = state->bookmarks.length;
  if (count == 0U) {
    return;
  }
  if (amount < 0) {
    state->bookmark_index =
        state->bookmark_index == 0U ? count - 1U : state->bookmark_index - 1U;
  } else {
    state->bookmark_index =
        state->bookmark_index + 1U >= count ? 0U : state->bookmark_index + 1U;
  }
  state->dirty = true;
}

static void follow_bookmark_selection(BacaTuiState *state) {
  if (state->bookmark_index >= state->bookmarks.length) {
    return;
  }
  const double progress =
      state->bookmarks.items[state->bookmark_index].reading_progress;
  (void)jump_to_line(state, restore_progress_line(state, progress));
  state->overlay = BACA_OVERLAY_NONE;
  state->overlay_scroll = 0U;
  state->dirty = true;
}

static void remove_bookmark_selection(BacaTuiState *state) {
  if (state->bookmark_index >= state->bookmarks.length) {
    return;
  }
  const int64_t id = state->bookmarks.items[state->bookmark_index].id;
  BacaError error = {0};
  if (!baca_database_remove_bookmark(&state->app->database, state->app->source,
                                     id, &error)) {
    open_alert(state, error.message);
    return;
  }
  open_bookmarks(state);
}

static void follow_toc_selection(BacaTuiState *state) {
  if (state->toc_index >= state->app->document.toc_count) {
    return;
  }
  const char *target = state->app->document.toc[state->toc_index].target;
  if (!follow_link(state, target,
                   "No target for selected table of contents entry")) {
    return;
  }
  state->overlay = BACA_OVERLAY_NONE;
  state->overlay_scroll = 0U;
  state->dirty = true;
}

static void handle_prompt_key(BacaTuiState *state,
                              const BacaNormalizedKey *key) {
  BacaPromptState *prompt = &state->prompt;
  if (key->command == BACA_COMMAND_QUIT) {
    prompt->active = false;
    state->dirty = true;
    return;
  }
  if (key->command == BACA_COMMAND_CONFIRM) {
    prompt->active = false;
    submit_search(state);
    state->dirty = true;
    return;
  }
  (void)baca_text_input_apply(&prompt->text, key->key_code, key->code,
                              key->character);
  state->dirty = true;
}

static void save_screenshot(BacaTuiState *state) {
  char **lines = calloc((size_t)state->rows, sizeof(*lines));
  wchar_t *wide = calloc((size_t)state->columns + 1U, sizeof(*wide));
  if (lines == NULL || wide == NULL) {
    free(lines);
    free(wide);
    open_alert(state, "Cannot allocate screenshot buffer");
    return;
  }

  bool captured = true;
  for (int row = 0; row < state->rows; ++row) {
    memset(wide, 0, ((size_t)state->columns + 1U) * sizeof(*wide));
    WINDOW *capture = curscr != NULL ? curscr : stdscr;
    if (mvwinnwstr(capture, row, 0, wide, state->columns) == ERR) {
      captured = false;
      break;
    }
    const size_t capacity =
        ((size_t)state->columns + 1U) * (size_t)MB_CUR_MAX + 1U;
    lines[row] = calloc(capacity, 1U);
    if (lines[row] == NULL ||
        wcstombs(lines[row], wide, capacity - 1U) == (size_t)-1) {
      captured = false;
      break;
    }
    size_t length = strlen(lines[row]);
    while (length > 0U && lines[row][length - 1U] == ' ') {
      lines[row][--length] = '\0';
    }
  }
  free(wide);

  BacaError error = {0};
  if (captured) {
    struct timespec timestamp = {0};
    struct tm local_time = {0};
    (void)clock_gettime(CLOCK_REALTIME, &timestamp);
    (void)localtime_r(&timestamp.tv_sec, &local_time);
    char path[128] = {0};
    (void)snprintf(path, sizeof(path),
                   "baca_%04d%02d%02d-%02d%02d%02d-%09ld.svg",
                   local_time.tm_year + 1900, local_time.tm_mon + 1,
                   local_time.tm_mday, local_time.tm_hour, local_time.tm_min,
                   local_time.tm_sec, timestamp.tv_nsec);
    const BacaColorScheme *theme = state->app->dark_mode
                                       ? &state->app->config.dark
                                       : &state->app->config.light;
    if (baca_platform_save_svg(path, (const char *const *)lines,
                               (size_t)state->rows, theme->background,
                               theme->foreground, &error)) {
      char message[256] = {0};
      (void)snprintf(message, sizeof(message), "Saved screenshot: %s", path);
      open_alert(state, message);
    } else {
      open_alert(state, error.message);
    }
  } else {
    open_alert(state, "Cannot capture terminal contents");
  }

  for (int row = 0; row < state->rows; ++row) {
    free(lines[row]);
  }
  free(lines);
}

static bool follow_link(BacaTuiState *state, const char *link,
                        const char *missing_target_message) {
  if (link == NULL || link[0] == '\0') {
    open_alert(state, missing_target_message);
    return false;
  }
  BacaError error = {0};
  const bool pdf_page_link = state->app->document.format == BACA_FORMAT_PDF &&
                             strncmp(link, "pdf://page/", 11U) == 0;
  if (!pdf_page_link && baca_is_external_uri(link)) {
    if (!open_external(state, link, NULL, &error)) {
      open_alert(state, error.message);
      return false;
    }
    return true;
  }
  const size_t line = baca_layout_target_line(&state->app->layout, link);
  if (line == SIZE_MAX) {
    open_alert(state, missing_target_message);
    return false;
  }
  (void)jump_to_line(state, line);
  return true;
}

static void cleanup_temp_directories(BacaTuiState *state) {
  for (size_t index = 0U; index < state->temp_directory_count; ++index) {
    BacaError ignored = {0};
    (void)baca_remove_tree(state->temp_directories[index], &ignored);
    free(state->temp_directories[index]);
  }
  free(state->temp_directories);
  state->temp_directories = NULL;
  state->temp_directory_count = 0U;
  state->temp_directory_capacity = 0U;
}

static bool remember_temp_directory(BacaTuiState *state, char *directory,
                                    BacaError *error) {
  BacaError reserve_error = {0};
  char **temp_directories = baca_array_reserve(
      state->temp_directories, &state->temp_directory_capacity,
      sizeof(*state->temp_directories), state->temp_directory_count + 1U,
      &reserve_error);
  if (baca_error_is_set(&reserve_error)) {
    if (error != NULL) {
      *error = reserve_error;
    }
    return false;
  }
  state->temp_directories = temp_directories;
  state->temp_directories[state->temp_directory_count++] = directory;
  return true;
}

static char *resource_filename(const BacaImageBlock *image,
                               const BacaResource *resource, BacaError *error) {
  char *filename = image->uri != NULL && strncmp(image->uri, "data:", 5U) != 0
                       ? baca_path_basename(image->uri, error)
                       : NULL;
  if (filename != NULL) {
    char *suffix = strpbrk(filename, "?#");
    if (suffix != NULL) {
      *suffix = '\0';
    }
    for (size_t index = 0U; filename[index] != '\0'; ++index) {
      const unsigned char character = (unsigned char)filename[index];
      if (isalnum(character) == 0 && character != (unsigned char)'.' &&
          character != (unsigned char)'_' && character != (unsigned char)'-') {
        filename[index] = '_';
      }
    }
    if (strlen(filename) > 120U) {
      filename[120] = '\0';
    }
  }
  if (filename != NULL && filename[0] != '\0' && strcmp(filename, ".") != 0 &&
      strcmp(filename, "..") != 0) {
    return filename;
  }
  free(filename);

  const char *fallback = "image.bin";
  if (resource->mime_type != NULL) {
    if (strcmp(resource->mime_type, "image/png") == 0) {
      fallback = "image.png";
    } else if (strcmp(resource->mime_type, "image/jpeg") == 0) {
      fallback = "image.jpg";
    } else if (strcmp(resource->mime_type, "image/gif") == 0) {
      fallback = "image.gif";
    } else if (strcmp(resource->mime_type, "image/svg+xml") == 0) {
      fallback = "image.svg";
    } else if (strcmp(resource->mime_type, "image/webp") == 0) {
      fallback = "image.webp";
    }
  }
  return baca_strdup(fallback, error);
}

static void open_image(BacaTuiState *state, size_t block_index) {
  if (block_index >= state->app->document.block_count ||
      state->app->document.blocks[block_index].kind != BACA_BLOCK_IMAGE) {
    return;
  }
  const BacaImageBlock *image =
      &state->app->document.blocks[block_index].value.image;
  BacaError error = {0};
  const bool standalone_image =
      state->app->document.format == BACA_FORMAT_IMAGE && image->uri != NULL &&
      state->app->document.path != NULL &&
      strcmp(image->uri, state->app->document.path) == 0;
  if (image->page_index >= 0 &&
      state->app->document.format == BACA_FORMAT_PDF) {
    if (!open_external(state, state->app->document.path, NULL, &error)) {
      open_alert(state, error.message);
    }
    return;
  }
  if (standalone_image) {
    BacaResource empty_resource = {0};
    char *directory = baca_make_temp_directory("baca-image-", &error);
    char *filename = directory == NULL
                         ? NULL
                         : resource_filename(image, &empty_resource, &error);
    char *path =
        filename == NULL ? NULL : baca_path_join(directory, filename, &error);
    if (path == NULL ||
        !baca_image_export_original(&state->app->document, path, &error)) {
      open_alert(state, error.message[0] != '\0'
                            ? error.message
                            : "Cannot export the original image");
    } else if (!remember_temp_directory(state, directory, &error)) {
      open_alert(state, error.message);
    } else {
      directory = NULL;
      if (!open_external(state, path, state->app->config.preferred_image_viewer,
                         &error)) {
        open_alert(state, error.message);
      }
    }
    if (directory != NULL) {
      BacaError ignored = {0};
      (void)baca_remove_tree(directory, &ignored);
    }
    free(directory);
    free(filename);
    free(path);
    return;
  }
  BacaResource resource = {0};
  char *directory = NULL;
  char *filename = NULL;
  char *path = NULL;

  if (image->uri == NULL ||
      !baca_document_load_resource(&state->app->document, image->uri, &resource,
                                   &error)) {
    open_alert(state, error.message[0] != '\0' ? error.message
                                               : "Cannot load image resource");
    goto cleanup;
  }
  directory = baca_make_temp_directory("baca-image-", &error);
  filename = resource_filename(image, &resource, &error);
  if (directory == NULL || filename == NULL) {
    open_alert(state, error.message);
    goto cleanup;
  }
  path = baca_path_join(directory, filename, &error);
  if (path == NULL ||
      !baca_write_file(path, resource.data, resource.length, &error)) {
    open_alert(state, error.message);
    goto cleanup;
  }
  if (!remember_temp_directory(state, directory, &error)) {
    open_alert(state, error.message);
    goto cleanup;
  }
  directory = NULL;
  if (!open_external(state, path, state->app->config.preferred_image_viewer,
                     &error)) {
    open_alert(state, error.message);
  }

cleanup:
  if (directory != NULL) {
    BacaError ignored = {0};
    (void)baca_remove_tree(directory, &ignored);
  }
  free(directory);
  free(filename);
  free(path);
  baca_resource_free(&resource);
}

static const char *image_link_at_position(const BacaTuiState *state,
                                          const BacaLayoutLine *line,
                                          int absolute_x) {
  if (line->block_index >= state->app->document.block_count ||
      line->image_rows <= 0) {
    return NULL;
  }
  const BacaBlock *block = &state->app->document.blocks[line->block_index];
  if (block->kind != BACA_BLOCK_IMAGE || block->value.image.link_count == 0U ||
      state->content_width <= 0 || !image_has_visible_render(state, line)) {
    return NULL;
  }
  const double x = ((double)(absolute_x - state->content_x) + 0.5) /
                   (double)state->content_width;
  const double y = ((double)line->image_row + 0.5) / (double)line->image_rows;
  for (size_t index = 0U; index < block->value.image.link_count; ++index) {
    const BacaImageLink *link = &block->value.image.links[index];
    if (x >= link->x && x < link->x + link->width && y >= link->y &&
        y < link->y + link->height) {
      return link->target;
    }
  }
  return NULL;
}

static const char *link_at_position(BacaTuiState *state, size_t line_index,
                                    int absolute_x) {
  if (line_index >= state->app->layout.line_count) {
    return NULL;
  }
  const BacaLayoutLine *line = &state->app->layout.lines[line_index];
  if (line->kind != BACA_LAYOUT_TEXT ||
      line->block_index >= state->app->document.block_count) {
    return NULL;
  }
  const BacaBlock *block = &state->app->document.blocks[line->block_index];
  if (block->kind != BACA_BLOCK_TEXT) {
    return NULL;
  }
  BacaError ignored = {0};
  char *raw =
      baca_layout_line_text(&state->app->layout, line_index, false, &ignored);
  char *rendered =
      baca_layout_line_text(&state->app->layout, line_index, true, &ignored);
  if (raw == NULL || rendered == NULL) {
    free(raw);
    free(rendered);
    return NULL;
  }
  const int clicked = absolute_x - state->content_x;
  const char *link = NULL;
  for (size_t index = 0U; index < block->value.text.span_count; ++index) {
    const BacaTextSpan *span = &block->value.text.spans[index];
    if (span->link == NULL) {
      continue;
    }
    const size_t start =
        span->start > line->byte_start ? span->start : line->byte_start;
    const size_t end = span->end < line->byte_end ? span->end : line->byte_end;
    if (start >= end) {
      continue;
    }
    const size_t indent = line->indent > 0 ? (size_t)line->indent : 0U;
    const int start_column = rendered_column_for_raw_byte(
        raw, rendered, indent + start - line->byte_start);
    const int end_column = rendered_column_for_raw_byte(
        raw, rendered, indent + end - line->byte_start);
    if (clicked >= start_column && clicked < end_column) {
      link = span->link;
      break;
    }
  }
  free(raw);
  free(rendered);
  return link;
}

static void handle_reader_click(BacaTuiState *state, const MEVENT *event) {
  if (event->y < 0 || event->y >= state->rows) {
    return;
  }
  if (state->prompt.active) {
    const int prompt_top = state->rows >= 3 ? state->rows - 3 : 0;
    if (event->y >= prompt_top) {
      return;
    }
  }
  const size_t line_index = state->app->scroll_line + (size_t)event->y;
  if (line_index >= state->app->layout.line_count) {
    return;
  }
  const BacaLayoutLine *line = &state->app->layout.lines[line_index];
  if (line->kind == BACA_LAYOUT_IMAGE) {
    int image_x = 0;
    int image_width = 0;
    if (image_content_bounds(state, line, line_index, &image_x, &image_width) &&
        event->x >= image_x && event->x < image_x + image_width) {
      const char *link = image_link_at_position(state, line, event->x);
      if (link != NULL) {
        (void)follow_link(state, link, "No exact target for link");
      } else {
        open_image(state, line->block_index);
      }
    }
    return;
  }
  const char *link = link_at_position(state, line_index, event->x);
  if (link != NULL) {
    (void)follow_link(state, link, "No exact target for link");
  }
}

static void handle_overlay_mouse(BacaTuiState *state, const MEVENT *event) {
  if ((event->bstate & BUTTON4_PRESSED) != 0U) {
    if (state->overlay == BACA_OVERLAY_TOC) {
      move_toc_selection(state, -1);
    } else if (state->overlay == BACA_OVERLAY_BOOKMARKS) {
      move_bookmark_selection(state, -1);
    } else {
      scroll_overlay(state, -1);
    }
    return;
  }
  if ((event->bstate & BUTTON5_PRESSED) != 0U) {
    if (state->overlay == BACA_OVERLAY_TOC) {
      move_toc_selection(state, 1);
    } else if (state->overlay == BACA_OVERLAY_BOOKMARKS) {
      move_bookmark_selection(state, 1);
    } else {
      scroll_overlay(state, 1);
    }
    return;
  }
  if ((state->overlay != BACA_OVERLAY_TOC &&
       state->overlay != BACA_OVERLAY_BOOKMARKS) ||
      (event->bstate & BUTTON1_CLICKED) == 0U) {
    return;
  }
  const BacaOverlayBox box_size = overlay_box(state);
  if (event->x <= box_size.x || event->x >= box_size.x + box_size.width - 1 ||
      event->y <= box_size.y || event->y >= box_size.y + box_size.height - 1) {
    return;
  }
  const size_t index =
      state->overlay_scroll + (size_t)(event->y - box_size.y - 1);
  if (state->overlay == BACA_OVERLAY_TOC &&
      index < state->app->document.toc_count) {
    state->toc_index = index;
    follow_toc_selection(state);
  } else if (state->overlay == BACA_OVERLAY_BOOKMARKS &&
             index < state->bookmarks.length) {
    state->bookmark_index = index;
    follow_bookmark_selection(state);
  }
}

static void handle_mouse(BacaTuiState *state) {
  MEVENT event = {0};
  if (getmouse(&event) != OK) {
    return;
  }
  if (state->overlay != BACA_OVERLAY_NONE) {
    handle_overlay_mouse(state, &event);
    return;
  }
  if ((event.bstate & BUTTON4_PRESSED) != 0U) {
    scroll_lines(state, -1);
  } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
    scroll_lines(state, 1);
  } else if ((event.bstate & BUTTON1_CLICKED) != 0U) {
    handle_reader_click(state, &event);
  }
}

static void handle_overlay_key(BacaTuiState *state,
                               const BacaNormalizedKey *key) {
  if (key->command == BACA_COMMAND_SCREENSHOT) {
    save_screenshot(state);
    return;
  }
  if (key->command == BACA_COMMAND_QUIT ||
      (state->overlay == BACA_OVERLAY_TOC &&
       key->command == BACA_COMMAND_TOC) ||
      (state->overlay == BACA_OVERLAY_BOOKMARKS &&
       key->command == BACA_COMMAND_BOOKMARKS)) {
    state->overlay = BACA_OVERLAY_NONE;
    state->overlay_scroll = 0U;
    state->dirty = true;
    return;
  }
  if (state->overlay == BACA_OVERLAY_TOC) {
    switch (key->command) {
    case BACA_COMMAND_SCROLL_DOWN:
      move_toc_selection(state, 1);
      break;
    case BACA_COMMAND_SCROLL_UP:
      move_toc_selection(state, -1);
      break;
    case BACA_COMMAND_HOME:
      state->toc_index = 0U;
      state->dirty = true;
      break;
    case BACA_COMMAND_END:
      state->toc_index = state->app->document.toc_count - 1U;
      state->dirty = true;
      break;
    case BACA_COMMAND_CONFIRM:
      follow_toc_selection(state);
      break;
    default:
      break;
    }
    return;
  }
  if (state->overlay == BACA_OVERLAY_BOOKMARKS) {
    if (key->key_code && key->code == KEY_DC) {
      remove_bookmark_selection(state);
      return;
    }
    switch (key->command) {
    case BACA_COMMAND_SCROLL_DOWN:
      move_bookmark_selection(state, 1);
      break;
    case BACA_COMMAND_SCROLL_UP:
      move_bookmark_selection(state, -1);
      break;
    case BACA_COMMAND_HOME:
      state->bookmark_index = 0U;
      state->dirty = true;
      break;
    case BACA_COMMAND_END:
      state->bookmark_index = state->bookmarks.length - 1U;
      state->dirty = true;
      break;
    case BACA_COMMAND_CONFIRM:
      follow_bookmark_selection(state);
      break;
    default:
      break;
    }
    return;
  }
  switch (key->command) {
  case BACA_COMMAND_SCROLL_DOWN:
    scroll_overlay(state, 1);
    break;
  case BACA_COMMAND_SCROLL_UP:
    scroll_overlay(state, -1);
    break;
  case BACA_COMMAND_PAGE_DOWN:
    scroll_overlay(state, state->rows > 2 ? state->rows - 2 : 1);
    break;
  case BACA_COMMAND_PAGE_UP:
    scroll_overlay(state, -(state->rows > 2 ? state->rows - 2 : 1));
    break;
  case BACA_COMMAND_HOME:
    state->overlay_scroll = 0U;
    state->dirty = true;
    break;
  case BACA_COMMAND_END: {
    const size_t count = overlay_line_count(state);
    state->overlay_scroll = count > 0U ? count - 1U : 0U;
    scroll_overlay(state, 0);
    break;
  }
  default:
    break;
  }
}

static void begin_search_prompt(BacaTuiState *state, bool forward) {
  memset(&state->prompt, 0, sizeof(state->prompt));
  state->prompt.active = true;
  state->prompt.forward = forward;
  remember_progress(state);
  state->prompt.saved_progress = state->app->saved_progress;
  state->dirty = true;
}

static void toggle_pdf_view(BacaTuiState *state) {
  if (state->app->document.format != BACA_FORMAT_PDF) {
    return;
  }
  const BacaPresentation previous = state->presentation;
  state->presentation = previous == BACA_PRESENTATION_FIXED
                            ? BACA_PRESENTATION_REFLOW
                            : BACA_PRESENTATION_FIXED;
  BacaError error = {0};
  if (!rebuild_layout(state, &error) && !state->quit) {
    state->presentation = previous;
    BacaError restore_error = {0};
    (void)rebuild_layout(state, &restore_error);
    open_alert(state, error.message[0] != '\0' ? error.message
                                               : "Cannot change PDF view");
  }
}

static void reset_reader_command(BacaTuiState *state) {
  state->reader_command = (BacaReaderCommandState){0};
}

static bool collect_reader_count(BacaTuiState *state,
                                 const BacaNormalizedKey *key) {
  if (key->key_code || key->character < L'0' || key->character > L'9') {
    return false;
  }
  const size_t digit = (size_t)(key->character - L'0');
  if (!state->reader_command.count_active && digit == 0U) {
    return false;
  }
  state->reader_command.count_active = true;
  if (state->reader_command.count > (SIZE_MAX - digit) / 10U) {
    state->reader_command.count = SIZE_MAX;
  } else {
    state->reader_command.count = state->reader_command.count * 10U + digit;
  }
  return true;
}

static size_t take_reader_count(BacaTuiState *state, bool *explicit_count) {
  *explicit_count = state->reader_command.count_active;
  const size_t count = *explicit_count ? state->reader_command.count : 1U;
  state->reader_command.count = 0U;
  state->reader_command.count_active = false;
  return count;
}

static void scroll_reader_lines(BacaTuiState *state, bool down, size_t count) {
  const size_t maximum = maximum_scroll(state);
  const size_t current = minimum_size(pending_scroll_target(state), maximum);
  const size_t target =
      down ? (count > maximum - current ? maximum : current + count)
           : (count > current ? 0U : current - count);
  double duration = state->app->config.page_scroll_duration;
  if (isfinite(duration) && duration > 0.12) {
    duration = 0.12;
  }
  start_scroll_animation_with_duration(state, target, duration);
}

static void handle_reader_key(BacaTuiState *state,
                              const BacaNormalizedKey *key) {
  if (key->command == BACA_COMMAND_QUIT &&
      (state->reader_command.count_active || state->reader_command.g_pending)) {
    reset_reader_command(state);
    return;
  }
  if (collect_reader_count(state, key)) {
    return;
  }
  if (!key->key_code && key->character == L'g') {
    if (!state->reader_command.g_pending) {
      state->reader_command.g_pending = true;
      return;
    }
    bool explicit_count = false;
    const size_t count = take_reader_count(state, &explicit_count);
    state->reader_command.g_pending = false;
    (void)jump_to_line(state, explicit_count ? count - 1U : 0U);
    return;
  }
  if (state->reader_command.g_pending) {
    reset_reader_command(state);
  }
  const bool plain_tab = !key->key_code && key->character == L'\t';
  const bool ctrl_i = key->key_code && key->code == BACA_KEY_CTRL_I;
  if ((plain_tab || ctrl_i) && state->jumps.forward_count > 0U) {
    state->reader_command.count = 0U;
    state->reader_command.count_active = false;
    (void)traverse_jump_history(state, true);
    return;
  }
  if (ctrl_i) {
    return;
  }
  bool explicit_count = false;
  const size_t count = take_reader_count(state, &explicit_count);
  switch (key->command) {
  case BACA_COMMAND_QUIT:
    if (state->search.active) {
      cancel_search(state);
    } else {
      state->quit = true;
    }
    break;
  case BACA_COMMAND_SCROLL_DOWN:
    scroll_reader_lines(state, true, count);
    break;
  case BACA_COMMAND_SCROLL_UP:
    scroll_reader_lines(state, false, count);
    break;
  case BACA_COMMAND_PAGE_DOWN:
    page_scroll(state, true, count);
    break;
  case BACA_COMMAND_PAGE_UP:
    page_scroll(state, false, count);
    break;
  case BACA_COMMAND_HOME:
    (void)jump_to_line(state, 0U);
    break;
  case BACA_COMMAND_END:
    (void)jump_to_line(state,
                       explicit_count ? count - 1U : maximum_scroll(state));
    break;
  case BACA_COMMAND_TOC:
    open_toc(state);
    break;
  case BACA_COMMAND_ADD_BOOKMARK:
    add_bookmark(state);
    break;
  case BACA_COMMAND_BOOKMARKS:
    open_bookmarks(state);
    break;
  case BACA_COMMAND_METADATA:
    state->overlay = BACA_OVERLAY_METADATA;
    state->overlay_scroll = 0U;
    state->dirty = true;
    break;
  case BACA_COMMAND_HELP:
    state->overlay = BACA_OVERLAY_HELP;
    state->overlay_scroll = 0U;
    state->dirty = true;
    break;
  case BACA_COMMAND_TOGGLE_THEME:
    state->app->dark_mode = !state->app->dark_mode;
    apply_theme(state);
    break;
  case BACA_COMMAND_SEARCH_FORWARD:
    begin_search_prompt(state, true);
    break;
  case BACA_COMMAND_SEARCH_BACKWARD:
    begin_search_prompt(state, false);
    break;
  case BACA_COMMAND_NEXT_MATCH:
    for (size_t index = 0U; index < count; ++index) {
      if (!repeat_search(state, true)) {
        break;
      }
    }
    break;
  case BACA_COMMAND_PREVIOUS_MATCH:
    for (size_t index = 0U; index < count; ++index) {
      if (!repeat_search(state, false)) {
        break;
      }
    }
    break;
  case BACA_COMMAND_TOGGLE_PDF_VIEW:
    toggle_pdf_view(state);
    break;
  case BACA_COMMAND_CONFIRM:
    clear_search(&state->search);
    state->dirty = true;
    break;
  case BACA_COMMAND_SCREENSHOT:
    save_screenshot(state);
    break;
  case BACA_COMMAND_NONE:
    if (!key->key_code && key->character == 15) {
      (void)traverse_jump_history(state, false);
    }
    break;
  }
}

static bool initialize_curses(BacaTuiState *state, BacaError *error) {
  if (initscr() == NULL) {
    baca_error_set(error, BACA_ERROR_EXTERNAL, "cannot initialize terminal");
    return false;
  }
  (void)raw();
  (void)noecho();
  (void)keypad(stdscr, true);
  define_extended_keys();
  (void)set_escdelay(25);
  state->previous_cursor = curs_set(0);
  state->colors = false;
  if (has_colors() == true && start_color() == OK && COLORS >= 8 &&
      COLOR_PAIRS > BACA_PAIR_ALERT) {
    state->colors = true;
    (void)use_default_colors();
  }
  (void)mouseinterval(0);
  (void)mousemask(BUTTON1_CLICKED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
  update_dimensions(state);
  apply_theme(state);
  return true;
}

static void initialize_graphics(BacaTuiState *state) {
  const BacaGraphicsEnvironment environment = {
      .term_program = getenv("TERM_PROGRAM"),
      .term = getenv("TERM"),
      .kitty_window_id = getenv("KITTY_WINDOW_ID"),
      .colorterm = getenv("COLORTERM"),
      .tmux = getenv("TMUX"),
      .sty = getenv("STY"),
      .colors = state->colors,
      .output_is_tty = isatty(STDOUT_FILENO) != 0,
  };
  const BacaGraphicsMultiplexer multiplexer =
      baca_graphics_multiplexer(&environment);
  state->image_mode = baca_graphics_select_mode(
      state->app->config.image_mode, state->app->config.image_mode_explicit,
      &environment);
  state->raw_truecolor = baca_graphics_truecolor_available(&environment) &&
                         multiplexer == BACA_GRAPHICS_MULTIPLEXER_NONE;

  int pair_capacity = state->colors ? COLOR_PAIRS - BACA_PAIR_IMAGE_FIRST : 0;
  if (pair_capacity > 256) {
    pair_capacity = 256;
  }
  if (pair_capacity < 0) {
    pair_capacity = 0;
  }
  state->image_pair_capacity = (short)pair_capacity;

  if (state->image_mode == BACA_IMAGE_MODE_ANSI && !state->raw_truecolor &&
      state->image_pair_capacity == 0) {
    state->image_mode = BACA_IMAGE_MODE_PLACEHOLDER;
  }
  if (state->image_mode == BACA_IMAGE_MODE_PLACEHOLDER) {
    state->presentation = state->app->document.format == BACA_FORMAT_PDF
                              ? BACA_PRESENTATION_REFLOW
                              : BACA_PRESENTATION_DEFAULT;
    update_cell_pixels(state);
    baca_document_probe_images(&state->app->document, false);
    return;
  }

  BacaError ignored = {0};
  state->graphics =
      baca_graphics_create(BACA_GRAPHICS_DEFAULT_CACHE_BYTES, multiplexer,
                           current_background(state), &ignored);
  if (state->graphics == NULL) {
    state->image_mode = BACA_IMAGE_MODE_PLACEHOLDER;
    state->presentation = state->app->document.format == BACA_FORMAT_PDF
                              ? BACA_PRESENTATION_REFLOW
                              : BACA_PRESENTATION_DEFAULT;
    update_cell_pixels(state);
    baca_document_probe_images(&state->app->document, false);
    return;
  }
  if (!baca_graphics_resize(state->graphics, state->columns, state->rows,
                            &ignored)) {
    baca_graphics_free(state->graphics);
    state->graphics = NULL;
    state->image_mode = BACA_IMAGE_MODE_PLACEHOLDER;
    state->presentation = state->app->document.format == BACA_FORMAT_PDF
                              ? BACA_PRESENTATION_REFLOW
                              : BACA_PRESENTATION_DEFAULT;
    update_cell_pixels(state);
    baca_document_probe_images(&state->app->document, false);
    return;
  }
  state->presentation = state->app->document.format == BACA_FORMAT_PDF
                            ? BACA_PRESENTATION_FIXED
                            : BACA_PRESENTATION_DEFAULT;
  update_cell_pixels(state);
  baca_document_probe_images(&state->app->document, true);
}

int baca_tui_run(BacaApp *app, BacaError *error) {
  if (app == NULL || app->document.path == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "application is not initialized");
    return EXIT_FAILURE;
  }
  BacaTuiState state = {
      .app = app,
      .previous_cursor = ERR,
      .dirty = true,
  };
  if (!baca_terminal_runtime_begin(error)) {
    return EXIT_FAILURE;
  }
  if (!initialize_curses(&state, error)) {
    return baca_terminal_runtime_finish(EXIT_FAILURE, error);
  }
  initialize_graphics(&state);

  int result = EXIT_SUCCESS;
  if (app->document.format == BACA_FORMAT_PDF &&
      app->document.block_count > 0U) {
    state.pdf_render_failures =
        calloc(app->document.block_count, sizeof(*state.pdf_render_failures));
    if (state.pdf_render_failures == NULL) {
      baca_error_set(error, BACA_ERROR_MEMORY,
                     "cannot allocate PDF render failure cache");
      result = EXIT_FAILURE;
      goto cleanup;
    }
    state.pdf_render_failure_count = app->document.block_count;
  }
  if (!baca_terminal_runtime_activate(error)) {
    result = EXIT_FAILURE;
    goto cleanup;
  }
  if (!rebuild_layout(&state, error)) {
    result = EXIT_FAILURE;
    goto cleanup;
  }

  while (!state.quit) {
    if (baca_terminal_runtime_interrupted()) {
      state.quit = true;
      continue;
    }
    update_animation(&state);
    if (state.dirty) {
      draw_frame(&state);
    }

    wtimeout(stdscr, state.animation.active ? 16 : 100);
    wint_t input = 0;
    const int status = wget_wch(stdscr, &input);
    if (baca_terminal_runtime_interrupted()) {
      state.quit = true;
      continue;
    }
    const BacaNormalizedKey key =
        read_normalized_key(&app->config, status, input);
    if (!key.valid) {
      continue;
    }
    if (key.key_code && key.code == KEY_RESIZE) {
      reset_reader_command(&state);
      update_dimensions(&state);
      BacaError resize_error = {0};
      if (!rebuild_layout(&state, &resize_error) && !state.quit) {
        open_alert(&state, resize_error.message);
      }
      continue;
    }
    if (key.key_code && key.code == KEY_MOUSE) {
      reset_reader_command(&state);
      handle_mouse(&state);
      continue;
    }
    if (state.prompt.active) {
      reset_reader_command(&state);
      handle_prompt_key(&state, &key);
    } else if (state.overlay != BACA_OVERLAY_NONE) {
      reset_reader_command(&state);
      handle_overlay_key(&state, &key);
    } else {
      handle_reader_key(&state, &key);
    }
  }

cleanup:
  remember_progress(&state);
  clear_search(&state.search);
  baca_bookmarks_free(&state.bookmarks);
  free(state.pdf_render_failures);
  cleanup_temp_directories(&state);
  delete_kitty_images(&state);
  baca_graphics_free(state.graphics);
  state.graphics = NULL;
  if (state.previous_cursor != ERR) {
    (void)curs_set(state.previous_cursor);
  }
  (void)endwin();
  return baca_terminal_runtime_finish(result, error);
}
