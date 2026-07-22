#include "baca/library_tui.h"
#include "baca/graphics.h"
#include "baca/library_shelf.h"
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
  BACA_PAIR_BASE = 1,
  BACA_PAIR_ACCENT,
  BACA_PAIR_SEARCH,
  BACA_PAIR_ALERT,
  BACA_PAIR_IMAGE_FIRST,
};

typedef enum BacaLibraryInputKind {
  BACA_LIBRARY_INPUT_NONE = 0,
  BACA_LIBRARY_INPUT_FILTER,
  BACA_LIBRARY_INPUT_PATH,
  BACA_LIBRARY_INPUT_ROOT,
} BacaLibraryInputKind;

typedef struct BacaLibraryTuiState {
  const BacaConfig *config;
  const BacaHistory *history;
  BacaCatalog *catalog;
  BacaGraphicsContext *graphics;
  BacaDocument cover_document;
  BacaLibraryShelf shelf;
  BacaLibraryShelfKind mode;
  BacaLibraryShelfKind format_parent;
  char *author;
  size_t format_book;
  BacaLibraryView view;
  BacaLibraryShelf picker_shelf;
  BacaLibraryView picker_view;
  BacaLibraryAction action;
  BacaTextInput input;
  BacaTextInput picker_input;
  BacaLibraryInputKind input_kind;
  BacaLibrarySort sort;
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
  char filter[BACA_TEXT_INPUT_CAPACITY];
  char filter_before[BACA_TEXT_INPUT_CAPACITY];
  char *status;
  char *input_selection;
  BacaImageMode image_mode;
  short image_pair_capacity;
  bool colors;
  bool raw_truecolor;
  bool cover_pending;
  bool detail_view;
  bool help_open;
  bool picker_open;
  bool setup_required;
  bool space_pending;
  bool g_pending;
  bool quit;
  bool dirty;
} BacaLibraryTuiState;

static bool library_key_is_enter(bool key_code, int code, wchar_t character);
static void library_draw_box(int y, int x, int height, int width);
static void library_ensure_picker_selection_visible(BacaLibraryTuiState *state);

static int size_to_int(size_t value) {
  return value > (size_t)INT_MAX ? INT_MAX : (int)value;
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
  BacaError ignored = {0};
  char *safe = baca_library_sanitize_text(text, (size_t)columns, &ignored);
  if (safe != NULL) {
    (void)mvwaddnstr(window, y, x, safe, size_to_int(strlen(safe)));
  }
  free(safe);
}

static short terminal_color(uint32_t rgb) {
  return (short)baca_graphics_rgb_to_palette(rgb, (unsigned)COLORS);
}

static size_t library_visible_rows(const BacaLibraryTuiState *state) {
  return state->rows > 3 ? (size_t)(state->rows - 3) : 1U;
}

static void library_ensure_selection_visible(BacaLibraryTuiState *state) {
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

static void library_update_cell_pixels(BacaLibraryTuiState *state) {
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

static void library_update_dimensions(BacaLibraryTuiState *state) {
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
    BacaError ignored = {0};
    if (state->image_mode == BACA_IMAGE_MODE_KITTY) {
      (void)baca_graphics_kitty_delete_placements(
          state->graphics, baca_terminal_graphics_write, state, &ignored);
    }
    (void)baca_graphics_resize(state->graphics, state->columns, state->rows,
                               &ignored);
  }
  library_update_cell_pixels(state);
  library_ensure_selection_visible(state);
  library_ensure_picker_selection_visible(state);
  state->dirty = true;
}

static void library_clear_status(BacaLibraryTuiState *state) {
  free(state->status);
  state->status = NULL;
}

static void library_copy_status(BacaLibraryTuiState *state,
                                const char *message) {
  BacaError ignored = {0};
  char *safe = baca_library_sanitize_text(message, SIZE_MAX, &ignored);
  if (safe != NULL) {
    library_clear_status(state);
    state->status = safe;
  }
}

static bool library_replace_view(BacaLibraryTuiState *state,
                                 BacaLibraryShelfKind mode, const char *author,
                                 size_t format_book, const char *filter,
                                 BacaLibrarySort sort,
                                 const char *selected_filepath,
                                 BacaError *error) {
  BacaLibraryShelf replacement_shelf = {0};
  const BacaHistory *history = state->history;
  if (state->catalog != NULL) {
    if (!baca_library_shelf_build(&replacement_shelf, state->catalog, mode,
                                  author, format_book, filter, error)) {
      return false;
    }
    history = &replacement_shelf.entries;
  }
  const char *view_filter = state->catalog == NULL ? filter : "";
  const BacaLibrarySort view_sort = state->catalog != NULL && filter[0] != '\0'
                                        ? BACA_LIBRARY_SORT_RELEVANCE
                                        : sort;
  BacaLibraryView replacement_view = {0};
  if (!baca_library_view_build(&replacement_view, history, view_filter,
                               view_sort, error)) {
    baca_library_shelf_free(&replacement_shelf);
    return false;
  }
  const size_t selected = baca_library_preserve_selection(
      &replacement_view, selected_filepath, state->selected);
  baca_library_view_free(&state->view);
  baca_library_shelf_free(&state->shelf);
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

static bool library_rebuild_view(BacaLibraryTuiState *state, const char *filter,
                                 BacaLibrarySort sort,
                                 const char *selected_filepath,
                                 BacaError *error) {
  return library_replace_view(state, state->mode, state->author,
                              state->format_book, filter, sort,
                              selected_filepath, error);
}

static bool library_rebuild_picker(BacaLibraryTuiState *state,
                                   const char *selected_filepath,
                                   BacaError *error) {
  BacaLibraryShelf replacement_shelf = {0};
  const BacaHistory *history = state->history;
  if (state->catalog != NULL) {
    if (!baca_library_shelf_build(&replacement_shelf, state->catalog,
                                  BACA_LIBRARY_SHELF_ALL, NULL, 0U,
                                  state->picker_input.value, error)) {
      return false;
    }
    history = &replacement_shelf.entries;
  }
  const char *filter = state->catalog == NULL ? state->picker_input.value : "";
  const BacaLibrarySort sort = state->picker_input.length == 0U
                                   ? state->sort
                                   : BACA_LIBRARY_SORT_RELEVANCE;
  BacaLibraryView replacement_view = {0};
  if (!baca_library_view_build(&replacement_view, history, filter, sort,
                               error)) {
    baca_library_shelf_free(&replacement_shelf);
    return false;
  }
  const size_t selected = baca_library_preserve_selection(
      &replacement_view, selected_filepath, state->picker_selected);
  baca_library_view_free(&state->picker_view);
  baca_library_shelf_free(&state->picker_shelf);
  state->picker_view = replacement_view;
  state->picker_shelf = replacement_shelf;
  state->picker_selected = selected;
  state->picker_top = selected;
  return true;
}

static const char *library_selected_filepath(const BacaLibraryTuiState *state) {
  if (state->selected >= state->view.length) {
    return NULL;
  }
  return state->view.rows[state->selected].entry->filepath;
}

static const BacaLibraryShelfReference *
library_selected_reference(const BacaLibraryTuiState *state) {
  if (state->catalog == NULL || state->selected >= state->view.length) {
    return NULL;
  }
  return baca_library_shelf_reference(&state->shelf,
                                      state->view.rows[state->selected].entry);
}

static const char *
library_picker_selected_filepath(const BacaLibraryTuiState *state) {
  if (state->picker_selected >= state->picker_view.length) {
    return NULL;
  }
  return state->picker_view.rows[state->picker_selected].entry->filepath;
}

static const BacaLibraryShelfReference *
library_picker_selected_reference(const BacaLibraryTuiState *state) {
  if (state->catalog == NULL ||
      state->picker_selected >= state->picker_view.length) {
    return NULL;
  }
  return baca_library_shelf_reference(
      &state->picker_shelf,
      state->picker_view.rows[state->picker_selected].entry);
}

static const BacaCatalogBook *
library_book_for_reference(const BacaLibraryTuiState *state,
                           const BacaLibraryShelfReference *reference) {
  if (state->catalog == NULL || reference == NULL ||
      reference->book_index >= state->catalog->length) {
    return NULL;
  }
  return &state->catalog->books[reference->book_index];
}

static void library_runtime_error(BacaLibraryTuiState *state,
                                  const BacaError *error,
                                  const char *fallback) {
  library_copy_status(state, error != NULL && error->message[0] != '\0'
                                 ? error->message
                                 : fallback);
  state->dirty = true;
}

static void library_move_selection(BacaLibraryTuiState *state, int amount) {
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
  if (!baca_library_parse_timestamp(last_read, &parsed)) {
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

static void library_draw_row(BacaLibraryTuiState *state, int screen_row,
                             size_t view_index) {
  if (view_index >= state->view.length || state->columns <= 0) {
    return;
  }
  const BacaLibraryRow *row = &state->view.rows[view_index];
  const int x = state->columns > 2 ? 1 : 0;
  const int width = state->columns > 2 ? state->columns - 2 : state->columns;
  if (width <= 0) {
    return;
  }

  const bool selected = view_index == state->selected;
  const attr_t selected_attributes =
      selected ? (state->colors ? A_NORMAL : A_REVERSE) : A_NORMAL;
  const short selected_pair =
      selected && state->colors ? BACA_PAIR_SEARCH : BACA_PAIR_BASE;
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

static void library_draw_progress(BacaLibraryTuiState *state, int y, int x,
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

static void library_clear_cover(BacaLibraryTuiState *state) {
  baca_document_close(&state->cover_document);
}

static bool library_load_cover(BacaLibraryTuiState *state, size_t book_index) {
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
  const BacaCatalogBook *book = &state->catalog->books[book_index];
  const BacaCatalogFormat *preferred = baca_catalog_preferred_format(book);
  BacaError ignored = {0};
  char *directory =
      preferred == NULL ? NULL : baca_path_dirname(preferred->path, &ignored);
  if (directory == NULL) {
    return false;
  }
  for (size_t index = 0U; index < BACA_ARRAY_LEN(names); ++index) {
    char *path = baca_path_join(directory, names[index], &ignored);
    if (path == NULL) {
      continue;
    }
    struct stat status = {0};
    const bool regular = lstat(path, &status) == 0 && S_ISREG(status.st_mode);
    if (regular && baca_document_open(&state->cover_document, path, &ignored) &&
        state->cover_document.block_count > 0U &&
        state->cover_document.blocks[0].kind == BACA_BLOCK_IMAGE) {
      baca_document_probe_images(&state->cover_document, true);
      if (state->cover_document.blocks[0].value.image.broken) {
        baca_document_close(&state->cover_document);
        free(path);
        continue;
      }
      free(path);
      free(directory);
      return true;
    }
    baca_document_close(&state->cover_document);
    free(path);
  }
  free(directory);
  return false;
}

static bool library_draw_cover_cell(void *user_data, int row, int column,
                                    uint32_t foreground, uint32_t background) {
  BacaLibraryTuiState *state = user_data;
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

static bool library_prepare_cover(BacaLibraryTuiState *state, size_t book_index,
                                  int y, int x, int rows, int columns,
                                  BacaGraphicsSurface *surface) {
  if (state->graphics == NULL || rows <= 0 || columns <= 0 ||
      !library_load_cover(state, book_index)) {
    return false;
  }
  BacaError ignored = {0};
  if (!baca_graphics_prepare(state->graphics, &state->cover_document, 0U,
                             columns, rows, surface, &ignored)) {
    return false;
  }
  state->cover_y = y;
  state->cover_x = x;
  state->cover_rows = rows;
  state->cover_columns = columns;
  return true;
}

static bool library_draw_cover(BacaLibraryTuiState *state, size_t book_index,
                               int y, int x, int rows, int columns) {
  BacaGraphicsSurface surface = {0};
  if (!library_prepare_cover(state, book_index, y, x, rows, columns,
                             &surface)) {
    return false;
  }
  if ((state->image_mode == BACA_IMAGE_MODE_ANSI && state->raw_truecolor) ||
      state->image_mode == BACA_IMAGE_MODE_KITTY) {
    state->cover_pending = true;
    baca_graphics_surface_release(&surface);
    return true;
  }
  const BacaGraphicsPlacement placement = {
      .row = y,
      .column = x,
      .viewport_rows = state->rows,
      .viewport_columns = state->columns,
  };
  BacaError ignored = {0};
  const bool drawn = baca_graphics_render_cells(
      &surface, &placement, library_draw_cover_cell, state, &ignored);
  baca_graphics_surface_release(&surface);
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

static void library_draw_details(BacaLibraryTuiState *state,
                                 const BacaLibraryRow *row,
                                 const BacaLibraryShelfReference *reference,
                                 int y, int x, int height, int width) {
  if (row == NULL || height <= 0 || width <= 0) {
    return;
  }
  const BacaCatalogBook *book = library_book_for_reference(state, reference);
  const BacaCatalogFormat *preferred =
      book == NULL ? NULL : baca_catalog_preferred_format(book);
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
                A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
  library_add_clipped(stdscr, line++, detail_x, title, detail_width);
  (void)wattroff(stdscr,
                 A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
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
    BacaString formats = {0};
    BacaError ignored = {0};
    for (size_t index = 0U; index < book->format_count; ++index) {
      if (index > 0U && !baca_string_append(&formats, "  ", &ignored)) {
        break;
      }
      if (!baca_string_append(&formats, book->formats[index].name, &ignored)) {
        break;
      }
    }
    if (formats.data != NULL) {
      char value[512] = {0};
      (void)snprintf(value, sizeof(value), "formats  %s", formats.data);
      library_add_clipped(stdscr, line++, detail_x, value, detail_width);
    }
    baca_string_free(&formats);
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
  if (length >= BACA_TEXT_INPUT_CAPACITY) {
    return 0U;
  }
  char segment[BACA_TEXT_INPUT_CAPACITY] = {0};
  memcpy(segment, text, length);
  BacaError ignored = {0};
  char *safe = baca_library_sanitize_text(segment, SIZE_MAX, &ignored);
  const size_t width = safe == NULL ? 0U : baca_utf8_width(safe, strlen(safe));
  free(safe);
  return width;
}

static size_t library_input_visible_start(const BacaTextInput *input,
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
        baca_utf8_next(input->value, input->cursor, start, &width);
    start = next > start ? next : start + 1U;
  }
  return start;
}

static void library_draw_context(BacaLibraryTuiState *state, int row) {
  if (row < 0 || row >= state->rows || state->columns <= 0) {
    return;
  }
  const int x = state->columns > 2 ? 1 : 0;
  const int width = state->columns > 2 ? state->columns - 2 : state->columns;
  (void)wattron(stdscr, A_DIM);
  if (state->input_kind != BACA_LIBRARY_INPUT_NONE) {
    const char *prefix = state->input_kind == BACA_LIBRARY_INPUT_FILTER ? "/ "
                         : state->input_kind == BACA_LIBRARY_INPUT_ROOT
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
                       baca_library_sort_name(state->sort),
                       state->filter[0] == '\0' ? "" : " - /");
      } else if (state->filter[0] == '\0') {
        (void)snprintf(context, sizeof(context), "%zu/%zu - %s",
                       state->selected + 1U, state->view.length,
                       baca_library_sort_name(state->sort));
      } else {
        (void)snprintf(context, sizeof(context), "%zu/%zu - %s - /",
                       state->selected + 1U, state->view.length,
                       baca_library_sort_name(state->sort));
      }
      library_add_clipped(stdscr, row, x, context, width);
      if (state->filter[0] != '\0') {
        const int context_width = size_to_int(strlen(context));
        if (context_width < width) {
          library_add_clipped(stdscr, row, x + context_width, state->filter,
                              width - context_width);
        }
      }
      if (width >= 10) {
        const char *help = "? help";
        const int help_width = size_to_int(strlen(help));
        (void)mvwhline(stdscr, row, x + width - help_width, ' ', help_width);
        library_add_clipped(stdscr, row, x + width - help_width, help,
                            help_width);
      }
      (void)wattroff(stdscr, A_DIM);
      return;
    }
    const char *scope = state->catalog == NULL                      ? "history"
                        : state->mode == BACA_LIBRARY_SHELF_AUTHORS ? "authors"
                        : state->mode == BACA_LIBRARY_SHELF_BOOKS   ? "books"
                        : state->mode == BACA_LIBRARY_SHELF_FORMATS
                            ? "formats"
                            : "all books";
    if (state->view.length == 0U) {
      (void)snprintf(context, sizeof(context), "%s | %s | %s%s",
                     state->filter[0] == '\0' ? "0 books" : "no matches", scope,
                     baca_library_sort_name(state->sort),
                     state->filter[0] == '\0' ? "" : " - /");
    } else if (state->filter[0] == '\0') {
      (void)snprintf(context, sizeof(context), "%zu/%zu | %s | %s",
                     state->selected + 1U, state->view.length, scope,
                     baca_library_sort_name(state->sort));
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
  if (state->input_kind == BACA_LIBRARY_INPUT_NONE && width >= 10) {
    const char *help = "? help";
    const int help_width = size_to_int(strlen(help));
    (void)mvwhline(stdscr, row, x + width - help_width, ' ', help_width);
    library_add_clipped(stdscr, row, x + width - help_width, help, help_width);
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

static size_t library_picker_visible_rows(const BacaLibraryTuiState *state) {
  return state->rows > 8 ? (size_t)(state->rows - 8) : 1U;
}

static void
library_ensure_picker_selection_visible(BacaLibraryTuiState *state) {
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

static void library_draw_picker_row(BacaLibraryTuiState *state, int y, int x,
                                    int width, size_t view_index) {
  if (view_index >= state->picker_view.length || width <= 0) {
    return;
  }
  const BacaLibraryRow *row = &state->picker_view.rows[view_index];
  const bool selected = view_index == state->picker_selected;
  const attr_t attributes =
      selected ? (state->colors ? A_NORMAL : A_REVERSE) : A_NORMAL;
  const short pair =
      selected && state->colors ? BACA_PAIR_SEARCH : BACA_PAIR_BASE;
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

static void library_draw_picker(BacaLibraryTuiState *state) {
  const int margin_y = state->rows >= 8 ? 1 : 0;
  const int margin_x = state->columns >= 40 ? 2 : 0;
  const int height = state->rows - margin_y * 2;
  const int width = state->columns - margin_x * 2;
  if (height <= 0 || width <= 0) {
    return;
  }
  (void)wattron(stdscr, state->colors ? COLOR_PAIR(BACA_PAIR_BASE) : A_NORMAL);
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
                  A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
    library_add_clipped(stdscr, title_y, inner_x, "find a book", inner_width);
    (void)wattroff(stdscr,
                   A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
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
        const BacaLibraryRow *row =
            &state->picker_view.rows[state->picker_selected];
        library_draw_details(
            state, row, library_picker_selected_reference(state), content_y,
            details_x, content_height, details_width);
      }
    }
  }
  if (footer_y >= margin_y && footer_y < margin_y + height) {
    (void)wattron(stdscr, A_DIM);
    library_add_clipped(stdscr, footer_y, inner_x,
                        "type to filter  up/down select  enter open  esc close",
                        inner_width);
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
  (void)wattroff(stdscr, state->colors ? COLOR_PAIR(BACA_PAIR_BASE) : A_NORMAL);
}

typedef struct BacaLibraryHelpLine {
  const char *key;
  const char *description;
} BacaLibraryHelpLine;

static void library_draw_help(BacaLibraryTuiState *state) {
  static const BacaLibraryHelpLine lines[] = {
      {.key = NULL, .description = "navigation"},
      {.key = "j / down", .description = "select the next book"},
      {.key = "k / up", .description = "select the previous book"},
      {.key = "gg / home", .description = "select the first book"},
      {.key = "G / end", .description = "select the last book"},
      {.key = "ctrl+f / pgdn", .description = "move down one screen"},
      {.key = "ctrl+b / pgup", .description = "move up one screen"},
      {.key = "enter / l", .description = "open the selected item"},
      {.key = "h / backspace", .description = "go back one shelf"},
      {.key = NULL, .description = "library"},
      {.key = "v", .description = "switch between list and card view"},
      {.key = "a", .description = "switch between all books and authors"},
      {.key = "f", .description = "choose a format for the selected book"},
      {.key = "s", .description = "change the sort order"},
      {.key = "r", .description = "rescan the library"},
      {.key = "o", .description = "open a typed file path"},
      {.key = NULL, .description = "search"},
      {.key = "space space", .description = "find any book with preview"},
      {.key = "/", .description = "filter the current shelf"},
      {.key = "esc", .description = "close a prompt or clear a filter"},
      {.key = NULL, .description = "application"},
      {.key = "?", .description = "open or close this help"},
      {.key = "q", .description = "quit the library"},
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
                A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
  library_add_clipped(stdscr, title_y, inner_x, "help", inner_width);
  (void)wattroff(stdscr,
                 A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
  const int content_y = title_y + 2;
  const int footer_y = margin_y + height - 2;
  const int visible = footer_y - content_y;
  const size_t maximum_scroll =
      visible > 0 && BACA_ARRAY_LEN(lines) > (size_t)visible
          ? BACA_ARRAY_LEN(lines) - (size_t)visible
          : 0U;
  if (state->help_scroll > maximum_scroll) {
    state->help_scroll = maximum_scroll;
  }
  const int key_width = inner_width >= 38 ? 18 : inner_width / 2;
  for (int row = 0; row < visible; ++row) {
    const size_t index = state->help_scroll + (size_t)row;
    if (index >= BACA_ARRAY_LEN(lines)) {
      break;
    }
    const BacaLibraryHelpLine *line = &lines[index];
    if (line->key == NULL) {
      (void)wattron(
          stdscr, A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
      library_add_clipped(stdscr, content_y + row, inner_x, line->description,
                          inner_width);
      (void)wattroff(
          stdscr, A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
      continue;
    }
    (void)wattron(stdscr, A_BOLD);
    library_add_clipped(stdscr, content_y + row, inner_x, line->key, key_width);
    (void)wattroff(stdscr, A_BOLD);
    library_add_clipped(stdscr, content_y + row, inner_x + key_width + 1,
                        line->description, inner_width - key_width - 1);
  }
  if (footer_y >= margin_y && footer_y < margin_y + height) {
    (void)wattron(stdscr, A_DIM);
    library_add_clipped(stdscr, footer_y, inner_x, "j/k scroll  esc/? close",
                        inner_width);
    (void)wattroff(stdscr, A_DIM);
  }
}

static void library_begin_cover_frame(BacaLibraryTuiState *state) {
  state->cover_pending = false;
  if (state->graphics == NULL) {
    return;
  }
  if (state->image_mode == BACA_IMAGE_MODE_KITTY) {
    BacaError ignored = {0};
    (void)fflush(stdout);
    (void)baca_graphics_kitty_delete_placements(
        state->graphics, baca_terminal_graphics_write, state, &ignored);
  } else if (state->image_mode == BACA_IMAGE_MODE_ANSI &&
             state->raw_truecolor) {
    (void)clearok(stdscr, true);
  }
}

static void library_draw_terminal_cover(BacaLibraryTuiState *state) {
  if (!state->cover_pending || state->graphics == NULL ||
      state->cover_book_index == SIZE_MAX) {
    return;
  }
  BacaGraphicsSurface surface = {0};
  if (!library_prepare_cover(state, state->cover_book_index, state->cover_y,
                             state->cover_x, state->cover_rows,
                             state->cover_columns, &surface)) {
    return;
  }
  const BacaGraphicsPlacement placement = {
      .row = state->cover_y,
      .column = state->cover_x,
      .viewport_rows = state->rows,
      .viewport_columns = state->columns,
  };
  BacaError ignored = {0};
  (void)fflush(stdout);
  if (state->image_mode == BACA_IMAGE_MODE_ANSI && state->raw_truecolor) {
    (void)baca_graphics_render_ansi(
        &surface, &placement, baca_terminal_graphics_write, state, &ignored);
  } else if (state->image_mode == BACA_IMAGE_MODE_KITTY) {
    (void)baca_graphics_kitty_draw(state->graphics, &surface, &placement,
                                   baca_terminal_graphics_write, state,
                                   &ignored);
  }
  baca_graphics_surface_release(&surface);
}

static void library_draw_frame(BacaLibraryTuiState *state) {
  library_begin_cover_frame(state);
  (void)werase(stdscr);
  (void)wbkgd(stdscr,
              state->colors ? (chtype)COLOR_PAIR(BACA_PAIR_BASE) : A_NORMAL);
  const bool compact_input =
      state->rows == 2 && state->input_kind != BACA_LIBRARY_INPUT_NONE;
  const bool input_active = state->input_kind != BACA_LIBRARY_INPUT_NONE;
  const bool show_heading = state->rows >= 2 && !compact_input;
  const bool show_separator = state->rows >= 4;
  const int content_row = show_separator ? 2 : show_heading ? 1 : 0;
  const int footer_row =
      input_active || state->rows >= 3 ? state->rows - 1 : -1;
  const int content_end = footer_row >= 0 ? footer_row : state->rows;
  if (show_heading) {
    char heading[512] = "baca / history";
    if (state->setup_required) {
      (void)snprintf(heading, sizeof(heading), "baca / set up library");
    } else if (state->catalog != NULL) {
      if (state->mode == BACA_LIBRARY_SHELF_AUTHORS) {
        (void)snprintf(heading, sizeof(heading), "baca / authors");
      } else if (state->mode == BACA_LIBRARY_SHELF_ALL) {
        (void)snprintf(heading, sizeof(heading), "baca / all books%s",
                       state->detail_view ? " / card" : "");
      } else if (state->mode == BACA_LIBRARY_SHELF_BOOKS) {
        (void)snprintf(heading, sizeof(heading), "baca / authors / %s",
                       state->author == NULL ? "unknown" : state->author);
      } else if (state->format_book < state->catalog->length) {
        (void)snprintf(heading, sizeof(heading), "baca / formats / %s",
                       state->catalog->books[state->format_book].title);
      }
    }
    (void)wattron(stdscr,
                  A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
    library_add_clipped(stdscr, 0, state->columns > 2 ? 1 : 0, heading,
                        state->columns > 2 ? state->columns - 2
                                           : state->columns);
    (void)wattroff(stdscr,
                   A_BOLD | (state->colors ? COLOR_PAIR(BACA_PAIR_ACCENT) : 0));
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
  if (state->input_kind != BACA_LIBRARY_INPUT_NONE || state->picker_open) {
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

static bool library_initialize_curses(BacaLibraryTuiState *state,
                                      BacaError *error) {
  if (initscr() == NULL) {
    baca_error_set(error, BACA_ERROR_EXTERNAL, "cannot initialize terminal");
    return false;
  }
  (void)cbreak();
  (void)noecho();
  (void)keypad(stdscr, true);
  (void)set_escdelay(25);
  state->previous_cursor = curs_set(0);
  state->colors = false;
  if (has_colors() == true && start_color() == OK && COLORS >= 8 &&
      COLOR_PAIRS > BACA_PAIR_SEARCH) {
    const BacaColorScheme *theme = &state->config->dark;
    (void)use_default_colors();
    if (init_pair(BACA_PAIR_BASE, terminal_color(theme->foreground),
                  terminal_color(theme->background)) != ERR &&
        init_pair(BACA_PAIR_ACCENT, terminal_color(theme->accent),
                  terminal_color(theme->background)) != ERR &&
        init_pair(BACA_PAIR_SEARCH, terminal_color(theme->background),
                  terminal_color(theme->accent)) != ERR) {
      state->colors = true;
    }
  }
  library_update_dimensions(state);
  (void)wbkgd(stdscr,
              state->colors ? (chtype)COLOR_PAIR(BACA_PAIR_BASE) : A_NORMAL);
  return true;
}

static void library_initialize_graphics(BacaLibraryTuiState *state) {
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
      state->config->image_mode, state->config->image_mode_explicit,
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
    library_update_cell_pixels(state);
    return;
  }

  BacaError ignored = {0};
  state->graphics =
      baca_graphics_create(BACA_GRAPHICS_DEFAULT_CACHE_BYTES, multiplexer,
                           state->config->dark.background, &ignored);
  if (state->graphics == NULL ||
      !baca_graphics_resize(state->graphics, state->columns, state->rows,
                            &ignored)) {
    baca_graphics_free(state->graphics);
    state->graphics = NULL;
    state->image_mode = BACA_IMAGE_MODE_PLACEHOLDER;
  }
  library_update_cell_pixels(state);
}

static void library_begin_filter(BacaLibraryTuiState *state) {
  BacaError error = {0};
  free(state->input_selection);
  state->input_selection = NULL;
  const char *selected = library_selected_filepath(state);
  if (selected != NULL) {
    state->input_selection = baca_strdup(selected, &error);
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
  state->input_kind = BACA_LIBRARY_INPUT_FILTER;
  library_clear_status(state);
  state->dirty = true;
}

static void library_begin_path(BacaLibraryTuiState *state) {
  memset(&state->input, 0, sizeof(state->input));
  state->input_kind = BACA_LIBRARY_INPUT_PATH;
  library_clear_status(state);
  state->dirty = true;
}

static void library_begin_root(BacaLibraryTuiState *state) {
  memset(&state->input, 0, sizeof(state->input));
  state->input_kind = BACA_LIBRARY_INPUT_ROOT;
  library_clear_status(state);
  state->dirty = true;
}

static void library_finish_input(BacaLibraryTuiState *state) {
  state->input_kind = BACA_LIBRARY_INPUT_NONE;
  memset(&state->input, 0, sizeof(state->input));
  free(state->input_selection);
  state->input_selection = NULL;
  state->dirty = true;
}

static bool library_update_filter(BacaLibraryTuiState *state) {
  BacaError error = {0};
  if (!library_rebuild_view(state, state->input.value, state->sort,
                            state->input_selection, &error)) {
    library_runtime_error(state, &error, "cannot filter library");
    return false;
  }
  library_clear_status(state);
  return true;
}

static void library_cancel_input(BacaLibraryTuiState *state) {
  if (state->input_kind == BACA_LIBRARY_INPUT_FILTER &&
      strcmp(state->filter, state->filter_before) != 0) {
    BacaError error = {0};
    if (!library_rebuild_view(state, state->filter_before, state->sort,
                              state->input_selection, &error)) {
      library_runtime_error(state, &error, "cannot restore filter");
      return;
    }
  }
  library_clear_status(state);
  library_finish_input(state);
}

static void library_emit(BacaLibraryTuiState *state, BacaLibraryCommand command,
                         const char *path) {
  char *stored_path = NULL;
  if (path != NULL) {
    BacaError error = {0};
    stored_path = baca_strdup(path, &error);
    if (stored_path == NULL) {
      library_runtime_error(state, &error, "cannot select book");
      return;
    }
  }
  free(state->action.path);
  state->action = (BacaLibraryAction){
      .command = command,
      .path = stored_path,
      .sort = state->sort,
  };
  state->quit = true;
}

static void library_close_picker(BacaLibraryTuiState *state) {
  state->picker_open = false;
  memset(&state->picker_input, 0, sizeof(state->picker_input));
  baca_library_view_free(&state->picker_view);
  baca_library_shelf_free(&state->picker_shelf);
  state->picker_selected = 0U;
  state->picker_top = 0U;
  state->dirty = true;
}

static void library_open_picker(BacaLibraryTuiState *state) {
  memset(&state->picker_input, 0, sizeof(state->picker_input));
  state->picker_selected = 0U;
  state->picker_top = 0U;
  BacaError error = {0};
  if (!library_rebuild_picker(state, library_selected_filepath(state),
                              &error)) {
    library_runtime_error(state, &error, "cannot open book search");
    return;
  }
  library_ensure_picker_selection_visible(state);
  state->picker_open = true;
  state->space_pending = false;
  library_clear_status(state);
  state->dirty = true;
}

static void library_move_picker_selection(BacaLibraryTuiState *state,
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

static void library_handle_picker_key(BacaLibraryTuiState *state, bool key_code,
                                      int code, wchar_t character) {
  if (!key_code && character == 27) {
    library_close_picker(state);
    return;
  }
  if (library_key_is_enter(key_code, code, character)) {
    const char *path = library_picker_selected_filepath(state);
    if (path != NULL && path[0] != '\0') {
      library_emit(state, BACA_LIBRARY_COMMAND_OPEN, path);
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
      baca_text_input_apply(&state->picker_input, key_code, code, character);

  if (changed) {
    const char *selected = library_picker_selected_filepath(state);
    BacaError error = {0};
    if (!library_rebuild_picker(state, selected, &error)) {
      library_runtime_error(state, &error, "cannot search books");
      return;
    }
    library_ensure_picker_selection_visible(state);
  }
  state->dirty = true;
}

static bool library_key_is_enter(bool key_code, int code, wchar_t character) {
  return (key_code && code == KEY_ENTER) ||
         (!key_code && (character == L'\n' || character == L'\r'));
}

static void library_handle_input_key(BacaLibraryTuiState *state, bool key_code,
                                     int code, wchar_t character) {
  if (!key_code && character == 27) {
    library_cancel_input(state);
    return;
  }
  if (library_key_is_enter(key_code, code, character)) {
    if (state->input_kind == BACA_LIBRARY_INPUT_ROOT) {
      if (state->input.length == 0U) {
        library_copy_status(state, "enter a book directory");
        state->dirty = true;
      } else {
        library_emit(state, BACA_LIBRARY_COMMAND_SET_ROOT, state->input.value);
      }
    } else if (state->input_kind == BACA_LIBRARY_INPUT_PATH &&
               state->input.length > 0U) {
      library_emit(state, BACA_LIBRARY_COMMAND_OPEN, state->input.value);
    } else if (state->input_kind == BACA_LIBRARY_INPUT_FILTER &&
               strcmp(state->filter, state->input.value) != 0 &&
               !library_update_filter(state)) {
      return;
    } else {
      library_finish_input(state);
    }
    return;
  }

  const bool changed =
      baca_text_input_apply(&state->input, key_code, code, character);
  if (changed && state->input_kind == BACA_LIBRARY_INPUT_FILTER) {
    (void)library_update_filter(state);
  }
  state->dirty = true;
}

static void library_cycle_sort(BacaLibraryTuiState *state) {
  const char *selected = library_selected_filepath(state);
  const BacaLibrarySort sort = baca_library_sort_next(state->sort);
  BacaError error = {0};
  if (!library_rebuild_view(state, state->filter, sort, selected, &error)) {
    library_runtime_error(state, &error, "cannot sort library");
  } else {
    library_clear_status(state);
  }
}

static void library_clear_filter(BacaLibraryTuiState *state) {
  if (state->filter[0] == '\0') {
    return;
  }
  const char *selected = library_selected_filepath(state);
  BacaError error = {0};
  if (!library_rebuild_view(state, "", state->sort, selected, &error)) {
    library_runtime_error(state, &error, "cannot clear filter");
  } else {
    library_clear_status(state);
  }
}

static void library_change_mode(BacaLibraryTuiState *state,
                                BacaLibraryShelfKind mode, const char *author,
                                size_t format_book) {
  char *stored_author = NULL;
  BacaError error = {0};
  if (author != NULL) {
    stored_author = baca_strdup(author, &error);
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

static void library_open_selected(BacaLibraryTuiState *state) {
  const BacaLibraryShelfReference *reference =
      library_selected_reference(state);
  if (reference != NULL && state->mode == BACA_LIBRARY_SHELF_AUTHORS &&
      reference->book_index < state->catalog->length) {
    library_change_mode(state, BACA_LIBRARY_SHELF_BOOKS,
                        state->catalog->books[reference->book_index].author,
                        0U);
    return;
  }
  const char *path = library_selected_filepath(state);
  if (path == NULL || path[0] == '\0') {
    library_copy_status(state, "book path unavailable");
    state->dirty = true;
  } else {
    library_emit(state, BACA_LIBRARY_COMMAND_OPEN, path);
  }
}

static void library_open_formats(BacaLibraryTuiState *state) {
  const BacaLibraryShelfReference *reference =
      library_selected_reference(state);
  if (reference == NULL || reference->book_index >= state->catalog->length ||
      state->mode == BACA_LIBRARY_SHELF_AUTHORS) {
    library_copy_status(state, "select a book to view formats");
    state->dirty = true;
    return;
  }
  state->format_parent = state->mode;
  library_change_mode(state, BACA_LIBRARY_SHELF_FORMATS, NULL,
                      reference->book_index);
}

static void library_go_back(BacaLibraryTuiState *state) {
  if (state->catalog == NULL) {
    library_clear_filter(state);
  } else if (state->filter[0] != '\0') {
    library_clear_filter(state);
  } else if (state->mode == BACA_LIBRARY_SHELF_FORMATS) {
    const char *author = state->format_book < state->catalog->length
                             ? state->catalog->books[state->format_book].author
                             : NULL;
    const BacaLibraryShelfKind parent =
        state->format_parent == BACA_LIBRARY_SHELF_BOOKS
            ? BACA_LIBRARY_SHELF_BOOKS
            : BACA_LIBRARY_SHELF_ALL;
    library_change_mode(state, parent,
                        parent == BACA_LIBRARY_SHELF_BOOKS ? author : NULL, 0U);
  } else if (state->mode == BACA_LIBRARY_SHELF_BOOKS) {
    library_change_mode(state, BACA_LIBRARY_SHELF_AUTHORS, NULL, 0U);
  } else {
    library_clear_filter(state);
  }
}

static void library_handle_key(BacaLibraryTuiState *state, bool key_code,
                               int code, wchar_t character) {
  if (state->help_open) {
    if ((!key_code &&
         (character == 27 || character == L'?' || character == L'q')) ||
        (key_code && code == KEY_BACKSPACE)) {
      state->help_open = false;
      state->dirty = true;
    } else if ((key_code && code == KEY_DOWN) ||
               (key_code && code == KEY_NPAGE) ||
               (!key_code && character == L'j')) {
      if (state->help_scroll < SIZE_MAX) {
        ++state->help_scroll;
      }
      state->dirty = true;
    } else if ((key_code && code == KEY_UP) ||
               (key_code && code == KEY_PPAGE) ||
               (!key_code && character == L'k')) {
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
  if (state->input_kind != BACA_LIBRARY_INPUT_NONE) {
    library_handle_input_key(state, key_code, code, character);
    return;
  }

  if (!key_code && character == L' ') {
    if (state->space_pending) {
      library_open_picker(state);
    } else {
      state->space_pending = true;
    }
    return;
  }
  state->space_pending = false;

  if (!key_code && character == L'g') {
    if (state->g_pending) {
      state->selected = 0U;
      state->g_pending = false;
      library_ensure_selection_visible(state);
      state->dirty = true;
    } else {
      state->g_pending = true;
    }
    return;
  }
  state->g_pending = false;

  if ((key_code && code == KEY_DOWN) || (!key_code && character == L'j')) {
    library_move_selection(state, 1);
  } else if ((key_code && code == KEY_UP) || (!key_code && character == L'k')) {
    library_move_selection(state, -1);
  } else if (key_code && code == KEY_HOME) {
    state->selected = 0U;
    library_ensure_selection_visible(state);
    state->dirty = true;
  } else if ((key_code && code == KEY_END) ||
             (!key_code && character == L'G')) {
    if (state->view.length > 0U) {
      state->selected = state->view.length - 1U;
      library_ensure_selection_visible(state);
      state->dirty = true;
    }
  } else if ((key_code && code == KEY_NPAGE) || (!key_code && character == 6)) {
    library_move_selection(state, size_to_int(library_visible_rows(state)));
  } else if ((key_code && code == KEY_PPAGE) || (!key_code && character == 2)) {
    library_move_selection(state, -size_to_int(library_visible_rows(state)));
  } else if (library_key_is_enter(key_code, code, character) ||
             (!key_code && character == L'l')) {
    library_open_selected(state);
  } else if (!key_code && character == L'/') {
    library_begin_filter(state);
  } else if (!key_code && character == L's') {
    library_cycle_sort(state);
  } else if (!key_code && character == L'v') {
    state->detail_view = !state->detail_view;
    library_clear_status(state);
    state->dirty = true;
  } else if (!key_code && character == L'?') {
    state->help_open = true;
    state->help_scroll = 0U;
    library_clear_status(state);
    state->dirty = true;
  } else if (!key_code && character == L'r') {
    library_emit(state, BACA_LIBRARY_COMMAND_REFRESH,
                 library_selected_filepath(state));
  } else if (!key_code && character == L'o') {
    library_begin_path(state);
  } else if (!key_code && character == L'f' && state->catalog != NULL) {
    library_open_formats(state);
  } else if (!key_code && character == L'a' && state->catalog != NULL) {
    library_change_mode(state,
                        state->mode == BACA_LIBRARY_SHELF_ALL
                            ? BACA_LIBRARY_SHELF_AUTHORS
                            : BACA_LIBRARY_SHELF_ALL,
                        NULL, 0U);
  } else if ((key_code && code == KEY_BACKSPACE) ||
             (!key_code && (character == 8 || character == 127 ||
                            character == 27 || character == L'h'))) {
    library_go_back(state);
  } else if (!key_code && character == L'q') {
    library_emit(state, BACA_LIBRARY_COMMAND_QUIT, NULL);
  }
}

int baca_library_tui_run(const BacaConfig *config, const BacaHistory *history,
                         BacaCatalog *catalog, bool setup_required,
                         BacaLibrarySort sort, const char *selected_filepath,
                         const char *context, BacaLibraryAction *action,
                         BacaError *error) {
  if (config == NULL || history == NULL || action == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid library application state");
    return EXIT_FAILURE;
  }
  if (action->command != BACA_LIBRARY_COMMAND_NONE || action->path != NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "library action output is not empty");
    return EXIT_FAILURE;
  }

  BacaLibraryTuiState state = {
      .config = config,
      .history = history,
      .catalog = catalog,
      .mode = BACA_LIBRARY_SHELF_ALL,
      .format_parent = BACA_LIBRARY_SHELF_ALL,
      .sort = sort,
      .cover_book_index = SIZE_MAX,
      .setup_required = setup_required,
      .previous_cursor = ERR,
      .dirty = true,
  };
  if (!library_rebuild_view(&state, "", sort, selected_filepath, error)) {
    library_clear_status(&state);
    baca_library_shelf_free(&state.shelf);
    return EXIT_FAILURE;
  }
  if (state.setup_required) {
    library_begin_root(&state);
  }
  library_copy_status(&state, context);

  if (!baca_terminal_runtime_begin(error)) {
    library_clear_status(&state);
    baca_library_view_free(&state.view);
    baca_library_shelf_free(&state.shelf);
    return EXIT_FAILURE;
  }
  if (!library_initialize_curses(&state, error)) {
    library_clear_status(&state);
    baca_library_view_free(&state.view);
    baca_library_shelf_free(&state.shelf);
    return baca_terminal_runtime_finish(EXIT_FAILURE, error);
  }
  library_initialize_graphics(&state);

  int result = EXIT_SUCCESS;
  if (!baca_terminal_runtime_activate(error)) {
    result = EXIT_FAILURE;
    goto cleanup;
  }
  while (!state.quit) {
    if (baca_terminal_runtime_interrupted()) {
      break;
    }
    if (state.dirty) {
      library_draw_frame(&state);
    }
    wtimeout(stdscr, 100);
    wint_t input = 0;
    const int status = wget_wch(stdscr, &input);
    if (baca_terminal_runtime_interrupted()) {
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
  if (state.graphics != NULL && state.image_mode == BACA_IMAGE_MODE_KITTY) {
    BacaError ignored = {0};
    (void)fflush(stdout);
    (void)baca_graphics_kitty_delete_all(
        state.graphics, baca_terminal_graphics_write, &state, &ignored);
  }
  library_clear_cover(&state);
  baca_graphics_free(state.graphics);
  state.graphics = NULL;
  if (state.previous_cursor != ERR) {
    (void)curs_set(state.previous_cursor);
  }
  (void)endwin();
  free(state.input_selection);
  baca_library_view_free(&state.picker_view);
  baca_library_shelf_free(&state.picker_shelf);
  library_clear_status(&state);
  baca_library_view_free(&state.view);
  baca_library_shelf_free(&state.shelf);
  free(state.author);
  result = baca_terminal_runtime_finish(result, error);
  if (result == EXIT_SUCCESS) {
    *action = state.action;
  } else {
    free(state.action.path);
  }
  return result;
}
