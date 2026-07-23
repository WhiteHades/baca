#include "test_support.h"

#include "baca/database.h"
#include "baca/state_migration.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_IMPORT_MARKER ".mereader-tui-legacy-state-import-v1.complete"
#define STATE_IMPORT_MARKER_CONTENTS                                           \
  "mereader-tui legacy state import v1 complete\n"

typedef struct StateMigrationEnvironment {
  char *config;
  char *cache;
  char *previous_config;
  char *previous_cache;
  bool had_previous_config;
  bool had_previous_cache;
} StateMigrationEnvironment;

static void state_environment_restore(StateMigrationEnvironment *environment) {
  if (environment->had_previous_config) {
    (void)setenv("XDG_CONFIG_HOME", environment->previous_config, 1);
  } else {
    (void)unsetenv("XDG_CONFIG_HOME");
  }
  if (environment->had_previous_cache) {
    (void)setenv("XDG_CACHE_HOME", environment->previous_cache, 1);
  } else {
    (void)unsetenv("XDG_CACHE_HOME");
  }
  free(environment->config);
  free(environment->cache);
  free(environment->previous_config);
  free(environment->previous_cache);
  *environment = (StateMigrationEnvironment){0};
}

static bool state_environment_init(const char *name,
                                   StateMigrationEnvironment *environment) {
  const char *previous_config = getenv("XDG_CONFIG_HOME");
  const char *previous_cache = getenv("XDG_CACHE_HOME");
  environment->had_previous_config = previous_config != NULL;
  environment->had_previous_cache = previous_cache != NULL;
  environment->previous_config =
      previous_config == NULL ? NULL : strdup(previous_config);
  environment->previous_cache =
      previous_cache == NULL ? NULL : strdup(previous_cache);
  if ((previous_config != NULL && environment->previous_config == NULL) ||
      (previous_cache != NULL && environment->previous_cache == NULL)) {
    free(environment->previous_config);
    free(environment->previous_cache);
    *environment = (StateMigrationEnvironment){0};
    return false;
  }

  BacaError error = {0};
  char *root = baca_test_path("state-migration");
  char *case_root = root == NULL ? NULL : baca_path_join(root, name, &error);
  environment->config =
      case_root == NULL ? NULL : baca_path_join(case_root, "config", &error);
  environment->cache =
      case_root == NULL ? NULL : baca_path_join(case_root, "cache", &error);
  const bool ready = environment->config != NULL &&
                     environment->cache != NULL &&
                     baca_mkdirs(environment->config, &error) &&
                     baca_mkdirs(environment->cache, &error) &&
                     setenv("XDG_CONFIG_HOME", environment->config, 1) == 0 &&
                     setenv("XDG_CACHE_HOME", environment->cache, 1) == 0;
  if (!ready) {
    fprintf(stderr, "state migration fixture: %s\n", error.message);
    state_environment_restore(environment);
  }
  free(root);
  free(case_root);
  return ready;
}

static char *state_path(const char *root, const char *relative) {
  BacaError error = {0};
  return baca_path_join(root, relative, &error);
}

static bool state_write(const char *root, const char *relative,
                        const char *contents) {
  char *path = state_path(root, relative);
  if (path == NULL) {
    return false;
  }
  BacaError error = {0};
  char *directory = baca_path_dirname(path, &error);
  const bool written =
      directory != NULL && baca_mkdirs(directory, &error) &&
      baca_write_file(path, contents, strlen(contents), &error);
  free(directory);
  free(path);
  return written;
}

static bool state_path_exists(const char *path) {
  struct stat status = {0};
  return lstat(path, &status) == 0;
}

static bool state_regular_file(const char *path) {
  struct stat status = {0};
  return lstat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static bool state_directory_mode(const char *path, mode_t expected) {
  struct stat status = {0};
  return stat(path, &status) == 0 && S_ISDIR(status.st_mode) &&
         (status.st_mode & 0777) == expected;
}

static bool state_contents_equal(const char *path, const char *expected) {
  BacaBuffer contents = {0};
  BacaError error = {0};
  const bool read = baca_read_file(path, &contents, &error);
  const bool equal = read && contents.length == strlen(expected) &&
                     memcmp(contents.data, expected, contents.length) == 0;
  baca_buffer_free(&contents);
  return equal;
}

static bool state_database_create(const char *root, const char *relative,
                                  const char *identity, bool wal,
                                  BacaDatabase *database) {
  BacaError error = {0};
  char *path = state_path(root, relative);
  char *directory = path == NULL ? NULL : baca_path_dirname(path, &error);
  bool created = path != NULL && directory != NULL &&
                 baca_mkdirs(directory, &error) &&
                 baca_database_open(database, path, &error);
  if (created && wal) {
    created =
        sqlite3_exec(database->handle,
                     "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0",
                     NULL, NULL, NULL) == SQLITE_OK;
  }
  if (created) {
    created = baca_database_migrate(database, &error);
  }

  char filepath[128] = {0};
  char library_root[128] = {0};
  char book_key[128] = {0};
  char relative_path[128] = {0};
  if (created) {
    created =
        snprintf(filepath, sizeof(filepath), "%s-book.epub", identity) > 0 &&
        snprintf(library_root, sizeof(library_root), "%s-library", identity) >
            0 &&
        snprintf(book_key, sizeof(book_key), "%s-key", identity) > 0 &&
        snprintf(relative_path, sizeof(relative_path), "%s/book.epub",
                 identity) > 0;
  }
  BacaHistoryEntry entry = {
      .filepath = filepath,
      .title = "Imported title",
      .author = "Imported author",
      .reading_progress = 0.625,
      .last_read = "2026-07-20 12:34:56",
  };
  if (created) {
    created = baca_database_save_progress(database, &entry, &error) &&
              baca_database_add_bookmark(database, filepath, 0.375, &error) &&
              baca_database_save_format_preference(
                  database, library_root, book_key, relative_path, &error);
  }
  if (!created) {
    fprintf(stderr, "state database fixture: %s\n",
            baca_error_is_set(&error) ? error.message
                                      : sqlite3_errmsg(database->handle));
    baca_database_close(database);
  }
  free(directory);
  free(path);
  return created;
}

static bool state_database_has_identity(const char *path,
                                        const char *identity) {
  BacaDatabase database = {0};
  BacaError error = {0};
  if (!baca_database_open(&database, path, &error)) {
    fprintf(stderr, "state database query: %s\n", error.message);
    return false;
  }

  char filepath[128] = {0};
  char library_root[128] = {0};
  char book_key[128] = {0};
  char relative_path[128] = {0};
  bool valid =
      snprintf(filepath, sizeof(filepath), "%s-book.epub", identity) > 0 &&
      snprintf(library_root, sizeof(library_root), "%s-library", identity) >
          0 &&
      snprintf(book_key, sizeof(book_key), "%s-key", identity) > 0 &&
      snprintf(relative_path, sizeof(relative_path), "%s/book.epub", identity) >
          0;

  BacaHistory history = {0};
  BacaBookmarks bookmarks = {0};
  BacaFormatPreferences preferences = {0};
  valid = valid && baca_database_history(&database, false, &history, &error) &&
          history.length == 1U &&
          strcmp(history.items[0].filepath, filepath) == 0 &&
          baca_database_bookmarks(&database, filepath, &bookmarks, &error) &&
          bookmarks.length == 1U &&
          baca_database_format_preferences(&database, library_root,
                                           &preferences, &error) &&
          preferences.length == 1U &&
          strcmp(preferences.items[0].book_key, book_key) == 0 &&
          strcmp(preferences.items[0].relative_path, relative_path) == 0;
  if (!valid && baca_error_is_set(&error)) {
    fprintf(stderr, "state database query: %s\n", error.message);
  }
  baca_history_free(&history);
  baca_bookmarks_free(&bookmarks);
  baca_format_preferences_free(&preferences);
  baca_database_close(&database);
  return valid;
}

static BacaTestResult test_old_only_config(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("config", &environment));
  TEST_ASSERT(
      state_write(environment.config, "baca/config.ini", "legacy config\n"));
  char *old_path = state_path(environment.config, "baca/config.ini");
  char *new_path = state_path(environment.config, "mereader-tui/config.ini");
  char *marker = state_path(environment.config, STATE_IMPORT_MARKER);
  char *old_marker = state_path(environment.cache,
                                "mereader-tui/legacy-state-import-v1.complete");
  TEST_ASSERT(old_path != NULL && new_path != NULL && marker != NULL &&
              old_marker != NULL);
  TEST_ASSERT(chmod(old_path, 0640) == 0);

  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  struct stat status = {0};
  TEST_ASSERT(state_contents_equal(old_path, "legacy config\n"));
  TEST_ASSERT(state_contents_equal(new_path, "legacy config\n"));
  TEST_ASSERT(stat(new_path, &status) == 0 && (status.st_mode & 0777) == 0600);
  TEST_ASSERT(state_regular_file(marker));
  TEST_ASSERT(!state_path_exists(old_marker));
  free(old_path);
  free(new_path);
  free(marker);
  free(old_marker);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_database_snapshot_includes_wal_state(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("database", &environment));
  BacaDatabase legacy = {0};
  TEST_ASSERT(state_database_create(environment.cache, "baca/baca.db", "legacy",
                                    true, &legacy));
  char *old_path = state_path(environment.cache, "baca/baca.db");
  char *old_wal = state_path(environment.cache, "baca/baca.db-wal");
  char *old_shm = state_path(environment.cache, "baca/baca.db-shm");
  char *new_path =
      state_path(environment.cache, "mereader-tui/mereader-tui.db");
  char *new_wal =
      state_path(environment.cache, "mereader-tui/mereader-tui.db-wal");
  char *new_shm =
      state_path(environment.cache, "mereader-tui/mereader-tui.db-shm");
  TEST_ASSERT(old_path != NULL && old_wal != NULL && old_shm != NULL &&
              new_path != NULL && new_wal != NULL && new_shm != NULL);
  TEST_ASSERT(state_regular_file(old_wal) && state_regular_file(old_shm));

  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  TEST_ASSERT(state_regular_file(old_path));
  TEST_ASSERT(state_regular_file(old_wal));
  TEST_ASSERT(state_regular_file(old_shm));
  TEST_ASSERT(state_regular_file(new_path));
  TEST_ASSERT(!state_path_exists(new_wal));
  TEST_ASSERT(!state_path_exists(new_shm));
  TEST_ASSERT(state_database_has_identity(new_path, "legacy"));

  baca_database_close(&legacy);
  free(old_path);
  free(old_wal);
  free(old_shm);
  free(new_path);
  free(new_wal);
  free(new_shm);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_current_database_family_wins(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("database-conflict", &environment));
  BacaDatabase legacy = {0};
  BacaDatabase current = {0};
  TEST_ASSERT(state_database_create(environment.cache, "baca/baca.db", "legacy",
                                    true, &legacy));
  TEST_ASSERT(state_database_create(environment.cache,
                                    "mereader-tui/mereader-tui.db", "current",
                                    false, &current));
  baca_database_close(&current);
  char *old_wal = state_path(environment.cache, "baca/baca.db-wal");
  char *old_shm = state_path(environment.cache, "baca/baca.db-shm");
  char *old_journal = state_path(environment.cache, "baca/baca.db-journal");
  char *new_path =
      state_path(environment.cache, "mereader-tui/mereader-tui.db");
  char *new_wal =
      state_path(environment.cache, "mereader-tui/mereader-tui.db-wal");
  char *new_shm =
      state_path(environment.cache, "mereader-tui/mereader-tui.db-shm");
  TEST_ASSERT(old_wal != NULL && old_shm != NULL && old_journal != NULL &&
              new_path != NULL && new_wal != NULL && new_shm != NULL);
  TEST_ASSERT(state_regular_file(old_wal) && state_regular_file(old_shm));
  BacaError setup_error = {0};
  TEST_ASSERT(baca_mkdirs(old_journal, &setup_error));

  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  TEST_ASSERT(state_database_has_identity(new_path, "current"));
  TEST_ASSERT(state_regular_file(old_wal) && state_regular_file(old_shm));
  TEST_ASSERT(state_path_exists(old_journal));
  TEST_ASSERT(!state_path_exists(new_wal));
  TEST_ASSERT(!state_path_exists(new_shm));

  baca_database_close(&legacy);
  free(old_wal);
  free(old_shm);
  free(old_journal);
  free(new_path);
  free(new_wal);
  free(new_shm);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_old_only_downloads(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("downloads", &environment));
  TEST_ASSERT(state_write(environment.cache, "baca/downloads/one.epub", "one"));
  TEST_ASSERT(state_write(environment.cache, "baca/downloads/two.pdf", "two"));
  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);

  char *old_one = state_path(environment.cache, "baca/downloads/one.epub");
  char *old_two = state_path(environment.cache, "baca/downloads/two.pdf");
  char *new_one =
      state_path(environment.cache, "mereader-tui/downloads/one.epub");
  char *new_two =
      state_path(environment.cache, "mereader-tui/downloads/two.pdf");
  TEST_ASSERT(old_one != NULL && old_two != NULL && new_one != NULL &&
              new_two != NULL);
  TEST_ASSERT(state_contents_equal(old_one, "one"));
  TEST_ASSERT(state_contents_equal(old_two, "two"));
  TEST_ASSERT(state_contents_equal(new_one, "one"));
  TEST_ASSERT(state_contents_equal(new_two, "two"));
  free(old_one);
  free(old_two);
  free(new_one);
  free(new_two);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_current_config_ignores_unsafe_legacy(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("config-conflict", &environment));
  TEST_ASSERT(state_write(environment.config, "outside.ini", "outside"));
  TEST_ASSERT(
      state_write(environment.config, "mereader-tui/config.ini", "current"));
  char *old_root = state_path(environment.config, "baca");
  char *old_path = state_path(environment.config, "baca/config.ini");
  char *new_path = state_path(environment.config, "mereader-tui/config.ini");
  TEST_ASSERT(old_root != NULL && old_path != NULL && new_path != NULL);
  BacaError setup_error = {0};
  TEST_ASSERT(baca_mkdirs(old_root, &setup_error));
  TEST_ASSERT(symlink("../outside.ini", old_path) == 0);

  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  TEST_ASSERT(state_path_exists(old_path));
  TEST_ASSERT(state_contents_equal(new_path, "current"));
  free(old_root);
  free(old_path);
  free(new_path);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_download_merge_preserves_collisions(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("download-merge", &environment));
  TEST_ASSERT(
      state_write(environment.cache, "baca/downloads/legacy.epub", "legacy"));
  TEST_ASSERT(state_write(environment.cache, "baca/downloads/collision.epub",
                          "old collision"));
  TEST_ASSERT(state_write(environment.cache,
                          "mereader-tui/downloads/current.epub", "current"));
  TEST_ASSERT(state_write(environment.cache,
                          "mereader-tui/downloads/collision.epub",
                          "new collision"));
  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);

  char *old_legacy =
      state_path(environment.cache, "baca/downloads/legacy.epub");
  char *new_legacy =
      state_path(environment.cache, "mereader-tui/downloads/legacy.epub");
  char *old_collision =
      state_path(environment.cache, "baca/downloads/collision.epub");
  char *new_collision =
      state_path(environment.cache, "mereader-tui/downloads/collision.epub");
  char *new_current =
      state_path(environment.cache, "mereader-tui/downloads/current.epub");
  TEST_ASSERT(old_legacy != NULL && new_legacy != NULL &&
              old_collision != NULL && new_collision != NULL &&
              new_current != NULL);
  TEST_ASSERT(state_contents_equal(old_legacy, "legacy"));
  TEST_ASSERT(state_contents_equal(new_legacy, "legacy"));
  TEST_ASSERT(state_contents_equal(old_collision, "old collision"));
  TEST_ASSERT(state_contents_equal(new_collision, "new collision"));
  TEST_ASSERT(state_contents_equal(new_current, "current"));
  free(old_legacy);
  free(new_legacy);
  free(old_collision);
  free(new_collision);
  free(new_current);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_unsafe_download_collision_rejected(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(
      state_environment_init("unsafe-download-collision", &environment));
  TEST_ASSERT(
      state_write(environment.cache, "baca/downloads/book.epub", "legacy"));
  TEST_ASSERT(state_write(environment.cache, "outside.epub", "outside"));
  char *downloads = state_path(environment.cache, "mereader-tui/downloads");
  char *collision =
      state_path(environment.cache, "mereader-tui/downloads/book.epub");
  TEST_ASSERT(downloads != NULL && collision != NULL);
  BacaError setup_error = {0};
  TEST_ASSERT(baca_mkdirs(downloads, &setup_error));
  TEST_ASSERT(symlink("../../outside.epub", collision) == 0);

  BacaError error = {0};
  TEST_ASSERT(!baca_state_migrate(&error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  TEST_ASSERT(state_path_exists(collision));
  free(downloads);
  free(collision);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_current_downloads_ignore_unsafe_legacy(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("downloads-unsafe-legacy", &environment));
  TEST_ASSERT(state_write(environment.cache, "outside/book.epub", "outside"));
  TEST_ASSERT(state_write(environment.cache,
                          "mereader-tui/downloads/current.epub", "current"));
  char *old_root = state_path(environment.cache, "baca");
  char *old_downloads = state_path(environment.cache, "baca/downloads");
  char *new_current =
      state_path(environment.cache, "mereader-tui/downloads/current.epub");
  TEST_ASSERT(old_root != NULL && old_downloads != NULL && new_current != NULL);
  BacaError setup_error = {0};
  TEST_ASSERT(baca_mkdirs(old_root, &setup_error));
  TEST_ASSERT(symlink("../outside", old_downloads) == 0);

  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  TEST_ASSERT(state_path_exists(old_downloads));
  TEST_ASSERT(state_contents_equal(new_current, "current"));
  free(old_root);
  free(old_downloads);
  free(new_current);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_current_downloads_report_legacy_open_failure(void) {
  if (geteuid() == 0) {
    return baca_test_skip("root bypasses directory access checks");
  }

  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(
      state_environment_init("downloads-legacy-open-failure", &environment));
  TEST_ASSERT(
      state_write(environment.cache, "baca/downloads/legacy.epub", "legacy"));
  TEST_ASSERT(state_write(environment.cache,
                          "mereader-tui/downloads/current.epub", "current"));
  BacaDatabase current = {0};
  TEST_ASSERT(state_database_create(environment.cache,
                                    "mereader-tui/mereader-tui.db", "current",
                                    false, &current));
  baca_database_close(&current);

  char *legacy_root = state_path(environment.cache, "baca");
  char *current_download =
      state_path(environment.cache, "mereader-tui/downloads/current.epub");
  char *marker = state_path(environment.config, STATE_IMPORT_MARKER);
  TEST_ASSERT(legacy_root != NULL && current_download != NULL &&
              marker != NULL);
  TEST_ASSERT(chmod(legacy_root, 0000) == 0);

  BacaError error = {0};
  const bool migrated = baca_state_migrate(&error);
  const int restore_status = chmod(legacy_root, 0700);
  TEST_ASSERT(restore_status == 0);
  TEST_ASSERT(!migrated);
  TEST_ASSERT_ERROR(error, BACA_ERROR_IO);
  TEST_ASSERT(state_contents_equal(current_download, "current"));
  TEST_ASSERT(!state_path_exists(marker));

  free(legacy_root);
  free(current_download);
  free(marker);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_current_directory_modes_hardened(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("directory-modes", &environment));
  TEST_ASSERT(state_write(environment.config, "baca/config.ini", "legacy"));
  TEST_ASSERT(
      state_write(environment.config, "mereader-tui/config.ini", "current"));
  TEST_ASSERT(
      state_write(environment.cache, "baca/downloads/legacy.epub", "legacy"));
  TEST_ASSERT(state_write(environment.cache,
                          "mereader-tui/downloads/current.epub", "current"));

  char *legacy_config_root = state_path(environment.config, "baca");
  char *current_config_root = state_path(environment.config, "mereader-tui");
  char *legacy_cache_root = state_path(environment.cache, "baca");
  char *current_cache_root = state_path(environment.cache, "mereader-tui");
  char *current_downloads =
      state_path(environment.cache, "mereader-tui/downloads");
  TEST_ASSERT(legacy_config_root != NULL && current_config_root != NULL &&
              legacy_cache_root != NULL && current_cache_root != NULL &&
              current_downloads != NULL);
  TEST_ASSERT(chmod(environment.config, 0755) == 0);
  TEST_ASSERT(chmod(environment.cache, 0755) == 0);
  TEST_ASSERT(chmod(legacy_config_root, 0755) == 0);
  TEST_ASSERT(chmod(current_config_root, 0755) == 0);
  TEST_ASSERT(chmod(legacy_cache_root, 0755) == 0);
  TEST_ASSERT(chmod(current_cache_root, 0755) == 0);
  TEST_ASSERT(chmod(current_downloads, 0755) == 0);

  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  TEST_ASSERT(state_directory_mode(environment.config, 0755));
  TEST_ASSERT(state_directory_mode(environment.cache, 0755));
  TEST_ASSERT(state_directory_mode(legacy_config_root, 0755));
  TEST_ASSERT(state_directory_mode(legacy_cache_root, 0755));
  TEST_ASSERT(state_directory_mode(current_config_root, 0700));
  TEST_ASSERT(state_directory_mode(current_cache_root, 0700));
  TEST_ASSERT(state_directory_mode(current_downloads, 0700));

  free(legacy_config_root);
  free(current_config_root);
  free(legacy_cache_root);
  free(current_cache_root);
  free(current_downloads);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_symlink_rejected(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("symlink", &environment));
  TEST_ASSERT(state_write(environment.config, "outside.ini", "outside"));
  char *old_root = state_path(environment.config, "baca");
  char *old_path = state_path(environment.config, "baca/config.ini");
  char *new_path = state_path(environment.config, "mereader-tui/config.ini");
  TEST_ASSERT(old_root != NULL && old_path != NULL && new_path != NULL);
  BacaError setup_error = {0};
  TEST_ASSERT(baca_mkdirs(old_root, &setup_error));
  TEST_ASSERT(symlink("../outside.ini", old_path) == 0);

  BacaError error = {0};
  TEST_ASSERT(!baca_state_migrate(&error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  TEST_ASSERT(state_path_exists(old_path));
  TEST_ASSERT(!state_path_exists(new_path));
  free(old_root);
  free(old_path);
  free(new_path);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_completion_marker_prevents_reimport(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("completion-marker", &environment));
  TEST_ASSERT(state_write(environment.config, "baca/config.ini", "legacy"));
  char *old_path = state_path(environment.config, "baca/config.ini");
  char *new_path = state_path(environment.config, "mereader-tui/config.ini");
  char *new_cache_root = state_path(environment.cache, "mereader-tui");
  char *marker = state_path(environment.config, STATE_IMPORT_MARKER);
  TEST_ASSERT(old_path != NULL && new_path != NULL && new_cache_root != NULL &&
              marker != NULL);
  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  TEST_ASSERT(state_regular_file(marker));
  TEST_ASSERT(unlink(new_path) == 0);
  BacaError cleanup_error = {0};
  TEST_ASSERT(baca_remove_tree(new_cache_root, &cleanup_error));
  TEST_ASSERT(!state_path_exists(new_cache_root));
  TEST_ASSERT(state_write(environment.config, "baca/config.ini", "stale"));
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  TEST_ASSERT(state_contents_equal(old_path, "stale"));
  TEST_ASSERT(!state_path_exists(new_path));
  TEST_ASSERT(!state_path_exists(new_cache_root));
  free(old_path);
  free(new_path);
  free(new_cache_root);
  free(marker);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_completion_marker_rehardens_current_roots(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("marker-modes", &environment));
  TEST_ASSERT(state_write(environment.config, "baca/config.ini", "legacy"));
  TEST_ASSERT(
      state_write(environment.cache, "baca/downloads/book.epub", "legacy"));
  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);

  char *config_root = state_path(environment.config, "mereader-tui");
  char *cache_root = state_path(environment.cache, "mereader-tui");
  char *downloads = state_path(environment.cache, "mereader-tui/downloads");
  TEST_ASSERT(config_root != NULL && cache_root != NULL && downloads != NULL);
  TEST_ASSERT(chmod(config_root, 0755) == 0);
  TEST_ASSERT(chmod(cache_root, 0755) == 0);
  TEST_ASSERT(chmod(downloads, 0755) == 0);

  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  TEST_ASSERT(state_directory_mode(config_root, 0700));
  TEST_ASSERT(state_directory_mode(cache_root, 0700));
  TEST_ASSERT(state_directory_mode(downloads, 0700));
  free(config_root);
  free(cache_root);
  free(downloads);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_orphan_new_database_sidecar_rejected(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("orphan-sidecar", &environment));
  TEST_ASSERT(state_write(environment.cache, "mereader-tui/mereader-tui.db-wal",
                          "orphan"));
  BacaError error = {0};
  TEST_ASSERT(!baca_state_migrate(&error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_unsafe_new_cache_root_rejected(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("unsafe-cache-root", &environment));
  TEST_ASSERT(
      state_write(environment.cache, "mereader-tui", "not a directory"));
  BacaError error = {0};
  TEST_ASSERT(!baca_state_migrate(&error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_unsafe_new_downloads_rejected(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("unsafe-downloads", &environment));
  TEST_ASSERT(state_write(environment.cache, "mereader-tui/downloads",
                          "not a directory"));
  BacaError error = {0};
  TEST_ASSERT(!baca_state_migrate(&error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_unsafe_completion_marker_rejected(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("unsafe-marker", &environment));
  char *marker = state_path(environment.config, STATE_IMPORT_MARKER);
  TEST_ASSERT(marker != NULL);
  BacaError setup_error = {0};
  TEST_ASSERT(baca_mkdirs(marker, &setup_error));
  BacaError error = {0};
  TEST_ASSERT(!baca_state_migrate(&error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  free(marker);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_invalid_completion_marker_rejected(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("invalid-marker", &environment));
  TEST_ASSERT(
      state_write(environment.config, STATE_IMPORT_MARKER, "invalid\n"));
  BacaError error = {0};
  TEST_ASSERT(!baca_state_migrate(&error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_marker_does_not_bypass_unsafe_current_root(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("marker-unsafe-root", &environment));
  TEST_ASSERT(state_write(environment.config, STATE_IMPORT_MARKER,
                          STATE_IMPORT_MARKER_CONTENTS));
  BacaError setup_error = {0};
  char *outside = state_path(environment.cache, "outside");
  char *current = state_path(environment.cache, "mereader-tui");
  TEST_ASSERT(outside != NULL && current != NULL);
  TEST_ASSERT(baca_mkdirs(outside, &setup_error));
  TEST_ASSERT(symlink("outside", current) == 0);

  BacaError error = {0};
  TEST_ASSERT(!baca_state_migrate(&error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  free(outside);
  free(current);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

static BacaTestResult test_null_error_rejected(void) {
  TEST_ASSERT(!baca_state_migrate(NULL));
  return BACA_TEST_PASS;
}

static BacaTestResult test_aliased_xdg_roots(void) {
  StateMigrationEnvironment environment = {0};
  TEST_ASSERT(state_environment_init("aliased-roots", &environment));
  TEST_ASSERT(setenv("XDG_CONFIG_HOME", environment.cache, 1) == 0);
  TEST_ASSERT(state_write(environment.cache, "baca/config.ini", "legacy"));
  BacaError error = {0};
  TEST_ASSERT_MSG(baca_state_migrate(&error), "%s", error.message);
  char *old_path = state_path(environment.cache, "baca/config.ini");
  char *new_path = state_path(environment.cache, "mereader-tui/config.ini");
  TEST_ASSERT(old_path != NULL && new_path != NULL);
  TEST_ASSERT(state_contents_equal(old_path, "legacy"));
  TEST_ASSERT(state_contents_equal(new_path, "legacy"));
  free(old_path);
  free(new_path);
  state_environment_restore(&environment);
  return BACA_TEST_PASS;
}

const BacaTestCase *baca_state_migration_test_cases(size_t *count) {
  static const BacaTestCase cases[] = {
      {.name = "old_only_config", .function = test_old_only_config},
      {.name = "database_snapshot_includes_wal_state",
       .function = test_database_snapshot_includes_wal_state},
      {.name = "current_database_family_wins",
       .function = test_current_database_family_wins},
      {.name = "old_only_downloads", .function = test_old_only_downloads},
      {.name = "current_config_ignores_unsafe_legacy",
       .function = test_current_config_ignores_unsafe_legacy},
      {.name = "download_merge_preserves_collisions",
       .function = test_download_merge_preserves_collisions},
      {.name = "unsafe_download_collision_rejected",
       .function = test_unsafe_download_collision_rejected},
      {.name = "current_downloads_ignore_unsafe_legacy",
       .function = test_current_downloads_ignore_unsafe_legacy},
      {.name = "current_downloads_report_legacy_open_failure",
       .function = test_current_downloads_report_legacy_open_failure},
      {.name = "current_directory_modes_hardened",
       .function = test_current_directory_modes_hardened},
      {.name = "symlink_rejected", .function = test_symlink_rejected},
      {.name = "completion_marker_prevents_reimport",
       .function = test_completion_marker_prevents_reimport},
      {.name = "completion_marker_rehardens_current_roots",
       .function = test_completion_marker_rehardens_current_roots},
      {.name = "orphan_new_database_sidecar_rejected",
       .function = test_orphan_new_database_sidecar_rejected},
      {.name = "unsafe_new_cache_root_rejected",
       .function = test_unsafe_new_cache_root_rejected},
      {.name = "unsafe_new_downloads_rejected",
       .function = test_unsafe_new_downloads_rejected},
      {.name = "unsafe_completion_marker_rejected",
       .function = test_unsafe_completion_marker_rejected},
      {.name = "invalid_completion_marker_rejected",
       .function = test_invalid_completion_marker_rejected},
      {.name = "marker_does_not_bypass_unsafe_current_root",
       .function = test_marker_does_not_bypass_unsafe_current_root},
      {.name = "null_error_rejected", .function = test_null_error_rejected},
      {.name = "aliased_xdg_roots", .function = test_aliased_xdg_roots},
  };
  *count = BACA_ARRAY_LEN(cases);
  return cases;
}
