#include "baca/app.h"
#include "baca/library_tui.h"
#include "baca/platform.h"
#include "baca/remote.h"
#include "baca/state_migration.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static double normalized_progress(double progress) {
  if (!isfinite(progress) || progress < 0.0) {
    return 0.0;
  }
  if (progress > 1.0) {
    return 1.0;
  }
  return progress;
}

static bool save_progress(BacaApp *app, BacaError *error) {
  baca_error_clear(error);
  if (app->database.handle == NULL || app->source == NULL ||
      app->document.format == BACA_FORMAT_UNKNOWN) {
    return true;
  }

  char *last_read = baca_now_iso8601(error);
  if (last_read == NULL) {
    return false;
  }
  const BacaHistoryEntry entry = {
      .filepath = app->source,
      .title = app->document.metadata.title,
      .author = app->document.metadata.author != NULL
                    ? app->document.metadata.author
                    : app->document.metadata.creator,
      .reading_progress = normalized_progress(app->saved_progress),
      .last_read = last_read,
  };
  const bool saved =
      baca_database_save_progress(&app->database, &entry, error);
  free(last_read);
  return saved;
}

static void dispose_app(BacaApp *app) {
  if (app == NULL) {
    return;
  }
  baca_layout_free(&app->layout);
  baca_document_close(&app->document);
  baca_database_close(&app->database);
  baca_config_free(&app->config);
  free(app->source);
  memset(app, 0, sizeof(*app));
}

bool baca_app_init(BacaApp *app, const char *path, bool direct_open,
                   BacaError *error) {
  if (app == NULL || path == NULL || path[0] == '\0') {
    baca_error_set(error, BACA_ERROR_ARGUMENT, "missing document path");
    return false;
  }

  baca_error_clear(error);
  memset(app, 0, sizeof(*app));
  app->dark_mode = true;
  app->direct_open = direct_open;

  BacaRemoteFile remote = {0};
  const char *document_path = path;
  if (baca_remote_is_url(path)) {
    if (!baca_remote_fetch(path, &remote, error)) {
      dispose_app(app);
      return false;
    }
    app->source = remote.url;
    remote.url = NULL;
    document_path = remote.path;
  }

  if (!baca_config_load(&app->config, error) ||
      !baca_database_open_default(&app->database, error) ||
      !baca_database_migrate(&app->database, error) ||
      !baca_document_open(&app->document, document_path, error)) {
    baca_remote_file_free(&remote);
    dispose_app(app);
    return false;
  }
  baca_remote_file_free(&remote);
  if (app->source == NULL) {
    app->source = baca_strdup(app->document.path, error);
    if (app->source == NULL) {
      dispose_app(app);
      return false;
    }
  }

  BacaHistory history = {0};
  if (!baca_database_history(&app->database, false, &history, error)) {
    dispose_app(app);
    return false;
  }
  for (size_t index = 0; index < history.length; ++index) {
    const BacaHistoryEntry *entry = &history.items[index];
    if (entry->filepath != NULL &&
        strcmp(entry->filepath, app->source) == 0) {
      app->saved_progress = normalized_progress(entry->reading_progress);
      break;
    }
  }
  baca_history_free(&history);
  return true;
}

bool baca_app_free(BacaApp *app, BacaError *error) {
  if (app == NULL) {
    baca_error_clear(error);
    return true;
  }
  const bool saved = save_progress(app, error);
  dispose_app(app);
  return saved;
}

int baca_app_run(BacaApp *app, BacaError *error) {
  if (app == NULL || app->document.path == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "application is not initialized");
    return EXIT_FAILURE;
  }
  return baca_tui_run(app, error);
}

static void print_help(FILE *stream) {
  fprintf(stream,
          "usage: %s [-h] [-v] [-d] [-r] [PATH | URL | # | PATTERN ...]\n"
          "\n"
          "TUI ebook reader and library\n"
          "\n"
          "options:\n"
          "  -h, --help       print help and exit\n"
          "  -v, --version    print version and exit\n"
          "  -d, --doctor     print runtime diagnostics and exit\n"
          "  -r, --history    print reading history and exit\n"
          "\n"
          "examples:\n"
          "  %s\n"
          "  %s /path/to/ebook.epub\n"
          "  %s https://example.com/ebook.epub\n"
          "  %s 3\n"
          "  %s count monte\n",
          BACA_NAME, BACA_NAME, BACA_NAME, BACA_NAME, BACA_NAME, BACA_NAME);
}

static int report_error(const BacaError *error) {
  const char *message = "operation failed";
  if (error != NULL && error->message[0] != '\0') {
    message = error->message;
  }
  fprintf(stderr, "%s: %s\n", BACA_NAME, message);
  return EXIT_FAILURE;
}

static int print_doctor(void) {
  BacaError error = {0};
  char *config = baca_xdg_config_path("config.ini", &error);
  char *database =
      config == NULL ? NULL : baca_xdg_cache_path(BACA_NAME ".db", &error);
  char *downloads = database == NULL ? NULL : baca_xdg_cache_path("downloads", &error);
  if (config == NULL || database == NULL || downloads == NULL) {
    free(config);
    free(database);
    free(downloads);
    return report_error(&error);
  }

  const char *mobitool = baca_platform_find_executable("mobitool");
  printf("%s Doctor\n", BACA_NAME);
  printf("Version: %s\n", BACA_VERSION);
  printf("Config: %s\n", config);
  printf("Database: %s\n", database);
  printf("Downloads: %s\n", downloads);
  printf("MOBI/AZW: %s\n",
         mobitool == NULL ? "unavailable (mobitool not found in PATH)"
                          : mobitool);
  puts("Core formats: ready");
  free(config);
  free(database);
  free(downloads);
  return EXIT_SUCCESS;
}

static double history_progress(double progress) {
  return normalized_progress(progress) * 100.0;
}

static void print_history_field(const char *value) {
  if (value == NULL) {
    return;
  }
  for (size_t index = 0U; value[index] != '\0'; ++index) {
    const unsigned char character = (unsigned char)value[index];
    if (character < 0x20U || character == 0x7fU) {
      (void)putchar(' ');
    } else if (character == 0xc2U &&
               (unsigned char)value[index + 1U] >= 0x80U &&
               (unsigned char)value[index + 1U] <= 0x9fU) {
      (void)putchar(' ');
      ++index;
    } else {
      (void)putchar((int)character);
    }
  }
}

static void format_file_size(const char *path, char output[32]) {
  struct stat status = {0};
  if (path == NULL || stat(path, &status) != 0) {
    (void)snprintf(output, 32U, "?");
    return;
  }

  const double bytes = (double)status.st_size;
  if (bytes < 1024.0 * 1024.0) {
    (void)snprintf(output, 32U, "%.2f kb", bytes / 1024.0);
  } else {
    (void)snprintf(output, 32U, "%.2f mb", bytes / (1024.0 * 1024.0));
  }
}

static bool open_history_database(BacaDatabase *database, BacaError *error) {
  return baca_database_open_default(database, error) &&
         baca_database_migrate(database, error);
}

static int print_history(BacaError *error) {
  BacaDatabase database = {0};
  BacaHistory history = {0};
  if (!open_history_database(&database, error) ||
      !baca_database_history(&database, true, &history, error)) {
    baca_history_free(&history);
    baca_database_close(&database);
    return report_error(error);
  }

  printf("%s History\n", BACA_NAME);
  puts("#\tLast Read\tProgress\tTitle\tAuthor\tPath\tSize");
  for (size_t index = 0; index < history.length; ++index) {
    const BacaHistoryEntry *entry = &history.items[index];
    char size[32] = {0};
    format_file_size(entry->filepath, size);
    printf("%zu\t", index + 1U);
    print_history_field(entry->last_read);
    printf("\t%.2f%%\t", history_progress(entry->reading_progress));
    print_history_field(entry->title);
    (void)putchar('\t');
    print_history_field(entry->author);
    (void)putchar('\t');
    print_history_field(entry->filepath);
    (void)putchar('\t');
    print_history_field(size);
    (void)putchar('\n');
  }

  baca_history_free(&history);
  baca_database_close(&database);
  return EXIT_SUCCESS;
}

static bool is_decimal_string(const char *value) {
  if (value == NULL || value[0] == '\0') {
    return false;
  }
  for (const unsigned char *cursor = (const unsigned char *)value;
       *cursor != '\0'; ++cursor) {
    if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
      return false;
    }
  }
  return true;
}

static bool parse_history_number(const char *value, size_t *number) {
  if (!is_decimal_string(value)) {
    return false;
  }

  errno = 0;
  char *end = NULL;
  const uintmax_t parsed = strtoumax(value, &end, 10);
  if (errno == ERANGE || end == value || *end != '\0' || parsed == 0U ||
      parsed > (uintmax_t)SIZE_MAX) {
    return false;
  }
  *number = (size_t)parsed;
  return true;
}

static char *join_pattern(char **arguments, int count, BacaError *error) {
  BacaString pattern = {0};
  for (int index = 0; index < count; ++index) {
    if (index > 0 && !baca_string_append_char(&pattern, ' ', error)) {
      baca_string_free(&pattern);
      return NULL;
    }
    if (!baca_string_append(&pattern, arguments[index], error)) {
      baca_string_free(&pattern);
      return NULL;
    }
  }
  return baca_string_take(&pattern);
}

static char *resolve_history_path(char **arguments, int count,
                                  BacaError *error) {
  BacaDatabase database = {0};
  BacaHistory history = {0};
  char *result = NULL;

  if (!open_history_database(&database, error) ||
      !baca_database_history(&database, true, &history, error)) {
    goto cleanup;
  }

  const BacaHistoryEntry *entry = NULL;
  if (count == 0) {
    entry = baca_history_nth(&history, 1U);
    if (entry == NULL) {
      baca_error_set(error, BACA_ERROR_NOT_FOUND, "no reading history");
    }
  } else {
    size_t number = 0U;
    if (count == 1 && is_decimal_string(arguments[0])) {
      if (!parse_history_number(arguments[0], &number)) {
        baca_error_set(error, BACA_ERROR_ARGUMENT,
                       "history number must be a positive 1-based integer");
      } else {
        entry = baca_history_nth(&history, number);
        if (entry == NULL) {
          baca_error_set(error, BACA_ERROR_NOT_FOUND,
                         "history entry #%zu not found", number);
        }
      }
    } else {
      char *pattern = join_pattern(arguments, count, error);
      if (pattern != NULL) {
        entry = baca_history_best_match(&history, pattern);
        if (entry == NULL) {
          baca_error_set(error, BACA_ERROR_NOT_FOUND,
                         "no history entry matches '%s'", pattern);
        }
      }
      free(pattern);
    }
  }

  if (entry != NULL && entry->filepath != NULL) {
    result = baca_strdup(entry->filepath, error);
  }

cleanup:
  baca_history_free(&history);
  baca_database_close(&database);
  return result;
}

static int run_library_reader(const char *path, char context[512], bool *opened) {
  if (opened != NULL) {
    *opened = false;
  }
  BacaApp app = {0};
  BacaError error = {0};
  if (!baca_app_init(&app, path, false, &error)) {
    (void)snprintf(context, 512U, "cannot open: %.490s", error.message);
    return EXIT_SUCCESS;
  }
  const int result = baca_app_run(&app, &error);
  BacaError save_error = {0};
  const bool saved = baca_app_free(&app, &save_error);
  if (result >= 128) {
    if (!saved) {
      (void)report_error(&save_error);
    }
    return result;
  }
  if (!saved) {
    (void)snprintf(context, 512U, "progress not saved: %.487s",
                   save_error.message);
  } else if (result != EXIT_SUCCESS) {
    (void)snprintf(context, 512U, "reader failed: %.492s",
                   error.message[0] == '\0' ? "operation failed"
                                            : error.message);
  } else {
    context[0] = '\0';
  }
  if (opened != NULL && result == EXIT_SUCCESS) {
    *opened = true;
  }
  return EXIT_SUCCESS;
}

static bool apply_library_format_preferences(BacaDatabase *database,
                                             BacaCatalog *catalog,
                                             const char *library_root,
                                             BacaError *error) {
  BacaFormatPreferences preferences = {0};
  if (!baca_database_format_preferences(database, library_root, &preferences,
                                        error)) {
    return false;
  }
  const bool applied =
      baca_catalog_apply_format_preferences(catalog, &preferences, error);
  baca_format_preferences_free(&preferences);
  return applied;
}

static bool resolve_library_selection(const BacaHistory *history,
                                      BacaLibrarySort sort,
                                      const char *filepath,
                                      size_t fallback_index,
                                      const char **selected_filepath,
                                      size_t *selected_index,
                                      BacaError *error) {
  BacaLibraryView view = {0};
  if (!baca_library_view_build(&view, history, "", sort, error)) {
    return false;
  }
  const size_t index =
      baca_library_preserve_selection(&view, filepath, fallback_index);
  if (selected_filepath != NULL) {
    *selected_filepath =
        view.length == 0U ? NULL : view.rows[index].entry->filepath;
  }
  if (selected_index != NULL) {
    *selected_index = index;
  }
  baca_library_view_free(&view);
  return true;
}

static bool library_directory(const char *path) {
  struct stat status;
  return path != NULL && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool library_contains_path(const char *root, const char *path) {
  if (root == NULL || path == NULL) {
    return false;
  }
  const size_t root_length = strlen(root);
  return strncmp(root, path, root_length) == 0 &&
         (path[root_length] == '\0' || path[root_length] == '/');
}

static char *resolve_library_root(const BacaConfig *config, BacaError *error) {
  const char *override = getenv("MEREADER_TUI_LIBRARY_PATH");
  const char *setting = override != NULL && override[0] != '\0'
                            ? override
                            : config->library_path;
  if (setting != NULL && baca_casecmp(setting, "auto") != 0) {
    char *root = baca_realpath(setting, error);
    if (root != NULL && !library_directory(root)) {
      baca_error_set(error, BACA_ERROR_NOT_FOUND,
                     "library path is not a directory: %s", setting);
      free(root);
      return NULL;
    }
    return root;
  }

  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    return NULL;
  }
  static const char *candidates[] = {"Calibre Library",
                                     "Documents/Calibre Library"};
  for (size_t index = 0U; index < BACA_ARRAY_LEN(candidates); ++index) {
    char *candidate = baca_path_join(home, candidates[index], error);
    if (candidate == NULL) {
      return NULL;
    }
    if (library_directory(candidate)) {
      char *root = baca_realpath(candidate, error);
      free(candidate);
      return root;
    }
    free(candidate);
  }
  return NULL;
}

static int run_library(void) {
  BacaConfig config = {0};
  BacaDatabase database = {0};
  BacaError error = {0};
  if (!baca_config_load(&config, &error) ||
      !baca_database_open_default(&database, &error) ||
      !baca_database_migrate(&database, &error)) {
    baca_database_close(&database);
    baca_config_free(&config);
    return report_error(&error);
  }

  char *library_root = resolve_library_root(&config, &error);
  if (library_root == NULL && baca_error_is_set(&error)) {
    baca_database_close(&database);
    baca_config_free(&config);
    return report_error(&error);
  }
  BacaCatalog catalog = {0};

  BacaLibrarySort sort = BACA_LIBRARY_SORT_RECENT;
  char *selected_filepath = NULL;
  size_t selected_index = 0U;
  bool preserve_index = false;
  char context[512] = {0};
  bool remove_missing = false;
  int result = EXIT_SUCCESS;
  for (;;) {
    BacaHistory history = {0};
    if (!baca_database_history(&database, remove_missing, &history, &error)) {
      result = report_error(&error);
      break;
    }
    remove_missing = false;

    if (library_root != NULL) {
      bool catalog_ready = false;
      if (!baca_catalog_is_open(&catalog)) {
        catalog_ready =
            baca_catalog_open(&catalog, library_root, &history, true, &error) &&
            apply_library_format_preferences(&database, &catalog, library_root,
                                             &error);
      } else {
        catalog_ready = baca_catalog_update_progress(&catalog, &history, &error);
      }
      if (!catalog_ready) {
        baca_history_free(&history);
        result = report_error(&error);
        break;
      }
    }

    const char *tui_selected_filepath = selected_filepath;
    if (preserve_index && !baca_catalog_is_open(&catalog) &&
        !resolve_library_selection(&history, sort, selected_filepath,
                                   selected_index, &tui_selected_filepath,
                                   &selected_index, &error)) {
      baca_history_free(&history);
      result = report_error(&error);
      break;
    }

    BacaLibraryAction action = {0};
    result = baca_library_tui_run(&config, &history,
                                  baca_catalog_is_open(&catalog) ? &catalog : NULL,
                                  library_root == NULL && history.length == 0U,
                                  sort,
                                  tui_selected_filepath, context, &action,
                                  &error);
    size_t refresh_index = selected_index;
    if (result == EXIT_SUCCESS &&
        action.command == BACA_LIBRARY_COMMAND_REFRESH &&
        !baca_catalog_is_open(&catalog) &&
        !resolve_library_selection(&history, action.sort, action.path,
                                   selected_index, NULL, &refresh_index,
                                   &error)) {
      free(action.path);
      baca_history_free(&history);
      result = report_error(&error);
      break;
    }
    if (result == EXIT_SUCCESS &&
        action.command == BACA_LIBRARY_COMMAND_REFRESH &&
        baca_catalog_is_open(&catalog) &&
        (!baca_catalog_refresh(&catalog, &history, &error) ||
         !apply_library_format_preferences(&database, &catalog, library_root,
                                           &error))) {
      free(action.path);
      baca_history_free(&history);
      result = report_error(&error);
      break;
    }
    baca_history_free(&history);
    context[0] = '\0';
    if (result != EXIT_SUCCESS) {
      free(action.path);
      if (result < 128 && baca_error_is_set(&error)) {
        (void)report_error(&error);
      }
      break;
    }

    sort = action.sort;
    if (action.command == BACA_LIBRARY_COMMAND_QUIT) {
      free(action.path);
      result = EXIT_SUCCESS;
      break;
    }
    if (action.command == BACA_LIBRARY_COMMAND_REFRESH) {
      free(selected_filepath);
      selected_filepath = action.path;
      selected_index = refresh_index;
      preserve_index = !baca_catalog_is_open(&catalog);
      remove_missing = !baca_catalog_is_open(&catalog);
      (void)snprintf(context, sizeof(context), "library refreshed");
      continue;
    }
    if (action.command == BACA_LIBRARY_COMMAND_SET_ROOT) {
      BacaError path_error = {0};
      char *resolved_root = baca_realpath(action.path, &path_error);
      if (resolved_root == NULL || !library_directory(resolved_root)) {
        (void)snprintf(context, sizeof(context), "cannot use library: %.488s",
                       resolved_root == NULL ? path_error.message
                                             : "path is not a directory");
        free(resolved_root);
        free(action.path);
        continue;
      }
      if (!baca_config_save_library_path(resolved_root, &path_error)) {
        (void)snprintf(context, sizeof(context), "library not saved: %.491s",
                       path_error.message);
        free(resolved_root);
        free(action.path);
        continue;
      }
      free(library_root);
      library_root = resolved_root;
      free(selected_filepath);
      selected_filepath = NULL;
      preserve_index = false;
      free(action.path);
      (void)snprintf(context, sizeof(context), "library added");
      continue;
    }
    if (action.command != BACA_LIBRARY_COMMAND_OPEN || action.path == NULL) {
      free(action.path);
      baca_error_set(&error, BACA_ERROR_INTERNAL,
                     "library returned no command");
      result = report_error(&error);
      break;
    }

    char *requested_path = action.path;
    if (baca_remote_is_url(requested_path)) {
      free(selected_filepath);
      selected_filepath = requested_path;
      result = run_library_reader(requested_path, context, NULL);
      if (result != EXIT_SUCCESS) {
        break;
      }
      continue;
    }
    BacaError path_error = {0};
    char *resolved_path = baca_realpath(requested_path, &path_error);
    if (resolved_path == NULL) {
      free(selected_filepath);
      selected_filepath = requested_path;
      (void)snprintf(context, sizeof(context), "cannot open: %.490s",
                     path_error.message);
      continue;
    }
    if (baca_catalog_is_open(&catalog) &&
        !library_contains_path(library_root, resolved_path)) {
      free(resolved_path);
      free(selected_filepath);
      selected_filepath = requested_path;
      (void)snprintf(context, sizeof(context),
                     "cannot open: selected path leaves the library root");
      continue;
    }
    free(requested_path);
    free(selected_filepath);
    selected_filepath = resolved_path;
    bool opened = false;
    result = run_library_reader(resolved_path, context, &opened);
    if (opened && baca_catalog_is_open(&catalog)) {
      const char *book_key = NULL;
      const char *format_relative_path = NULL;
      if (baca_catalog_prefer_path(&catalog, resolved_path, &book_key,
                                   &format_relative_path) &&
          !baca_database_save_format_preference(
              &database, library_root, book_key, format_relative_path, &error)) {
        (void)snprintf(context, sizeof(context),
                       "format preference not saved: %.478s", error.message);
        baca_error_clear(&error);
      }
    }
    if (result != EXIT_SUCCESS) {
      break;
    }
  }

  free(selected_filepath);
  baca_catalog_close(&catalog);
  free(library_root);
  baca_database_close(&database);
  baca_config_free(&config);
  return result;
}

int baca_cli_main(int argc, char **argv) {
  static const struct option options[] = {
      {"help", no_argument, NULL, 'h'},
      {"version", no_argument, NULL, 'v'},
      {"doctor", no_argument, NULL, 'd'},
      {"history", no_argument, NULL, 'r'},
      {NULL, 0, NULL, 0},
  };

  bool show_history = false;
  bool show_doctor = false;
  opterr = 0;
  optind = 0;
  for (;;) {
    const int option = getopt_long(argc, argv, "hvdr", options, NULL);
    if (option == -1) {
      break;
    }
    switch (option) {
    case 'h':
      print_help(stdout);
      return EXIT_SUCCESS;
    case 'v':
      printf("v%s\n", BACA_VERSION);
      return EXIT_SUCCESS;
    case 'd':
      show_doctor = true;
      break;
    case 'r':
      show_history = true;
      break;
    default:
      fprintf(stderr, "%s: unknown option\n", BACA_NAME);
      return EXIT_FAILURE;
    }
  }

  BacaError error = {0};
  if (!baca_state_migrate(&error)) {
    return report_error(&error);
  }
  if (show_doctor) {
    return print_doctor();
  }
  if (show_history) {
    return print_history(&error);
  }

  const int argument_count = argc - optind;
  char **arguments = argv + optind;
  if (argument_count == 0) {
    return run_library();
  }
  bool direct_open = false;
  bool history_lookup = false;
  char *path = NULL;

  if (argument_count == 1) {
    if (baca_remote_is_url(arguments[0])) {
      path = baca_strdup(arguments[0], &error);
      direct_open = path != NULL;
    } else if (!is_decimal_string(arguments[0]) && baca_file_exists(arguments[0])) {
      path = baca_realpath(arguments[0], &error);
      direct_open = path != NULL;
    }
  }
  if (path == NULL && !baca_error_is_set(&error)) {
    history_lookup = true;
    path = resolve_history_path(arguments, argument_count, &error);
  }
  if (path == NULL) {
    if (history_lookup && argument_count > 0 &&
        (error.code == BACA_ERROR_NOT_FOUND ||
         error.code == BACA_ERROR_ARGUMENT)) {
      BacaError history_error = {0};
      (void)print_history(&history_error);
      (void)fflush(stdout);
    }
    return report_error(&error);
  }

  BacaApp app = {0};
  if (!baca_app_init(&app, path, direct_open, &error)) {
    free(path);
    return report_error(&error);
  }
  free(path);

  const int result = baca_app_run(&app, &error);
  BacaError save_error = {0};
  const bool saved = baca_app_free(&app, &save_error);
  if (result != EXIT_SUCCESS && baca_error_is_set(&error)) {
    (void)report_error(&error);
  }
  if (!saved) {
    (void)report_error(&save_error);
  }
  if (result == EXIT_SUCCESS && !saved) {
    return EXIT_FAILURE;
  }
  return result;
}
