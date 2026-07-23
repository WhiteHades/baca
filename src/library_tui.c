#include "mereader-tui/library_tui.h"
#include "mereader-tui/graphics.h"
#include "mereader-tui/keymap.h"
#include "mereader-tui/library_shelf.h"
#include "terminal_runtime.h"
#include "text_input.h"

#include <curses.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

enum {
  MEREADER_TUI_PAIR_BASE = 1,
  MEREADER_TUI_PAIR_ACCENT,
  MEREADER_TUI_PAIR_SEARCH,
  MEREADER_TUI_PAIR_ALERT,
  MEREADER_TUI_PAIR_IMAGE_FIRST,
};

typedef enum MereaderTuiLibraryInputKind {
  MEREADER_TUI_LIBRARY_INPUT_NONE = 0,
  MEREADER_TUI_LIBRARY_INPUT_FILTER,
  MEREADER_TUI_LIBRARY_INPUT_PATH,
  MEREADER_TUI_LIBRARY_INPUT_ROOT,
} MereaderTuiLibraryInputKind;

typedef enum MereaderTuiLibraryKeyAction {
  MEREADER_TUI_LIBRARY_KEY_NONE = 0,
  MEREADER_TUI_LIBRARY_KEY_MOVE_DOWN,
  MEREADER_TUI_LIBRARY_KEY_MOVE_UP,
  MEREADER_TUI_LIBRARY_KEY_PAGE_DOWN,
  MEREADER_TUI_LIBRARY_KEY_PAGE_UP,
  MEREADER_TUI_LIBRARY_KEY_FIRST,
  MEREADER_TUI_LIBRARY_KEY_LAST,
  MEREADER_TUI_LIBRARY_KEY_OPEN,
  MEREADER_TUI_LIBRARY_KEY_BACK,
  MEREADER_TUI_LIBRARY_KEY_TOGGLE_VIEW,
  MEREADER_TUI_LIBRARY_KEY_TOGGLE_SHELF,
  MEREADER_TUI_LIBRARY_KEY_CHOOSE_FORMAT,
  MEREADER_TUI_LIBRARY_KEY_SORT,
  MEREADER_TUI_LIBRARY_KEY_REFRESH,
  MEREADER_TUI_LIBRARY_KEY_OPEN_PATH,
  MEREADER_TUI_LIBRARY_KEY_FILTER,
  MEREADER_TUI_LIBRARY_KEY_FIND,
  MEREADER_TUI_LIBRARY_KEY_HELP,
  MEREADER_TUI_LIBRARY_KEY_CONFIRM,
  MEREADER_TUI_LIBRARY_KEY_CLOSE,
  MEREADER_TUI_LIBRARY_KEY_QUIT,
} MereaderTuiLibraryKeyAction;

typedef struct MereaderTuiLibraryTuiState {
  const MereaderTuiConfig *config;
  const MereaderTuiHistory *history;
  MereaderTuiCatalog *catalog;
  MereaderTuiGraphicsContext *graphics;
  MereaderTuiDocument cover_document;
  MereaderTuiLibraryShelf shelf;
  MereaderTuiLibraryShelfKind mode;
  MereaderTuiLibraryShelfKind format_parent;
  char *author;
  size_t format_book;
  MereaderTuiLibraryView view;
  MereaderTuiLibraryShelf picker_shelf;
  MereaderTuiLibraryView picker_view;
  MereaderTuiLibraryAction action;
  MereaderTuiTextInput input;
  MereaderTuiTextInput picker_input;
  MereaderTuiLibraryInputKind input_kind;
  MereaderTuiLibrarySort sort;
  size_t selected;
  size_t top;
  size_t picker_selected;
  size_t picker_top;
  size_t help_scroll;
  size_t cover_book_index;
  int rows;
  int columns;
  int cell_pixel_width;
  int cell_pixel_height;
  int cover_y;
  int cover_x;
  int cover_rows;
  int cover_columns;
  int previous_cursor;
  char filter[MEREADER_TUI_TEXT_INPUT_CAPACITY];
  char filter_before[MEREADER_TUI_TEXT_INPUT_CAPACITY];
  char *status;
  char *input_selection;
  MereaderTuiImageMode image_mode;
  short image_pair_capacity;
  bool colors;
  bool raw_truecolor;
  bool cover_pending;
  bool detail_view;
  bool help_open;
  bool picker_open;
  bool setup_required;
  bool sequence_pending;
  MereaderTuiKey sequence_key;
  bool quit;
  bool dirty;
} MereaderTuiLibraryTuiState;

static void library_draw_box(int y, int x, int height, int width);
static void library_ensure_picker_selection_visible(MereaderTuiLibraryTuiState *state);

static int size_to_int(size_t value) {
  return value > (size_t)INT_MAX ? INT_MAX : (int)value;
}

typedef struct MereaderTuiLibraryKeyEntry {
  MereaderTuiLibraryKeyAction action;
  const MereaderTuiKeyList *keys;
} MereaderTuiLibraryKeyEntry;

static MereaderTuiKey library_key(bool key_code, int code, wchar_t character) {
  return (MereaderTuiKey){
      .key_code = key_code,
      .code = code,
      .character = character,
  };
}

static MereaderTuiLibraryKeyAction library_match_key_entries(
    MereaderTuiLibraryTuiState *state,
    const MereaderTuiLibraryKeyEntry *entries, size_t count,
    const MereaderTuiKey *key) {
  if (state->sequence_pending) {
    state->sequence_pending = false;
    for (size_t index = 0U; index < count; ++index) {
      if (mereader_tui_key_list_matches_sequence(
              entries[index].keys, &state->sequence_key, key)) {
        return entries[index].action;
      }
    }
  }
  for (size_t index = 0U; index < count; ++index) {
    if (mereader_tui_key_list_matches(entries[index].keys, key)) {
      return entries[index].action;
    }
  }
  for (size_t index = 0U; index < count; ++index) {
    if (mereader_tui_key_list_starts_sequence(entries[index].keys, key)) {
      state->sequence_key = *key;
      state->sequence_pending = true;
      break;
    }
  }
  return MEREADER_TUI_LIBRARY_KEY_NONE;
}

static MereaderTuiLibraryKeyAction
library_key_action(MereaderTuiLibraryTuiState *state,
                   const MereaderTuiKey *key) {
  const MereaderTuiLibraryKeymaps *maps = &state->config->library_keymaps;
  const MereaderTuiLibraryKeyEntry entries[] = {
      {MEREADER_TUI_LIBRARY_KEY_MOVE_DOWN, &maps->move_down},
      {MEREADER_TUI_LIBRARY_KEY_MOVE_UP, &maps->move_up},
      {MEREADER_TUI_LIBRARY_KEY_PAGE_DOWN, &maps->page_down},
      {MEREADER_TUI_LIBRARY_KEY_PAGE_UP, &maps->page_up},
      {MEREADER_TUI_LIBRARY_KEY_FIRST, &maps->first},
      {MEREADER_TUI_LIBRARY_KEY_LAST, &maps->last},
      {MEREADER_TUI_LIBRARY_KEY_OPEN, &maps->open},
      {MEREADER_TUI_LIBRARY_KEY_BACK, &maps->back},
      {MEREADER_TUI_LIBRARY_KEY_TOGGLE_VIEW, &maps->toggle_view},
      {MEREADER_TUI_LIBRARY_KEY_TOGGLE_SHELF, &maps->toggle_shelf},
      {MEREADER_TUI_LIBRARY_KEY_CHOOSE_FORMAT, &maps->choose_format},
      {MEREADER_TUI_LIBRARY_KEY_SORT, &maps->sort},
      {MEREADER_TUI_LIBRARY_KEY_REFRESH, &maps->refresh},
      {MEREADER_TUI_LIBRARY_KEY_OPEN_PATH, &maps->open_path},
      {MEREADER_TUI_LIBRARY_KEY_FILTER, &maps->filter},
      {MEREADER_TUI_LIBRARY_KEY_FIND, &maps->find},
      {MEREADER_TUI_LIBRARY_KEY_HELP, &maps->help},
      {MEREADER_TUI_LIBRARY_KEY_CLOSE, &maps->close},
      {MEREADER_TUI_LIBRARY_KEY_QUIT, &maps->quit},
  };
  return library_match_key_entries(state, entries,
                                   MEREADER_TUI_ARRAY_LEN(entries), key);
}

static MereaderTuiLibraryKeyAction
library_modal_key_action(MereaderTuiLibraryTuiState *state,
                         const MereaderTuiKey *key) {
  const MereaderTuiLibraryKeymaps *maps = &state->config->library_keymaps;
  const MereaderTuiLibraryKeyEntry entries[] = {
      {MEREADER_TUI_LIBRARY_KEY_CONFIRM, &maps->confirm},
      {MEREADER_TUI_LIBRARY_KEY_CLOSE, &maps->close},
  };
  return library_match_key_entries(state, entries,
                                   MEREADER_TUI_ARRAY_LEN(entries), key);
}

static void library_add_clipped(WINDOW *window, int y, int x, const char *text,
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
  MereaderTuiError ignored = {0};
  char *safe = mereader_tui_library_sanitize_text(text, (size_t)columns, &ignored);
  if (safe != NULL) {
    (void)mvwaddnstr(window, y, x, safe, size_to_int(strlen(safe)));
  }
  free(safe);
}

static short terminal_color(uint32_t rgb) {
  return (short)mereader_tui_graphics_rgb_to_palette(rgb, (unsigned)COLORS);
}

static size_t library_visible_rows(const MereaderTuiLibraryTuiState *state) {
  return state->rows > 3 ? (size_t)(state->rows - 3) : 1U;
}

static void library_ensure_selection_visible(MereaderTuiLibraryTuiState *state) {
  if (state->view.length == 0U) {
    state->selected = 0U;
    state->top = 0U;
    return;
  }
  if (state->selected >= state->view.length) {
    state->selected = state->view.length - 1U;
  }
  const size_t visible = library_visible_rows(state);
  if (state->selected < state->top) {
    state->top = state->selected;
  } else if (state->selected >= state->top + visible) {
    state->top = state->selected - visible + 1U;
  }
  const size_t maximum_top =
      state->view.length > visible ? state->view.length - visible : 0U;
  if (state->top > maximum_top) {
    state->top = maximum_top;
  }
}

static void library_update_cell_pixels(MereaderTuiLibraryTuiState *state) {
  int width = 0;
  int height = 0;
  mereader_tui_terminal_probe_cell_pixels(state->image_mode == MEREADER_TUI_IMAGE_MODE_KITTY,
                                  &width, &height);
  state->cell_pixel_width = width;
  state->cell_pixel_height = height;
  if (state->graphics != NULL) {
    MereaderTuiError ignored = {0};
    (void)mereader_tui_graphics_set_cell_pixels(state->graphics, width, height,
                                        &ignored);
  }
}

static void library_update_dimensions(MereaderTuiLibraryTuiState *state) {
  const int previous_rows = state->rows;
  const int previous_columns = state->columns;
  getmaxyx(stdscr, state->rows, state->columns);
  if (state->rows < 1) {
    state->rows = 1;
  }
  if (state->columns < 1) {
    state->columns = 1;
  }
  if (state->graphics != NULL &&
      (state->rows != previous_rows || state->columns != previous_columns)) {
    MereaderTuiError ignored = {0};
    if (state->image_mode == MEREADER_TUI_IMAGE_MODE_KITTY) {
      (void)mereader_tui_graphics_kitty_delete_placements(
          state->graphics, mereader_tui_terminal_graphics_write, state, &ignored);
    }
    (void)mereader_tui_graphics_resize(state->graphics, state->columns, state->rows,
                               &ignored);
  }
  library_update_cell_pixels(state);
  library_ensure_selection_visible(state);
  library_ensure_picker_selection_visible(state);
  state->dirty = true;
}

static void library_clear_status(MereaderTuiLibraryTuiState *state) {
  free(state->status);
  state->status = NULL;
}

static void library_copy_status(MereaderTuiLibraryTuiState *state,
                                const char *message) {
  MereaderTuiError ignored = {0};
  char *safe = mereader_tui_library_sanitize_text(message, SIZE_MAX, &ignored);
  if (safe != NULL) {
    library_clear_status(state);
    state->status = safe;
  }
}

static bool library_replace_view(MereaderTuiLibraryTuiState *state,
                                 MereaderTuiLibraryShelfKind mode, const char *author,
                                 size_t format_book, const char *filter,
                                 MereaderTuiLibrarySort sort,
                                 const char *selected_filepath,
                                 MereaderTuiError *error) {
  MereaderTuiLibraryShelf replacement_shelf = {0};
  const MereaderTuiHistory *history = state->history;
  if (state->catalog != NULL) {
    if (!mereader_tui_library_shelf_build(&replacement_shelf, state->catalog, mode,
                                  author, format_book, filter, error)) {
      return false;
    }
    history = &replacement_shelf.entries;
  }
  const char *view_filter = state->catalog == NULL ? filter : "";
  const MereaderTuiLibrarySort view_sort = state->catalog != NULL && filter[0] != '\0'
                                        ? MEREADER_TUI_LIBRARY_SORT_RELEVANCE
                                        : sort;
  MereaderTuiLibraryView replacement_view = {0};
  if (!mereader_tui_library_view_build(&replacement_view, history, view_filter,
                               view_sort, error)) {
    mereader_tui_library_shelf_free(&replacement_shelf);
    return false;
  }
  const size_t selected = mereader_tui_library_preserve_selection(
      &replacement_view, selected_filepath, state->selected);
  mereader_tui_library_view_free(&state->view);
  mereader_tui_library_shelf_free(&state->shelf);
  state->view = replacement_view;
  state->shelf = replacement_shelf;
  state->selected = selected;
  if (filter != state->filter) {
    (void)snprintf(state->filter, sizeof(state->filter), "%s", filter);
  }
  state->sort = sort;
  library_ensure_selection_visible(state);
  state->dirty = true;
  return true;
}

static bool library_rebuild_view(MereaderTuiLibraryTuiState *state, const char *filter,
                                 MereaderTuiLibrarySort sort,
                                 const char *selected_filepath,
                                 MereaderTuiError *error) {
  return library_replace_view(state, state->mode, state->author,
                              state->format_book, filter, sort,
                              selected_filepath, error);
}

static bool library_rebuild_picker(MereaderTuiLibraryTuiState *state,
                                   const char *selected_filepath,
                                   MereaderTuiError *error) {
  MereaderTuiLibraryShelf replacement_shelf = {0};
  const MereaderTuiHistory *history = state->history;
  if (state->catalog != NULL) {
    if (!mereader_tui_library_shelf_build(&replacement_shelf, state->catalog,
                                  MEREADER_TUI_LIBRARY_SHELF_ALL, NULL, 0U,
                                  state->picker_input.value, error)) {
      return false;
    }
    history = &replacement_shelf.entries;
  }
  const char *filter = state->catalog == NULL ? state->picker_input.value : "";
  const MereaderTuiLibrarySort sort = state->picker_input.length == 0U
                                   ? state->sort
                                   : MEREADER_TUI_LIBRARY_SORT_RELEVANCE;
  MereaderTuiLibraryView replacement_view = {0};
  if (!mereader_tui_library_view_build(&replacement_view, history, filter, sort,
                               error)) {
    mereader_tui_library_shelf_free(&replacement_shelf);
    return false;
  }
  const size_t selected = mereader_tui_library_preserve_selection(
      &replacement_view, selected_filepath, state->picker_selected);
  mereader_tui_library_view_free(&state->picker_view);
  mereader_tui_library_shelf_free(&state->picker_shelf);
  state->picker_view = replacement_view;
  state->picker_shelf = replacement_shelf;
  state->picker_selected = selected;
  state->picker_top = selected;
  return true;
}

static const char *library_selected_filepath(const MereaderTuiLibraryTuiState *state) {
  if (state->selected >= state->view.length) {
    return NULL;
  }
  return state->view.rows[state->selected].entry->filepath;
}

static const MereaderTuiLibraryShelfReference *
library_selected_reference(const MereaderTuiLibraryTuiState *state) {
  if (state->catalog == NULL || state->selected >= state->view.length) {
    return NULL;
  }
  return mereader_tui_library_shelf_reference(&state->shelf,
                                      state->view.rows[state->selected].entry);
}

static const char *
library_picker_selected_filepath(const MereaderTuiLibraryTuiState *state) {
  if (state->picker_selected >= state->picker_view.length) {
    return NULL;
  }
  return state->picker_view.rows[state->picker_selected].entry->filepath;
}

static const MereaderTuiLibraryShelfReference *
library_picker_selected_reference(const MereaderTuiLibraryTuiState *state) {
  if (state->catalog == NULL ||
      state->picker_selected >= state->picker_view.length) {
    return NULL;
  }
  return mereader_tui_library_shelf_reference(
      &state->picker_shelf,
      state->picker_view.rows[state->picker_selected].entry);
}

static const MereaderTuiCatalogBook *
library_book_for_reference(const MereaderTuiLibraryTuiState *state,
                           const MereaderTuiLibraryShelfReference *reference) {
  if (state->catalog == NULL || reference == NULL ||
      reference->book_index >= state->catalog->length) {
    return NULL;
  }
  return &state->catalog->books[reference->book_index];
}

static void library_runtime_error(MereaderTuiLibraryTuiState *state,
                                  const MereaderTuiError *error,
                                  const char *fallback) {
  library_copy_status(state, error != NULL && error->message[0] != '\0'
                                 ? error->message
                                 : fallback);
  state->dirty = true;
}

static void library_move_selection(MereaderTuiLibraryTuiState *state, int amount) {
  if (state->view.length == 0U) {
    return;
  }
  if (amount < 0) {
    const size_t distance = (size_t)(-(long)amount);
    state->selected =
        distance > state->selected ? 0U : state->selected - distance;
  } else {
    const size_t distance = (size_t)amount;
    const size_t maximum = state->view.length - 1U;
    state->selected = distance > maximum - state->selected
                          ? maximum
                          : state->selected + distance;
  }
  library_clear_status(state);
  library_ensure_selection_visible(state);
  state->dirty = true;
}

static int library_progress_percent(double progress) {
  if (!isfinite(progress) || progress < 0.0) {
    progress = 0.0;
  } else if (progress > 1.0) {
    progress = 1.0;
  }
  return (int)lround(progress * 100.0);
}

static void library_format_recency(const char *last_read, char output[16]) {
  struct timespec parsed = {0};
  if (!mereader_tui_library_parse_timestamp(last_read, &parsed)) {
    (void)snprintf(output, 16U, "-");
    return;
  }
  const time_t now = time(NULL);
  if (now == (time_t)-1) {
    (void)snprintf(output, 16U, "-");
    return;
  }
  double elapsed = difftime(now, parsed.tv_sec);
  if (elapsed < 0.0) {
    elapsed = 0.0;
  }
  if (elapsed < 3600.0) {
    (void)snprintf(output, 16U, "now");
  } else if (elapsed < 86400.0) {
    (void)snprintf(output, 16U, "%.0fh", floor(elapsed / 3600.0));
  } else if (elapsed < 2592000.0) {
    (void)snprintf(output, 16U, "%.0fd", floor(elapsed / 86400.0));
  } else if (elapsed < 31536000.0) {
    (void)snprintf(output, 16U, "%.0fmo", floor(elapsed / 2592000.0));
  } else {
    (void)snprintf(output, 16U, "%.0fy", floor(elapsed / 31536000.0));
  }
}

static void library_draw_row(MereaderTuiLibraryTuiState *state, int screen_row,
                             size_t view_index) {
  if (view_index >= state->view.length || state->columns <= 0) {
    return;
  }
  const MereaderTuiLibraryRow *row = &state->view.rows[view_index];
  const int x = state->columns > 2 ? 1 : 0;
  const int width = state->columns > 2 ? state->columns - 2 : state->columns;
  if (width <= 0) {
    return;
  }

  const bool selected = view_index == state->selected;
  const attr_t selected_attributes =
      selected ? (state->colors ? A_NORMAL : A_REVERSE) : A_NORMAL;
  const short selected_pair =
      selected && state->colors ? MEREADER_TUI_PAIR_SEARCH : MEREADER_TUI_PAIR_BASE;
  (void)wattron(stdscr, selected_attributes |
                            (state->colors ? COLOR_PAIR(selected_pair) : 0));
  (void)mvwhline(stdscr, screen_row, x, ' ', width);

  char progress[8] = {0};
  char recency[16] = {0};
  (void)snprintf(progress, sizeof(progress), "%d%%",
                 library_progress_percent(row->entry->reading_progress));
  library_format_recency(row->entry->last_read, recency);
  const int progress_width = size_to_int(strlen(progress));
  const int recency_width = size_to_int(strlen(recency));

  if (width < progress_width + recency_width + 6) {
    library_add_clipped(stdscr, screen_row, x, row->title, width);
  } else {
    const int recency_x = x + width - recency_width;
    const int progress_x = recency_x - progress_width - 1;
    const int text_width = progress_x - x - 1;
    int author_width = text_width >= 6 ? text_width / 3 : 0;
    if (author_width > 20) {
      author_width = 20;
    }
    const int title_width =
        text_width - author_width - (author_width > 0 ? 1 : 0);
    library_add_clipped(stdscr, screen_row, x, row->title, title_width);
    if (author_width > 0) {
      library_add_clipped(stdscr, screen_row, x + title_width + 1, row->author,
                          author_width);
    }
    library_add_clipped(stdscr, screen_row, progress_x, progress,
                        progress_width);
    library_add_clipped(stdscr, screen_row, recency_x, recency, recency_width);
  }
  (void)wattroff(stdscr, selected_attributes |
                             (state->colors ? COLOR_PAIR(selected_pair) : 0));
}

static void library_draw_progress(MereaderTuiLibraryTuiState *state, int y, int x,
                                  int width, double progress) {
  if (width <= 0 || y < 0 || y >= state->rows) {
    return;
  }
  const int percent = library_progress_percent(progress);
  char label[16] = {0};
  (void)snprintf(label, sizeof(label), "%d%%", percent);
  const int label_width = size_to_int(strlen(label));
  const int bar_width = width - label_width - 1;
  if (bar_width < 4) {
    library_add_clipped(stdscr, y, x, label, width);
    return;
  }
  const int filled = (bar_width * percent + 50) / 100;
  (void)mvwaddch(stdscr, y, x, '[');
  for (int column = 1; column < bar_width - 1; ++column) {
    const chtype cell = column <= filled ? '=' : ' ';
    (void)mvwaddch(stdscr, y, x + column, cell);
  }
  (void)mvwaddch(stdscr, y, x + bar_width - 1, ']');
  library_add_clipped(stdscr, y, x + bar_width + 1, label, label_width);
}

static void library_clear_cover(MereaderTuiLibraryTuiState *state) {
  mereader_tui_document_close(&state->cover_document);
}

static bool library_load_cover(MereaderTuiLibraryTuiState *state, size_t book_index) {
  if (state->catalog == NULL || book_index >= state->catalog->length) {
    library_clear_cover(state);
    state->cover_book_index = SIZE_MAX;
    return false;
  }
  if (state->cover_book_index == book_index) {
    return state->cover_document.path != NULL;
  }
  library_clear_cover(state);
  state->cover_book_index = book_index;

  static const char *const names[] = {
      "cover.jpg",
      "cover.jpeg",
      "cover.png",
      "cover.webp",
  };
  const MereaderTuiCatalogBook *book = &state->catalog->books[book_index];
  const MereaderTuiCatalogFormat *preferred = mereader_tui_catalog_preferred_format(book);
  MereaderTuiError ignored = {0};
  char *directory =
      preferred == NULL ? NULL : mereader_tui_path_dirname(preferred->path, &ignored);
  if (directory == NULL) {
    return false;
  }
  for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(names); ++index) {
    char *path = mereader_tui_path_join(directory, names[index], &ignored);
    if (path == NULL) {
      continue;
    }
    struct stat status = {0};
    const bool regular = lstat(path, &status) == 0 && S_ISREG(status.st_mode);
    if (regular && mereader_tui_document_open(&state->cover_document, path, &ignored) &&
        state->cover_document.block_count > 0U &&
        state->cover_document.blocks[0].kind == MEREADER_TUI_BLOCK_IMAGE) {
      mereader_tui_document_probe_images(&state->cover_document, true);
      if (state->cover_document.blocks[0].value.image.broken) {
        mereader_tui_document_close(&state->cover_document);
        free(path);
        continue;
      }
      free(path);
      free(directory);
      return true;
    }
    mereader_tui_document_close(&state->cover_document);
    free(path);
  }
  free(directory);
  return false;
}

static bool library_draw_cover_cell(void *user_data, int row, int column,
                                    uint32_t foreground, uint32_t background) {
  MereaderTuiLibraryTuiState *state = user_data;
  const unsigned foreground_index =
      mereader_tui_graphics_rgb_to_palette(foreground, (unsigned)COLORS);
  const unsigned background_index =
      mereader_tui_graphics_rgb_to_palette(background, (unsigned)COLORS);
  bool created = false;
  const short pair = mereader_tui_graphics_pair(state->graphics, foreground_index,
                                        background_index, MEREADER_TUI_PAIR_IMAGE_FIRST,
                                        state->image_pair_capacity, &created);
  if (pair <= 0 || (created && init_pair(pair, (short)foreground_index,
                                         (short)background_index) == ERR)) {
    return false;
  }
  cchar_t character = {0};
  return setcchar(&character, L"\u2580", A_NORMAL, pair, NULL) != ERR &&
         mvwadd_wch(stdscr, row, column, &character) != ERR;
}

static bool library_prepare_cover(MereaderTuiLibraryTuiState *state, size_t book_index,
                                  int y, int x, int rows, int columns,
                                  MereaderTuiGraphicsSurface *surface) {
  if (state->graphics == NULL || rows <= 0 || columns <= 0 ||
      !library_load_cover(state, book_index)) {
    return false;
  }
  MereaderTuiError ignored = {0};
  if (!mereader_tui_graphics_prepare(state->graphics, &state->cover_document, 0U,
                             columns, rows, surface, &ignored)) {
    return false;
  }
  state->cover_y = y;
  state->cover_x = x;
  state->cover_rows = rows;
  state->cover_columns = columns;
  return true;
}

static bool library_draw_cover(MereaderTuiLibraryTuiState *state, size_t book_index,
                               int y, int x, int rows, int columns) {
  MereaderTuiGraphicsSurface surface = {0};
  if (!library_prepare_cover(state, book_index, y, x, rows, columns,
                             &surface)) {
    return false;
  }
  if ((state->image_mode == MEREADER_TUI_IMAGE_MODE_ANSI && state->raw_truecolor) ||
      state->image_mode == MEREADER_TUI_IMAGE_MODE_KITTY) {
    state->cover_pending = true;
    mereader_tui_graphics_surface_release(&surface);
    return true;
  }
  const MereaderTuiGraphicsPlacement placement = {
      .row = y,
      .column = x,
      .viewport_rows = state->rows,
      .viewport_columns = state->columns,
  };
  MereaderTuiError ignored = {0};
  const bool drawn = mereader_tui_graphics_render_cells(
      &surface, &placement, library_draw_cover_cell, state, &ignored);
  mereader_tui_graphics_surface_release(&surface);
  return drawn;
}

static void library_draw_cover_placeholder(int y, int x, int rows,
                                           int columns) {
  if (rows < 2 || columns < 4) {
    return;
  }
  library_draw_box(y, x, rows, columns);
  const char *label = "no cover";
  const int label_width = size_to_int(strlen(label));
  if (rows >= 3 && label_width < columns - 2) {
    (void)wattron(stdscr, A_DIM);
    library_add_clipped(stdscr, y + rows / 2, x + (columns - label_width) / 2,
                        label, label_width);
    (void)wattroff(stdscr, A_DIM);
  }
}

static void library_draw_details(MereaderTuiLibraryTuiState *state,
                                 const MereaderTuiLibraryRow *row,
                                 const MereaderTuiLibraryShelfReference *reference,
                                 int y, int x, int height, int width) {
  if (row == NULL || height <= 0 || width <= 0) {
    return;
  }
  const MereaderTuiCatalogBook *book = library_book_for_reference(state, reference);
  const MereaderTuiCatalogFormat *preferred =
      book == NULL ? NULL : mereader_tui_catalog_preferred_format(book);
  const char *title = book == NULL ? row->title : book->title;
  const char *author = book == NULL ? row->author : book->author;
  const double progress = preferred == NULL ? row->entry->reading_progress
                                            : preferred->reading_progress;

  int detail_x = x;
  int detail_width = width;
  if (book != NULL && reference != NULL && height >= 7 && width >= 26) {
    int cover_width = width / 3;
    if (cover_width > 18) {
      cover_width = 18;
    }
    int cover_height = height;
    if (cover_height > 12) {
      cover_height = 12;
    }
    if (!library_draw_cover(state, reference->book_index, y, x, cover_height,
                            cover_width)) {
      library_draw_cover_placeholder(y, x, cover_height, cover_width);
    }
    detail_x += cover_width + 2;
    detail_width -= cover_width + 2;
  }

  int line = y;
  (void)wattron(stdscr,
                A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
  library_add_clipped(stdscr, line++, detail_x, title, detail_width);
  (void)wattroff(stdscr,
                 A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
  if (line < y + height) {
    (void)wattron(stdscr, A_DIM);
    library_add_clipped(stdscr, line++, detail_x,
                        author == NULL || author[0] == '\0' ? "unknown author"
                                                            : author,
                        detail_width);
    (void)wattroff(stdscr, A_DIM);
  }
  if (line < y + height) {
    ++line;
  }
  if (line < y + height) {
    library_draw_progress(state, line++, detail_x, detail_width, progress);
  }

  if (book != NULL && line < y + height) {
    MereaderTuiString formats = {0};
    MereaderTuiError ignored = {0};
    for (size_t index = 0U; index < book->format_count; ++index) {
      if (index > 0U && !mereader_tui_string_append(&formats, "  ", &ignored)) {
        break;
      }
      if (!mereader_tui_string_append(&formats, book->formats[index].name, &ignored)) {
        break;
      }
    }
    if (formats.data != NULL) {
      char value[512] = {0};
      (void)snprintf(value, sizeof(value), "formats  %s", formats.data);
      library_add_clipped(stdscr, line++, detail_x, value, detail_width);
    }
    mereader_tui_string_free(&formats);
  }
  if (preferred != NULL && line < y + height) {
    char value[512] = {0};
    (void)snprintf(value, sizeof(value), "reading  %s", preferred->name);
    library_add_clipped(stdscr, line++, detail_x, value, detail_width);
  }
  if (line < y + height) {
    ++line;
  }
  if (line < y + height) {
    (void)wattron(stdscr, A_DIM);
    library_add_clipped(stdscr, line++, detail_x, row->entry->filepath,
                        detail_width);
    (void)wattroff(stdscr, A_DIM);
  }
}

static size_t library_sanitized_width(const char *text, size_t length) {
  if (length >= MEREADER_TUI_TEXT_INPUT_CAPACITY) {
    return 0U;
  }
  char segment[MEREADER_TUI_TEXT_INPUT_CAPACITY] = {0};
  memcpy(segment, text, length);
  MereaderTuiError ignored = {0};
  char *safe = mereader_tui_library_sanitize_text(segment, SIZE_MAX, &ignored);
  const size_t width = safe == NULL ? 0U : mereader_tui_utf8_width(safe, strlen(safe));
  free(safe);
  return width;
}

static size_t library_input_visible_start(const MereaderTuiTextInput *input,
                                          int columns) {
  if (columns <= 0) {
    return input->cursor;
  }
  size_t start = 0U;
  while (start < input->cursor &&
         library_sanitized_width(input->value + start, input->cursor - start) >
             (size_t)columns) {
    int width = 0;
    const size_t next =
        mereader_tui_utf8_next(input->value, input->cursor, start, &width);
    start = next > start ? next : start + 1U;
  }
  return start;
}

static void library_key_list_label(const MereaderTuiKeyList *keys, char *output,
                                   size_t capacity) {
  if (capacity == 0U) {
    return;
  }
  output[0] = '\0';
  if (keys == NULL) {
    return;
  }
  for (size_t index = 0U; index < keys->length; ++index) {
    const char *name = mereader_tui_key_display_name(keys->items[index]);
    const size_t used = strlen(output);
    if (used >= capacity - 1U) {
      return;
    }
    (void)snprintf(output + used, capacity - used, "%s%s",
                   index == 0U ? "" : " / ", name);
  }
}

static void library_draw_help_hint(MereaderTuiLibraryTuiState *state, int row,
                                   int x, int width) {
  const MereaderTuiKeyList *keys = &state->config->library_keymaps.help;
  if (row < 0 || width < 7 || keys->length == 0U) {
    return;
  }
  char hint[64] = {0};
  (void)snprintf(hint, sizeof(hint), "%s help",
                 mereader_tui_key_display_name(keys->items[0]));
  const int hint_width = size_to_int(strlen(hint));
  if (hint_width > width) {
    return;
  }
  (void)mvwhline(stdscr, row, x + width - hint_width, ' ', hint_width);
  library_add_clipped(stdscr, row, x + width - hint_width, hint, hint_width);
}

static void library_draw_context(MereaderTuiLibraryTuiState *state, int row) {
  if (row < 0 || row >= state->rows || state->columns <= 0) {
    return;
  }
  const int x = state->columns > 2 ? 1 : 0;
  const int width = state->columns > 2 ? state->columns - 2 : state->columns;
  (void)wattron(stdscr, A_DIM);
  if (state->input_kind != MEREADER_TUI_LIBRARY_INPUT_NONE) {
    const char *prefix = state->input_kind == MEREADER_TUI_LIBRARY_INPUT_FILTER ? "/ "
                         : state->input_kind == MEREADER_TUI_LIBRARY_INPUT_ROOT
                             ? "library "
                             : "open ";
    const int full_prefix_width = size_to_int(strlen(prefix));
    const int prefix_width = full_prefix_width < width ? full_prefix_width
                             : width > 1               ? width - 1
                                                       : 0;
    library_add_clipped(stdscr, row, x, prefix, prefix_width);
    const int input_width = width - prefix_width;
    const size_t start =
        library_input_visible_start(&state->input, input_width);
    library_add_clipped(stdscr, row, x + prefix_width,
                        state->input.value + start, input_width);
    int cursor_x =
        x + prefix_width +
        size_to_int(library_sanitized_width(state->input.value + start,
                                            state->input.cursor - start));
    if (cursor_x >= state->columns) {
      cursor_x = state->columns - 1;
    }
    if (cursor_x < 0) {
      cursor_x = 0;
    }
    (void)move(row, cursor_x);
  } else if (state->status != NULL && state->status[0] != '\0') {
    library_add_clipped(stdscr, row, x, state->status, width);
  } else {
    char context[256] = {0};
    if (state->catalog == NULL) {
      if (state->view.length == 0U) {
        (void)snprintf(context, sizeof(context), "%s - %s%s",
                       state->filter[0] == '\0' ? "0 books" : "no matches",
                       mereader_tui_library_sort_name(state->sort),
                       state->filter[0] == '\0' ? "" : " - /");
      } else if (state->filter[0] == '\0') {
        (void)snprintf(context, sizeof(context), "%zu/%zu - %s",
                       state->selected + 1U, state->view.length,
                       mereader_tui_library_sort_name(state->sort));
      } else {
        (void)snprintf(context, sizeof(context), "%zu/%zu - %s - /",
                       state->selected + 1U, state->view.length,
                       mereader_tui_library_sort_name(state->sort));
      }
      library_add_clipped(stdscr, row, x, context, width);
      if (state->filter[0] != '\0') {
        const int context_width = size_to_int(strlen(context));
        if (context_width < width) {
          library_add_clipped(stdscr, row, x + context_width, state->filter,
                              width - context_width);
        }
      }
      library_draw_help_hint(state, row, x, width);
      (void)wattroff(stdscr, A_DIM);
      return;
    }
    const char *scope = state->catalog == NULL                      ? "history"
                        : state->mode == MEREADER_TUI_LIBRARY_SHELF_AUTHORS ? "authors"
                        : state->mode == MEREADER_TUI_LIBRARY_SHELF_BOOKS   ? "books"
                        : state->mode == MEREADER_TUI_LIBRARY_SHELF_FORMATS
                            ? "formats"
                            : "all books";
    if (state->view.length == 0U) {
      (void)snprintf(context, sizeof(context), "%s | %s | %s%s",
                     state->filter[0] == '\0' ? "0 books" : "no matches", scope,
                     mereader_tui_library_sort_name(state->sort),
                     state->filter[0] == '\0' ? "" : " - /");
    } else if (state->filter[0] == '\0') {
      (void)snprintf(context, sizeof(context), "%zu/%zu | %s | %s",
                     state->selected + 1U, state->view.length, scope,
                     mereader_tui_library_sort_name(state->sort));
    } else {
      (void)snprintf(context, sizeof(context), "%zu/%zu | %s | relevance | /",
                     state->selected + 1U, state->view.length, scope);
    }
    library_add_clipped(stdscr, row, x, context, width);
    if (state->filter[0] != '\0') {
      const int context_width = size_to_int(strlen(context));
      if (context_width < width) {
        library_add_clipped(stdscr, row, x + context_width, state->filter,
                            width - context_width);
      }
    }
  }
  if (state->input_kind == MEREADER_TUI_LIBRARY_INPUT_NONE) {
    library_draw_help_hint(state, row, x, width);
  }
  (void)wattroff(stdscr, A_DIM);
}

static void library_draw_box(int y, int x, int height, int width) {
  if (height < 2 || width < 2) {
    return;
  }
  (void)mvwaddch(stdscr, y, x, ACS_ULCORNER);
  (void)mvwhline(stdscr, y, x + 1, ACS_HLINE, width - 2);
  (void)mvwaddch(stdscr, y, x + width - 1, ACS_URCORNER);
  for (int row = y + 1; row < y + height - 1; ++row) {
    (void)mvwaddch(stdscr, row, x, ACS_VLINE);
    (void)mvwaddch(stdscr, row, x + width - 1, ACS_VLINE);
  }
  (void)mvwaddch(stdscr, y + height - 1, x, ACS_LLCORNER);
  (void)mvwhline(stdscr, y + height - 1, x + 1, ACS_HLINE, width - 2);
  (void)mvwaddch(stdscr, y + height - 1, x + width - 1, ACS_LRCORNER);
}

static size_t library_picker_visible_rows(const MereaderTuiLibraryTuiState *state) {
  return state->rows > 8 ? (size_t)(state->rows - 8) : 1U;
}

static void
library_ensure_picker_selection_visible(MereaderTuiLibraryTuiState *state) {
  if (state->picker_view.length == 0U) {
    state->picker_selected = 0U;
    state->picker_top = 0U;
    return;
  }
  if (state->picker_selected >= state->picker_view.length) {
    state->picker_selected = state->picker_view.length - 1U;
  }
  const size_t visible = library_picker_visible_rows(state);
  if (state->picker_selected < state->picker_top) {
    state->picker_top = state->picker_selected;
  } else if (state->picker_selected >= state->picker_top + visible) {
    state->picker_top = state->picker_selected - visible + 1U;
  }
  const size_t maximum_top = state->picker_view.length > visible
                                 ? state->picker_view.length - visible
                                 : 0U;
  if (state->picker_top > maximum_top) {
    state->picker_top = maximum_top;
  }
}

static void library_draw_picker_row(MereaderTuiLibraryTuiState *state, int y, int x,
                                    int width, size_t view_index) {
  if (view_index >= state->picker_view.length || width <= 0) {
    return;
  }
  const MereaderTuiLibraryRow *row = &state->picker_view.rows[view_index];
  const bool selected = view_index == state->picker_selected;
  const attr_t attributes =
      selected ? (state->colors ? A_NORMAL : A_REVERSE) : A_NORMAL;
  const short pair =
      selected && state->colors ? MEREADER_TUI_PAIR_SEARCH : MEREADER_TUI_PAIR_BASE;
  (void)wattron(stdscr, attributes | (state->colors ? COLOR_PAIR(pair) : 0));
  (void)mvwhline(stdscr, y, x, ' ', width);
  const int progress = library_progress_percent(row->entry->reading_progress);
  char suffix[16] = {0};
  (void)snprintf(suffix, sizeof(suffix), "%d%%", progress);
  const int suffix_width = size_to_int(strlen(suffix));
  const int title_width =
      width > suffix_width + 1 ? width - suffix_width - 1 : width;
  library_add_clipped(stdscr, y, x, row->title, title_width);
  if (title_width < width) {
    library_add_clipped(stdscr, y, x + width - suffix_width, suffix,
                        suffix_width);
  }
  (void)wattroff(stdscr, attributes | (state->colors ? COLOR_PAIR(pair) : 0));
}

static void library_draw_picker(MereaderTuiLibraryTuiState *state) {
  const int margin_y = state->rows >= 8 ? 1 : 0;
  const int margin_x = state->columns >= 40 ? 2 : 0;
  const int height = state->rows - margin_y * 2;
  const int width = state->columns - margin_x * 2;
  if (height <= 0 || width <= 0) {
    return;
  }
  (void)wattron(stdscr, state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_BASE) : A_NORMAL);
  for (int row = margin_y; row < margin_y + height; ++row) {
    (void)mvwhline(stdscr, row, margin_x, ' ', width);
  }
  if (height >= 2 && width >= 2) {
    library_draw_box(margin_y, margin_x, height, width);
  }
  const int inner_x = width >= 4 ? margin_x + 2 : margin_x;
  const int inner_width = width >= 4 ? width - 4 : width;
  if (inner_width <= 0) {
    return;
  }

  const int title_y = margin_y + (height >= 2 ? 1 : 0);
  if (title_y < margin_y + height) {
    (void)wattron(stdscr,
                  A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
    library_add_clipped(stdscr, title_y, inner_x, "find a book", inner_width);
    (void)wattroff(stdscr,
                   A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
  }
  const int search_y = title_y + 1;
  if (search_y < margin_y + height) {
    library_add_clipped(stdscr, search_y, inner_x, ">", 1);
    const int input_width = inner_width - 2;
    const size_t input_start =
        library_input_visible_start(&state->picker_input, input_width);
    library_add_clipped(stdscr, search_y, inner_x + 2,
                        state->picker_input.value + input_start, input_width);
  }
  const int content_y = search_y + 2;
  const int footer_y = margin_y + height - 2;
  const int content_height = footer_y - content_y;
  if (content_height > 0) {
    const bool split = inner_width >= 58;
    const int list_width = split ? inner_width / 2 : inner_width;
    const int details_x = inner_x + list_width + 2;
    const int details_width = inner_x + inner_width - details_x;
    if (split) {
      (void)wattron(stdscr, A_DIM);
      (void)mvwvline(stdscr, content_y, inner_x + list_width, ACS_VLINE,
                     content_height);
      (void)wattroff(stdscr, A_DIM);
    }
    if (state->picker_view.length == 0U) {
      library_add_clipped(stdscr, content_y, inner_x, "no matching books",
                          list_width);
    } else {
      for (int offset = 0; offset < content_height; ++offset) {
        const size_t index = state->picker_top + (size_t)offset;
        if (index >= state->picker_view.length) {
          break;
        }
        library_draw_picker_row(state, content_y + offset, inner_x, list_width,
                                index);
      }
      if (split) {
        const MereaderTuiLibraryRow *row =
            &state->picker_view.rows[state->picker_selected];
        library_draw_details(
            state, row, library_picker_selected_reference(state), content_y,
            details_x, content_height, details_width);
      }
    }
  }
  if (footer_y >= margin_y && footer_y < margin_y + height) {
    char confirm[48] = {0};
    char close[48] = {0};
    char footer[192] = {0};
    library_key_list_label(&state->config->library_keymaps.confirm, confirm,
                           sizeof(confirm));
    library_key_list_label(&state->config->library_keymaps.close, close,
                           sizeof(close));
    (void)snprintf(footer, sizeof(footer),
                   "type to filter  up/down select  %s open  %s close", confirm,
                   close);
    (void)wattron(stdscr, A_DIM);
    library_add_clipped(stdscr, footer_y, inner_x, footer, inner_width);
    (void)wattroff(stdscr, A_DIM);
  }
  if (search_y >= 0 && search_y < state->rows) {
    const int input_width = inner_width - 2;
    const size_t input_start =
        library_input_visible_start(&state->picker_input, input_width);
    int cursor_x = inner_x + 2 +
                   size_to_int(library_sanitized_width(
                       state->picker_input.value + input_start,
                       state->picker_input.cursor - input_start));
    if (cursor_x >= state->columns) {
      cursor_x = state->columns - 1;
    }
    (void)move(search_y, cursor_x);
  }
  (void)wattroff(stdscr, state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_BASE) : A_NORMAL);
}

typedef struct MereaderTuiLibraryHelpLine {
  const MereaderTuiKeyList *keys;
  const char *description;
} MereaderTuiLibraryHelpLine;

static void library_draw_help(MereaderTuiLibraryTuiState *state) {
  const MereaderTuiLibraryKeymaps *maps = &state->config->library_keymaps;
  const MereaderTuiLibraryHelpLine lines[] = {
      {.keys = NULL, .description = "navigation"},
      {.keys = &maps->move_down, .description = "select the next item"},
      {.keys = &maps->move_up, .description = "select the previous item"},
      {.keys = &maps->first, .description = "select the first item"},
      {.keys = &maps->last, .description = "select the last item"},
      {.keys = &maps->page_down, .description = "move down one screen"},
      {.keys = &maps->page_up, .description = "move up one screen"},
      {.keys = &maps->open, .description = "open the selected item"},
      {.keys = &maps->back, .description = "go back one shelf"},
      {.keys = NULL, .description = "library"},
      {.keys = &maps->toggle_view,
       .description = "switch between list and card view"},
      {.keys = &maps->toggle_shelf,
       .description = "switch between all books and authors"},
      {.keys = &maps->choose_format,
       .description = "choose a format for the selected book"},
      {.keys = &maps->sort, .description = "change the sort order"},
      {.keys = &maps->refresh, .description = "rescan the library"},
      {.keys = &maps->open_path, .description = "open a typed path or URL"},
      {.keys = NULL, .description = "search"},
      {.keys = &maps->find, .description = "find any book with preview"},
      {.keys = &maps->filter, .description = "filter the current shelf"},
      {.keys = &maps->close,
       .description = "close a prompt or clear a filter"},
      {.keys = NULL, .description = "application"},
      {.keys = &maps->help, .description = "open or close this help"},
      {.keys = &maps->quit, .description = "quit the library"},
  };
  const int margin_y = state->rows >= 10 ? 1 : 0;
  const int margin_x = state->columns >= 56 ? 4 : 0;
  const int height = state->rows - margin_y * 2;
  const int width = state->columns - margin_x * 2;
  if (height <= 0 || width <= 0) {
    return;
  }
  for (int row = margin_y; row < margin_y + height; ++row) {
    (void)mvwhline(stdscr, row, margin_x, ' ', width);
  }
  if (height >= 2 && width >= 2) {
    library_draw_box(margin_y, margin_x, height, width);
  }
  const int inner_x = width >= 4 ? margin_x + 2 : margin_x;
  const int inner_width = width >= 4 ? width - 4 : width;
  const int title_y = margin_y + (height >= 2 ? 1 : 0);
  (void)wattron(stdscr,
                A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
  library_add_clipped(stdscr, title_y, inner_x, "help", inner_width);
  (void)wattroff(stdscr,
                 A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
  const int content_y = title_y + 2;
  const int footer_y = margin_y + height - 2;
  const int visible = footer_y - content_y;
  const size_t maximum_scroll =
      visible > 0 && MEREADER_TUI_ARRAY_LEN(lines) > (size_t)visible
          ? MEREADER_TUI_ARRAY_LEN(lines) - (size_t)visible
          : 0U;
  if (state->help_scroll > maximum_scroll) {
    state->help_scroll = maximum_scroll;
  }
  const int key_width = inner_width >= 38 ? 18 : inner_width / 2;
  for (int row = 0; row < visible; ++row) {
    const size_t index = state->help_scroll + (size_t)row;
    if (index >= MEREADER_TUI_ARRAY_LEN(lines)) {
      break;
    }
    const MereaderTuiLibraryHelpLine *line = &lines[index];
    if (line->keys == NULL) {
      (void)wattron(
          stdscr, A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
      library_add_clipped(stdscr, content_y + row, inner_x, line->description,
                          inner_width);
      (void)wattroff(
          stdscr, A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
      continue;
    }
    char keys[128] = {0};
    library_key_list_label(line->keys, keys, sizeof(keys));
    (void)wattron(stdscr, A_BOLD);
    library_add_clipped(stdscr, content_y + row, inner_x, keys, key_width);
    (void)wattroff(stdscr, A_BOLD);
    library_add_clipped(stdscr, content_y + row, inner_x + key_width + 1,
                        line->description, inner_width - key_width - 1);
  }
  if (footer_y >= margin_y && footer_y < margin_y + height) {
    char down[32] = {0};
    char up[32] = {0};
    char close[32] = {0};
    char help[32] = {0};
    char footer[160] = {0};
    library_key_list_label(&maps->move_down, down, sizeof(down));
    library_key_list_label(&maps->move_up, up, sizeof(up));
    library_key_list_label(&maps->close, close, sizeof(close));
    library_key_list_label(&maps->help, help, sizeof(help));
    (void)snprintf(footer, sizeof(footer), "%s / %s scroll  %s / %s close",
                   down, up, close, help);
    (void)wattron(stdscr, A_DIM);
    library_add_clipped(stdscr, footer_y, inner_x, footer, inner_width);
    (void)wattroff(stdscr, A_DIM);
  }
}

static void library_begin_cover_frame(MereaderTuiLibraryTuiState *state) {
  state->cover_pending = false;
  if (state->graphics == NULL) {
    return;
  }
  if (state->image_mode == MEREADER_TUI_IMAGE_MODE_KITTY) {
    MereaderTuiError ignored = {0};
    (void)fflush(stdout);
    (void)mereader_tui_graphics_kitty_delete_placements(
        state->graphics, mereader_tui_terminal_graphics_write, state, &ignored);
  } else if (state->image_mode == MEREADER_TUI_IMAGE_MODE_ANSI &&
             state->raw_truecolor) {
    (void)clearok(stdscr, true);
  }
}

static void library_draw_terminal_cover(MereaderTuiLibraryTuiState *state) {
  if (!state->cover_pending || state->graphics == NULL ||
      state->cover_book_index == SIZE_MAX) {
    return;
  }
  MereaderTuiGraphicsSurface surface = {0};
  if (!library_prepare_cover(state, state->cover_book_index, state->cover_y,
                             state->cover_x, state->cover_rows,
                             state->cover_columns, &surface)) {
    return;
  }
  const MereaderTuiGraphicsPlacement placement = {
      .row = state->cover_y,
      .column = state->cover_x,
      .viewport_rows = state->rows,
      .viewport_columns = state->columns,
  };
  MereaderTuiError ignored = {0};
  (void)fflush(stdout);
  if (state->image_mode == MEREADER_TUI_IMAGE_MODE_ANSI && state->raw_truecolor) {
    (void)mereader_tui_graphics_render_ansi(
        &surface, &placement, mereader_tui_terminal_graphics_write, state, &ignored);
  } else if (state->image_mode == MEREADER_TUI_IMAGE_MODE_KITTY) {
    (void)mereader_tui_graphics_kitty_draw(state->graphics, &surface, &placement,
                                   mereader_tui_terminal_graphics_write, state,
                                   &ignored);
  }
  mereader_tui_graphics_surface_release(&surface);
}

static void library_draw_frame(MereaderTuiLibraryTuiState *state) {
  library_begin_cover_frame(state);
  (void)werase(stdscr);
  (void)wbkgd(stdscr,
              state->colors ? (chtype)COLOR_PAIR(MEREADER_TUI_PAIR_BASE) : A_NORMAL);
  const bool compact_input =
      state->rows == 2 && state->input_kind != MEREADER_TUI_LIBRARY_INPUT_NONE;
  const bool input_active = state->input_kind != MEREADER_TUI_LIBRARY_INPUT_NONE;
  const bool show_heading = state->rows >= 2 && !compact_input;
  const bool show_separator = state->rows >= 4;
  const int content_row = show_separator ? 2 : show_heading ? 1 : 0;
  const int footer_row =
      input_active || state->rows >= 3 ? state->rows - 1 : -1;
  const int content_end = footer_row >= 0 ? footer_row : state->rows;
  if (show_heading) {
    char heading[512] = {0};
    (void)snprintf(heading, sizeof(heading), "%s / history%s", MEREADER_TUI_NAME,
                   state->detail_view ? " / card" : "");
    if (state->setup_required) {
      (void)snprintf(heading, sizeof(heading), "%s / set up library",
                     MEREADER_TUI_NAME);
    } else if (state->catalog != NULL) {
      if (state->mode == MEREADER_TUI_LIBRARY_SHELF_AUTHORS) {
        (void)snprintf(heading, sizeof(heading), "%s / authors", MEREADER_TUI_NAME);
      } else if (state->mode == MEREADER_TUI_LIBRARY_SHELF_ALL) {
        (void)snprintf(heading, sizeof(heading), "%s / all books%s",
                       MEREADER_TUI_NAME, state->detail_view ? " / card" : "");
      } else if (state->mode == MEREADER_TUI_LIBRARY_SHELF_BOOKS) {
        (void)snprintf(heading, sizeof(heading), "%s / authors / %s",
                       MEREADER_TUI_NAME,
                       state->author == NULL ? "unknown" : state->author);
      } else if (state->format_book < state->catalog->length) {
        (void)snprintf(heading, sizeof(heading), "%s / formats / %s",
                       MEREADER_TUI_NAME,
                       state->catalog->books[state->format_book].title);
      }
    }
    (void)wattron(stdscr,
                  A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
    library_add_clipped(stdscr, 0, state->columns > 2 ? 1 : 0, heading,
                        state->columns > 2 ? state->columns - 2
                                           : state->columns);
    (void)wattroff(stdscr,
                   A_BOLD | (state->colors ? COLOR_PAIR(MEREADER_TUI_PAIR_ACCENT) : 0));
  }
  if (show_separator) {
    (void)wattron(stdscr, A_DIM);
    (void)mvwhline(stdscr, 1, 0, ACS_HLINE, state->columns);
    (void)wattroff(stdscr, A_DIM);
  }
  if (state->view.length == 0U && content_row < content_end) {
    const char *message =
        state->setup_required
            ? state->status != NULL && state->status[0] != '\0'
                  ? state->status
                  : "Paste the path to your book directory"
        : state->filter[0] == '\0' ? "No books yet"
                                   : "No matching books";
    library_add_clipped(
        stdscr, content_row, state->columns > 2 ? 1 : 0, message,
        state->columns > 2 ? state->columns - 2 : state->columns);
  } else if (state->detail_view && !state->picker_open && !state->help_open &&
             content_row < content_end) {
    const int card_x = state->columns >= 8 ? 2 : 0;
    const int card_width =
        state->columns >= 8 ? state->columns - 4 : state->columns;
    const int card_height = content_end - content_row;
    if (card_height >= 3 && card_width >= 4) {
      library_draw_box(content_row, card_x, card_height, card_width);
      library_draw_details(state, &state->view.rows[state->selected],
                           library_selected_reference(state), content_row + 1,
                           card_x + 2, card_height - 2, card_width - 4);
    } else {
      library_draw_details(state, &state->view.rows[state->selected],
                           library_selected_reference(state), content_row,
                           card_x, card_height, card_width);
    }
  } else {
    const size_t visible = library_visible_rows(state);
    for (size_t offset = 0U; offset < visible; ++offset) {
      const size_t index = state->top + offset;
      if (index >= state->view.length ||
          offset > (size_t)(INT_MAX - content_row) ||
          content_row + (int)offset >= content_end) {
        break;
      }
      library_draw_row(state, content_row + (int)offset, index);
    }
  }
  library_draw_context(state, footer_row);
  if (state->picker_open) {
    library_draw_picker(state);
  }
  if (state->help_open) {
    library_draw_help(state);
  }
  if (state->input_kind != MEREADER_TUI_LIBRARY_INPUT_NONE || state->picker_open) {
    if (state->previous_cursor != ERR) {
      (void)curs_set(1);
    }
  } else {
    (void)curs_set(0);
  }
  (void)wnoutrefresh(stdscr);
  (void)doupdate();
  state->dirty = false;
  library_draw_terminal_cover(state);
}

static bool library_initialize_curses(MereaderTuiLibraryTuiState *state,
                                      MereaderTuiError *error) {
  if (initscr() == NULL) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "cannot initialize terminal");
    return false;
  }
  (void)cbreak();
  (void)noecho();
  (void)keypad(stdscr, true);
  (void)set_escdelay(25);
  state->previous_cursor = curs_set(0);
  state->colors = false;
  if (has_colors() == true && start_color() == OK && COLORS >= 8 &&
      COLOR_PAIRS > MEREADER_TUI_PAIR_SEARCH) {
    const MereaderTuiColorScheme *theme = &state->config->dark;
    (void)use_default_colors();
    if (init_pair(MEREADER_TUI_PAIR_BASE, terminal_color(theme->foreground),
                  terminal_color(theme->background)) != ERR &&
        init_pair(MEREADER_TUI_PAIR_ACCENT, terminal_color(theme->accent),
                  terminal_color(theme->background)) != ERR &&
        init_pair(MEREADER_TUI_PAIR_SEARCH, terminal_color(theme->background),
                  terminal_color(theme->accent)) != ERR) {
      state->colors = true;
    }
  }
  library_update_dimensions(state);
  (void)wbkgd(stdscr,
              state->colors ? (chtype)COLOR_PAIR(MEREADER_TUI_PAIR_BASE) : A_NORMAL);
  return true;
}

static void library_initialize_graphics(MereaderTuiLibraryTuiState *state) {
  const MereaderTuiGraphicsEnvironment environment = {
      .term_program = getenv("TERM_PROGRAM"),
      .term = getenv("TERM"),
      .kitty_window_id = getenv("KITTY_WINDOW_ID"),
      .colorterm = getenv("COLORTERM"),
      .tmux = getenv("TMUX"),
      .sty = getenv("STY"),
      .colors = state->colors,
      .output_is_tty = isatty(STDOUT_FILENO) != 0,
  };
  const MereaderTuiGraphicsMultiplexer multiplexer =
      mereader_tui_graphics_multiplexer(&environment);
  state->image_mode = mereader_tui_graphics_select_mode(
      state->config->image_mode, state->config->image_mode_explicit,
      &environment);
  state->raw_truecolor = mereader_tui_graphics_truecolor_available(&environment) &&
                         multiplexer == MEREADER_TUI_GRAPHICS_MULTIPLEXER_NONE;

  int pair_capacity = state->colors ? COLOR_PAIRS - MEREADER_TUI_PAIR_IMAGE_FIRST : 0;
  if (pair_capacity > 256) {
    pair_capacity = 256;
  }
  if (pair_capacity < 0) {
    pair_capacity = 0;
  }
  state->image_pair_capacity = (short)pair_capacity;
  if (state->image_mode == MEREADER_TUI_IMAGE_MODE_ANSI && !state->raw_truecolor &&
      state->image_pair_capacity == 0) {
    state->image_mode = MEREADER_TUI_IMAGE_MODE_PLACEHOLDER;
  }
  if (state->image_mode == MEREADER_TUI_IMAGE_MODE_PLACEHOLDER) {
    library_update_cell_pixels(state);
    return;
  }

  MereaderTuiError ignored = {0};
  state->graphics =
      mereader_tui_graphics_create(MEREADER_TUI_GRAPHICS_DEFAULT_CACHE_BYTES, multiplexer,
                           state->config->dark.background, &ignored);
  if (state->graphics == NULL ||
      !mereader_tui_graphics_resize(state->graphics, state->columns, state->rows,
                            &ignored)) {
    mereader_tui_graphics_free(state->graphics);
    state->graphics = NULL;
    state->image_mode = MEREADER_TUI_IMAGE_MODE_PLACEHOLDER;
  }
  library_update_cell_pixels(state);
}

static void library_begin_filter(MereaderTuiLibraryTuiState *state) {
  MereaderTuiError error = {0};
  free(state->input_selection);
  state->input_selection = NULL;
  const char *selected = library_selected_filepath(state);
  if (selected != NULL) {
    state->input_selection = mereader_tui_strdup(selected, &error);
    if (state->input_selection == NULL) {
      library_runtime_error(state, &error, "cannot start filter");
      return;
    }
  }
  (void)snprintf(state->filter_before, sizeof(state->filter_before), "%s",
                 state->filter);
  memset(&state->input, 0, sizeof(state->input));
  (void)snprintf(state->input.value, sizeof(state->input.value), "%s",
                 state->filter);
  state->input.length = strlen(state->input.value);
  state->input.cursor = state->input.length;
  state->input_kind = MEREADER_TUI_LIBRARY_INPUT_FILTER;
  library_clear_status(state);
  state->dirty = true;
}

static void library_begin_path(MereaderTuiLibraryTuiState *state) {
  memset(&state->input, 0, sizeof(state->input));
  state->input_kind = MEREADER_TUI_LIBRARY_INPUT_PATH;
  library_clear_status(state);
  state->dirty = true;
}

static void library_begin_root(MereaderTuiLibraryTuiState *state) {
  memset(&state->input, 0, sizeof(state->input));
  state->input_kind = MEREADER_TUI_LIBRARY_INPUT_ROOT;
  library_clear_status(state);
  state->dirty = true;
}

static void library_finish_input(MereaderTuiLibraryTuiState *state) {
  state->input_kind = MEREADER_TUI_LIBRARY_INPUT_NONE;
  memset(&state->input, 0, sizeof(state->input));
  free(state->input_selection);
  state->input_selection = NULL;
  state->dirty = true;
}

static bool library_update_filter(MereaderTuiLibraryTuiState *state) {
  MereaderTuiError error = {0};
  if (!library_rebuild_view(state, state->input.value, state->sort,
                            state->input_selection, &error)) {
    library_runtime_error(state, &error, "cannot filter library");
    return false;
  }
  library_clear_status(state);
  return true;
}

static void library_cancel_input(MereaderTuiLibraryTuiState *state) {
  if (state->input_kind == MEREADER_TUI_LIBRARY_INPUT_FILTER &&
      strcmp(state->filter, state->filter_before) != 0) {
    MereaderTuiError error = {0};
    if (!library_rebuild_view(state, state->filter_before, state->sort,
                              state->input_selection, &error)) {
      library_runtime_error(state, &error, "cannot restore filter");
      return;
    }
  }
  library_clear_status(state);
  library_finish_input(state);
}

static void library_emit(MereaderTuiLibraryTuiState *state, MereaderTuiLibraryCommand command,
                         const char *path) {
  char *stored_path = NULL;
  if (path != NULL) {
    MereaderTuiError error = {0};
    stored_path = mereader_tui_strdup(path, &error);
    if (stored_path == NULL) {
      library_runtime_error(state, &error, "cannot select book");
      return;
    }
  }
  free(state->action.path);
  state->action = (MereaderTuiLibraryAction){
      .command = command,
      .path = stored_path,
      .sort = state->sort,
  };
  state->quit = true;
}

static void library_close_picker(MereaderTuiLibraryTuiState *state) {
  state->picker_open = false;
  memset(&state->picker_input, 0, sizeof(state->picker_input));
  mereader_tui_library_view_free(&state->picker_view);
  mereader_tui_library_shelf_free(&state->picker_shelf);
  state->picker_selected = 0U;
  state->picker_top = 0U;
  state->dirty = true;
}

static void library_open_picker(MereaderTuiLibraryTuiState *state) {
  memset(&state->picker_input, 0, sizeof(state->picker_input));
  state->picker_selected = 0U;
  state->picker_top = 0U;
  MereaderTuiError error = {0};
  if (!library_rebuild_picker(state, library_selected_filepath(state),
                              &error)) {
    library_runtime_error(state, &error, "cannot open book search");
    return;
  }
  library_ensure_picker_selection_visible(state);
  state->picker_open = true;
  library_clear_status(state);
  state->dirty = true;
}

static void library_move_picker_selection(MereaderTuiLibraryTuiState *state,
                                          int amount) {
  if (state->picker_view.length == 0U) {
    return;
  }
  if (amount < 0) {
    const size_t distance = (size_t)(-(long)amount);
    state->picker_selected = distance > state->picker_selected
                                 ? 0U
                                 : state->picker_selected - distance;
  } else {
    const size_t maximum = state->picker_view.length - 1U;
    const size_t distance = (size_t)amount;
    state->picker_selected = distance > maximum - state->picker_selected
                                 ? maximum
                                 : state->picker_selected + distance;
  }
  library_ensure_picker_selection_visible(state);
  state->dirty = true;
}

static void library_handle_picker_key(MereaderTuiLibraryTuiState *state, bool key_code,
                                      int code, wchar_t character) {
  const MereaderTuiKey key = library_key(key_code, code, character);
  const MereaderTuiLibraryKeyAction action =
      library_modal_key_action(state, &key);
  if (action == MEREADER_TUI_LIBRARY_KEY_CLOSE) {
    library_close_picker(state);
    return;
  }
  if (action == MEREADER_TUI_LIBRARY_KEY_CONFIRM) {
    const char *path = library_picker_selected_filepath(state);
    if (path != NULL && path[0] != '\0') {
      library_emit(state, MEREADER_TUI_LIBRARY_COMMAND_OPEN, path);
    }
    return;
  }
  if (key_code && code == KEY_DOWN) {
    library_move_picker_selection(state, 1);
    return;
  }
  if (key_code && code == KEY_UP) {
    library_move_picker_selection(state, -1);
    return;
  }
  if (key_code && code == KEY_NPAGE) {
    library_move_picker_selection(
        state, size_to_int(library_picker_visible_rows(state)));
    return;
  }
  if (key_code && code == KEY_PPAGE) {
    library_move_picker_selection(
        state, -size_to_int(library_picker_visible_rows(state)));
    return;
  }

  const bool changed =
      mereader_tui_text_input_apply(&state->picker_input, key_code, code, character);

  if (changed) {
    const char *selected = library_picker_selected_filepath(state);
    MereaderTuiError error = {0};
    if (!library_rebuild_picker(state, selected, &error)) {
      library_runtime_error(state, &error, "cannot search books");
      return;
    }
    library_ensure_picker_selection_visible(state);
  }
  state->dirty = true;
}

static void library_handle_input_key(MereaderTuiLibraryTuiState *state, bool key_code,
                                     int code, wchar_t character) {
  const MereaderTuiKey key = library_key(key_code, code, character);
  const MereaderTuiLibraryKeyAction action =
      library_modal_key_action(state, &key);
  if (action == MEREADER_TUI_LIBRARY_KEY_CLOSE) {
    library_cancel_input(state);
    return;
  }
  if (action == MEREADER_TUI_LIBRARY_KEY_CONFIRM) {
    if (state->input_kind == MEREADER_TUI_LIBRARY_INPUT_ROOT) {
      if (state->input.length == 0U) {
        library_copy_status(state, "enter a book directory");
        state->dirty = true;
      } else {
        library_emit(state, MEREADER_TUI_LIBRARY_COMMAND_SET_ROOT, state->input.value);
      }
    } else if (state->input_kind == MEREADER_TUI_LIBRARY_INPUT_PATH &&
               state->input.length > 0U) {
      library_emit(state, MEREADER_TUI_LIBRARY_COMMAND_OPEN, state->input.value);
    } else if (state->input_kind == MEREADER_TUI_LIBRARY_INPUT_FILTER &&
               strcmp(state->filter, state->input.value) != 0 &&
               !library_update_filter(state)) {
      return;
    } else {
      library_finish_input(state);
    }
    return;
  }

  const bool changed =
      mereader_tui_text_input_apply(&state->input, key_code, code, character);
  if (changed && state->input_kind == MEREADER_TUI_LIBRARY_INPUT_FILTER) {
    (void)library_update_filter(state);
  }
  state->dirty = true;
}

static void library_cycle_sort(MereaderTuiLibraryTuiState *state) {
  const char *selected = library_selected_filepath(state);
  const MereaderTuiLibrarySort sort = mereader_tui_library_sort_next(state->sort);
  MereaderTuiError error = {0};
  if (!library_rebuild_view(state, state->filter, sort, selected, &error)) {
    library_runtime_error(state, &error, "cannot sort library");
  } else {
    library_clear_status(state);
  }
}

static void library_clear_filter(MereaderTuiLibraryTuiState *state) {
  if (state->filter[0] == '\0') {
    return;
  }
  const char *selected = library_selected_filepath(state);
  MereaderTuiError error = {0};
  if (!library_rebuild_view(state, "", state->sort, selected, &error)) {
    library_runtime_error(state, &error, "cannot clear filter");
  } else {
    library_clear_status(state);
  }
}

static void library_change_mode(MereaderTuiLibraryTuiState *state,
                                MereaderTuiLibraryShelfKind mode, const char *author,
                                size_t format_book) {
  char *stored_author = NULL;
  MereaderTuiError error = {0};
  if (author != NULL) {
    stored_author = mereader_tui_strdup(author, &error);
    if (stored_author == NULL) {
      library_runtime_error(state, &error, "cannot open shelf");
      return;
    }
  }
  if (!library_replace_view(state, mode, stored_author, format_book, "",
                            state->sort, NULL, &error)) {
    free(stored_author);
    library_runtime_error(state, &error, "cannot open shelf");
  } else {
    free(state->author);
    state->author = stored_author;
    state->mode = mode;
    state->format_book = format_book;
    state->selected = 0U;
    state->top = 0U;
    state->filter[0] = '\0';
    library_ensure_selection_visible(state);
    library_clear_status(state);
  }
}

static void library_open_selected(MereaderTuiLibraryTuiState *state) {
  const MereaderTuiLibraryShelfReference *reference =
      library_selected_reference(state);
  if (reference != NULL && state->mode == MEREADER_TUI_LIBRARY_SHELF_AUTHORS &&
      reference->book_index < state->catalog->length) {
    library_change_mode(state, MEREADER_TUI_LIBRARY_SHELF_BOOKS,
                        state->catalog->books[reference->book_index].author,
                        0U);
    return;
  }
  const char *path = library_selected_filepath(state);
  if (path == NULL || path[0] == '\0') {
    library_copy_status(state, "book path unavailable");
    state->dirty = true;
  } else {
    library_emit(state, MEREADER_TUI_LIBRARY_COMMAND_OPEN, path);
  }
}

static void library_open_formats(MereaderTuiLibraryTuiState *state) {
  const MereaderTuiLibraryShelfReference *reference =
      library_selected_reference(state);
  if (reference == NULL || reference->book_index >= state->catalog->length ||
      state->mode == MEREADER_TUI_LIBRARY_SHELF_AUTHORS) {
    library_copy_status(state, "select a book to view formats");
    state->dirty = true;
    return;
  }
  state->format_parent = state->mode;
  library_change_mode(state, MEREADER_TUI_LIBRARY_SHELF_FORMATS, NULL,
                      reference->book_index);
}

static void library_go_back(MereaderTuiLibraryTuiState *state) {
  if (state->catalog == NULL) {
    library_clear_filter(state);
  } else if (state->filter[0] != '\0') {
    library_clear_filter(state);
  } else if (state->mode == MEREADER_TUI_LIBRARY_SHELF_FORMATS) {
    const char *author = state->format_book < state->catalog->length
                             ? state->catalog->books[state->format_book].author
                             : NULL;
    const MereaderTuiLibraryShelfKind parent =
        state->format_parent == MEREADER_TUI_LIBRARY_SHELF_BOOKS
            ? MEREADER_TUI_LIBRARY_SHELF_BOOKS
            : MEREADER_TUI_LIBRARY_SHELF_ALL;
    library_change_mode(state, parent,
                        parent == MEREADER_TUI_LIBRARY_SHELF_BOOKS ? author : NULL, 0U);
  } else if (state->mode == MEREADER_TUI_LIBRARY_SHELF_BOOKS) {
    library_change_mode(state, MEREADER_TUI_LIBRARY_SHELF_AUTHORS, NULL, 0U);
  } else {
    library_clear_filter(state);
  }
}

static void library_handle_key(MereaderTuiLibraryTuiState *state, bool key_code,
                               int code, wchar_t character) {
  const MereaderTuiKey key = library_key(key_code, code, character);
  const MereaderTuiLibraryKeymaps *maps = &state->config->library_keymaps;
  if (state->help_open) {
    const MereaderTuiLibraryKeyEntry entries[] = {
        {MEREADER_TUI_LIBRARY_KEY_CLOSE, &maps->close},
        {MEREADER_TUI_LIBRARY_KEY_CLOSE, &maps->help},
        {MEREADER_TUI_LIBRARY_KEY_QUIT, &maps->quit},
        {MEREADER_TUI_LIBRARY_KEY_MOVE_DOWN, &maps->move_down},
        {MEREADER_TUI_LIBRARY_KEY_MOVE_DOWN, &maps->page_down},
        {MEREADER_TUI_LIBRARY_KEY_MOVE_UP, &maps->move_up},
        {MEREADER_TUI_LIBRARY_KEY_MOVE_UP, &maps->page_up},
    };
    const MereaderTuiLibraryKeyAction action = library_match_key_entries(
        state, entries, MEREADER_TUI_ARRAY_LEN(entries), &key);
    if (action == MEREADER_TUI_LIBRARY_KEY_CLOSE) {
      state->help_open = false;
      state->dirty = true;
    } else if (action == MEREADER_TUI_LIBRARY_KEY_QUIT) {
      library_emit(state, MEREADER_TUI_LIBRARY_COMMAND_QUIT, NULL);
    } else if (action == MEREADER_TUI_LIBRARY_KEY_MOVE_DOWN) {
      if (state->help_scroll < SIZE_MAX) {
        ++state->help_scroll;
      }
      state->dirty = true;
    } else if (action == MEREADER_TUI_LIBRARY_KEY_MOVE_UP) {
      if (state->help_scroll > 0U) {
        --state->help_scroll;
      }
      state->dirty = true;
    }
    return;
  }
  if (state->picker_open) {
    library_handle_picker_key(state, key_code, code, character);
    return;
  }
  if (state->input_kind != MEREADER_TUI_LIBRARY_INPUT_NONE) {
    library_handle_input_key(state, key_code, code, character);
    return;
  }

  switch (library_key_action(state, &key)) {
  case MEREADER_TUI_LIBRARY_KEY_MOVE_DOWN:
    library_move_selection(state, 1);
    break;
  case MEREADER_TUI_LIBRARY_KEY_MOVE_UP:
    library_move_selection(state, -1);
    break;
  case MEREADER_TUI_LIBRARY_KEY_FIRST:
    state->selected = 0U;
    library_ensure_selection_visible(state);
    state->dirty = true;
    break;
  case MEREADER_TUI_LIBRARY_KEY_LAST:
    if (state->view.length > 0U) {
      state->selected = state->view.length - 1U;
      library_ensure_selection_visible(state);
      state->dirty = true;
    }
    break;
  case MEREADER_TUI_LIBRARY_KEY_PAGE_DOWN:
    library_move_selection(state, size_to_int(library_visible_rows(state)));
    break;
  case MEREADER_TUI_LIBRARY_KEY_PAGE_UP:
    library_move_selection(state, -size_to_int(library_visible_rows(state)));
    break;
  case MEREADER_TUI_LIBRARY_KEY_OPEN:
    library_open_selected(state);
    break;
  case MEREADER_TUI_LIBRARY_KEY_FILTER:
    library_begin_filter(state);
    break;
  case MEREADER_TUI_LIBRARY_KEY_SORT:
    library_cycle_sort(state);
    break;
  case MEREADER_TUI_LIBRARY_KEY_TOGGLE_VIEW:
    state->detail_view = !state->detail_view;
    library_clear_status(state);
    state->dirty = true;
    break;
  case MEREADER_TUI_LIBRARY_KEY_HELP:
    state->help_open = true;
    state->help_scroll = 0U;
    library_clear_status(state);
    state->dirty = true;
    break;
  case MEREADER_TUI_LIBRARY_KEY_REFRESH:
    library_emit(state, MEREADER_TUI_LIBRARY_COMMAND_REFRESH,
                 library_selected_filepath(state));
    break;
  case MEREADER_TUI_LIBRARY_KEY_OPEN_PATH:
    library_begin_path(state);
    break;
  case MEREADER_TUI_LIBRARY_KEY_CHOOSE_FORMAT:
    if (state->catalog != NULL) {
      library_open_formats(state);
    }
    break;
  case MEREADER_TUI_LIBRARY_KEY_TOGGLE_SHELF:
    if (state->catalog != NULL) {
      library_change_mode(state,
                          state->mode == MEREADER_TUI_LIBRARY_SHELF_ALL
                              ? MEREADER_TUI_LIBRARY_SHELF_AUTHORS
                              : MEREADER_TUI_LIBRARY_SHELF_ALL,
                          NULL, 0U);
    }
    break;
  case MEREADER_TUI_LIBRARY_KEY_FIND:
    library_open_picker(state);
    break;
  case MEREADER_TUI_LIBRARY_KEY_BACK:
  case MEREADER_TUI_LIBRARY_KEY_CLOSE:
    library_go_back(state);
    break;
  case MEREADER_TUI_LIBRARY_KEY_QUIT:
    library_emit(state, MEREADER_TUI_LIBRARY_COMMAND_QUIT, NULL);
    break;
  case MEREADER_TUI_LIBRARY_KEY_CONFIRM:
  case MEREADER_TUI_LIBRARY_KEY_NONE:
    break;
  }
}

int mereader_tui_library_tui_run(const MereaderTuiConfig *config, const MereaderTuiHistory *history,
                         MereaderTuiCatalog *catalog, bool setup_required,
                         MereaderTuiLibrarySort sort, const char *selected_filepath,
                         const char *context, MereaderTuiLibraryAction *action,
                         MereaderTuiError *error) {
  if (config == NULL || history == NULL || action == NULL) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                   "invalid library application state");
    return EXIT_FAILURE;
  }
  if (action->command != MEREADER_TUI_LIBRARY_COMMAND_NONE || action->path != NULL) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                   "library action output is not empty");
    return EXIT_FAILURE;
  }

  MereaderTuiLibraryTuiState state = {
      .config = config,
      .history = history,
      .catalog = catalog,
      .mode = MEREADER_TUI_LIBRARY_SHELF_ALL,
      .format_parent = MEREADER_TUI_LIBRARY_SHELF_ALL,
      .sort = sort,
      .cover_book_index = SIZE_MAX,
      .setup_required = setup_required,
      .previous_cursor = ERR,
      .dirty = true,
  };
  if (!library_rebuild_view(&state, "", sort, selected_filepath, error)) {
    library_clear_status(&state);
    mereader_tui_library_shelf_free(&state.shelf);
    return EXIT_FAILURE;
  }
  if (state.setup_required) {
    library_begin_root(&state);
  }
  library_copy_status(&state, context);

  if (!mereader_tui_terminal_runtime_begin(error)) {
    library_clear_status(&state);
    mereader_tui_library_view_free(&state.view);
    mereader_tui_library_shelf_free(&state.shelf);
    return EXIT_FAILURE;
  }
  if (!library_initialize_curses(&state, error)) {
    library_clear_status(&state);
    mereader_tui_library_view_free(&state.view);
    mereader_tui_library_shelf_free(&state.shelf);
    return mereader_tui_terminal_runtime_finish(EXIT_FAILURE, error);
  }
  library_initialize_graphics(&state);

  int result = EXIT_SUCCESS;
  if (!mereader_tui_terminal_runtime_activate(error)) {
    result = EXIT_FAILURE;
    goto cleanup;
  }
  while (!state.quit) {
    if (mereader_tui_terminal_runtime_interrupted()) {
      break;
    }
    if (state.dirty) {
      library_draw_frame(&state);
    }
    wtimeout(stdscr, 100);
    wint_t input = 0;
    const int status = wget_wch(stdscr, &input);
    if (mereader_tui_terminal_runtime_interrupted()) {
      break;
    }
    if (status == ERR) {
      continue;
    }
    const bool key_code = status == KEY_CODE_YES;
    const int code = key_code ? (int)input : 0;
    const wchar_t character = key_code ? L'\0' : (wchar_t)input;
    if (key_code && code == KEY_RESIZE) {
      library_update_dimensions(&state);
      continue;
    }
    library_handle_key(&state, key_code, code, character);
  }

cleanup:
  if (state.graphics != NULL && state.image_mode == MEREADER_TUI_IMAGE_MODE_KITTY) {
    MereaderTuiError ignored = {0};
    (void)fflush(stdout);
    (void)mereader_tui_graphics_kitty_delete_all(
        state.graphics, mereader_tui_terminal_graphics_write, &state, &ignored);
  }
  library_clear_cover(&state);
  mereader_tui_graphics_free(state.graphics);
  state.graphics = NULL;
  if (state.previous_cursor != ERR) {
    (void)curs_set(state.previous_cursor);
  }
  (void)endwin();
  free(state.input_selection);
  mereader_tui_library_view_free(&state.picker_view);
  mereader_tui_library_shelf_free(&state.picker_shelf);
  library_clear_status(&state);
  mereader_tui_library_view_free(&state.view);
  mereader_tui_library_shelf_free(&state.shelf);
  free(state.author);
  result = mereader_tui_terminal_runtime_finish(result, error);
  if (result == EXIT_SUCCESS) {
    *action = state.action;
  } else {
    free(state.action.path);
  }
  return result;
}
