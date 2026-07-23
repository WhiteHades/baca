#include "test_support.h"

#include "baca/database.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static bool sqlite_execute(sqlite3 *database, const char *sql) {
    char *message = NULL;
    int status = sqlite3_exec(database, sql, NULL, NULL, &message);
    if (status != SQLITE_OK) {
        fprintf(stderr, "database fixture: %s\n", message == NULL ? sqlite3_errmsg(database) : message);
        sqlite3_free(message);
        return false;
    }
    return true;
}

static bool sqlite_scalar_int(sqlite3 *database, const char *sql, int *value) {
    sqlite3_stmt *statement = NULL;
    int status = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (status == SQLITE_OK) {
        status = sqlite3_step(statement);
    }
    if (status == SQLITE_ROW) {
        *value = sqlite3_column_int(statement, 0);
        status = sqlite3_step(statement);
    }
    bool success = status == SQLITE_DONE && sqlite3_finalize(statement) == SQLITE_OK;
    if (!success) {
        fprintf(stderr, "database fixture query: %s\n", sqlite3_errmsg(database));
    }
    return success;
}

static bool open_test_database(const char *relative, bool migrate, BacaDatabase *database, BacaError *error) {
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return false;
    }
    char *directory = baca_path_dirname(path, error);
    bool opened = directory != NULL && baca_mkdirs(directory, error) && baca_database_open(database, path, error);
    free(directory);
    free(path);
    return opened && (!migrate || baca_database_migrate(database, error));
}

static BacaTestResult test_open_and_migrate_idempotently(void) {
    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(open_test_database("database/migrate.db", true, &database, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_migrate(&database, &error), "%s", error.message);
    int metadata_count = -1;
    int table_count = -1;
    TEST_ASSERT(sqlite_scalar_int(database.handle, "SELECT count(*) FROM metadata WHERE version IN (0, 1, 2)",
                                   &metadata_count));
    TEST_ASSERT(sqlite_scalar_int(database.handle,
                                   "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                                   "name IN ('metadata', 'reading_history', 'bookmarks', "
                                   "'library_format_preferences')",
                                   &table_count));
    TEST_ASSERT_INT(metadata_count, 3);
    TEST_ASSERT_INT(table_count, 4);
    baca_database_close(&database);
    TEST_ASSERT(database.handle == NULL && database.path == NULL);
    return BACA_TEST_PASS;
}

static BacaTestResult test_default_path_is_xdg_isolated(void) {
    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_database_open_default(&database, &error), "%s", error.message);
    char *expected = baca_test_path("xdg-cache/mereader-tui/mereader-tui.db");
    TEST_ASSERT(expected != NULL);
    TEST_ASSERT_STR(database.path, expected);
    TEST_ASSERT_MSG(baca_database_migrate(&database, &error), "%s", error.message);
    TEST_ASSERT(baca_file_exists(expected));
    free(expected);
    baca_database_close(&database);
    return BACA_TEST_PASS;
}

static BacaTestResult test_legacy_peewee_schema(void) {
    TEST_ASSERT(baca_test_mkdir("database"));
    char *path = baca_test_path("database/legacy.db");
    TEST_ASSERT(path != NULL);
    sqlite3 *legacy = NULL;
    TEST_ASSERT(sqlite3_open(path, &legacy) == SQLITE_OK);
    static const char schema[] =
        "CREATE TABLE metadata (version INTEGER NOT NULL PRIMARY KEY, migrated_at DATETIME NOT NULL);"
        "CREATE TABLE reading_history (filepath VARCHAR(255) NOT NULL PRIMARY KEY, title VARCHAR(255), "
        "author VARCHAR(255), reading_progress REAL NOT NULL, last_read DATETIME NOT NULL);"
        "INSERT INTO metadata VALUES (0, '2023-01-02 03:04:05.000000');"
        "INSERT INTO reading_history VALUES ('/legacy/book.epub', 'Legacy Title', 'Legacy Author', 0.375, "
        "'2023-02-03 04:05:06.000000');";
    TEST_ASSERT(sqlite_execute(legacy, schema));
    TEST_ASSERT(sqlite3_close(legacy) == SQLITE_OK);

    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_database_open(&database, path, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_migrate(&database, &error), "%s", error.message);
    BacaHistory history = {0};
    TEST_ASSERT_MSG(baca_database_history(&database, false, &history, &error), "%s", error.message);
    TEST_ASSERT_SIZE(history.length, 1U);
    TEST_ASSERT_STR(history.items[0].filepath, "/legacy/book.epub");
    TEST_ASSERT_STR(history.items[0].title, "Legacy Title");
    TEST_ASSERT_STR(history.items[0].author, "Legacy Author");
    TEST_ASSERT_DOUBLE(history.items[0].reading_progress, 0.375, 1e-12);
    TEST_ASSERT_STR(history.items[0].last_read, "2023-02-03 04:05:06.000000");
    baca_history_free(&history);
    baca_database_close(&database);
    free(path);
    return BACA_TEST_PASS;
}

static BacaHistoryEntry history_entry(const char *filepath, const char *title, const char *author,
                                      double progress, const char *last_read) {
    return (BacaHistoryEntry){
        .filepath = (char *)filepath,
        .title = (char *)title,
        .author = (char *)author,
        .reading_progress = progress,
        .last_read = (char *)last_read,
    };
}

static BacaTestResult test_save_list_order_nth_fuzzy_and_stale(void) {
    TEST_ASSERT(baca_test_write_text("database/books/alpha.epub", "alpha"));
    TEST_ASSERT(baca_test_write_text("database/books/beta.epub", "beta"));
    char *alpha = baca_test_path("database/books/alpha.epub");
    char *beta = baca_test_path("database/books/beta.epub");
    char *missing = baca_test_path("database/books/missing.epub");
    TEST_ASSERT(alpha != NULL && beta != NULL && missing != NULL);

    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(open_test_database("database/history.db", true, &database, &error), "%s", error.message);
    BacaHistoryEntry alpha_entry = history_entry(alpha, "First Book", "Alice", 0.1,
                                                 "2024-01-01 00:00:00.000000");
    BacaHistoryEntry beta_entry = history_entry(beta, "Second Story", "Writer Two", 0.2,
                                                "2024-02-01 00:00:00.000000");
    BacaHistoryEntry missing_entry = history_entry(missing, "Gone Book", NULL, 0.3,
                                                   "2024-03-01 00:00:00.000000");
    BacaHistoryEntry remote_entry = history_entry("https://example.test/book.epub", "Remote Book", NULL, 0.4,
                                                  "2024-05-01 00:00:00.000000");
    TEST_ASSERT_MSG(baca_database_save_progress(&database, &alpha_entry, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_save_progress(&database, &beta_entry, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_save_progress(&database, &missing_entry, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_save_progress(&database, &remote_entry, &error), "%s", error.message);
    alpha_entry.title = "First Book Revised";
    alpha_entry.reading_progress = 0.75;
    alpha_entry.last_read = "2024-04-01 00:00:00.000000";
    TEST_ASSERT_MSG(baca_database_save_progress(&database, &alpha_entry, &error), "%s", error.message);

    BacaHistory history = {0};
    TEST_ASSERT_MSG(baca_database_history(&database, false, &history, &error), "%s", error.message);
    TEST_ASSERT_SIZE(history.length, 4U);
    TEST_ASSERT_STR(history.items[0].filepath, "https://example.test/book.epub");
    TEST_ASSERT_STR(history.items[1].filepath, alpha);
    TEST_ASSERT_STR(history.items[1].title, "First Book Revised");
    TEST_ASSERT_DOUBLE(history.items[1].reading_progress, 0.75, 1e-12);
    TEST_ASSERT_STR(history.items[2].filepath, missing);
    TEST_ASSERT_STR(history.items[3].filepath, beta);
    TEST_ASSERT(baca_history_nth(&history, 0U) == NULL);
    TEST_ASSERT(baca_history_nth(&history, 5U) == NULL);
    TEST_ASSERT(baca_history_nth(&history, 1U) == &history.items[0]);
    const BacaHistoryEntry *match = baca_history_best_match(&history, "second writer two");
    TEST_ASSERT(match != NULL);
    TEST_ASSERT_STR(match->filepath, beta);
    TEST_ASSERT(baca_history_best_match(&history, "") == NULL);
    baca_history_free(&history);

    TEST_ASSERT_MSG(baca_database_history(&database, true, &history, &error), "%s", error.message);
    TEST_ASSERT_SIZE(history.length, 3U);
    TEST_ASSERT_STR(history.items[0].filepath, "https://example.test/book.epub");
    TEST_ASSERT_STR(history.items[1].filepath, alpha);
    TEST_ASSERT_STR(history.items[2].filepath, beta);
    baca_history_free(&history);
    TEST_ASSERT_MSG(baca_database_history(&database, false, &history, &error), "%s", error.message);
    TEST_ASSERT_SIZE(history.length, 3U);
    baca_history_free(&history);

    TEST_ASSERT_MSG(baca_database_remove_history(&database, beta, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_remove_history(&database, beta, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_history(&database, false, &history, &error), "%s", error.message);
    TEST_ASSERT_SIZE(history.length, 2U);
    TEST_ASSERT_STR(history.items[0].filepath, "https://example.test/book.epub");
    TEST_ASSERT_STR(history.items[1].filepath, alpha);
    baca_history_free(&history);
    baca_database_close(&database);
    free(alpha);
    free(beta);
    free(missing);
    return BACA_TEST_PASS;
}

static BacaTestResult test_progress_values(void) {
    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(open_test_database("database/progress.db", true, &database, &error), "%s", error.message);
    BacaHistoryEntry entry = history_entry("/progress/book.epub", NULL, NULL, -0.25,
                                           "2024-01-01 00:00:00");
    TEST_ASSERT_MSG(baca_database_save_progress(&database, &entry, &error), "%s", error.message);
    entry.reading_progress = 1.75;
    entry.last_read = "2024-01-02 00:00:00";
    TEST_ASSERT_MSG(baca_database_save_progress(&database, &entry, &error), "%s", error.message);
    BacaHistory history = {0};
    TEST_ASSERT_MSG(baca_database_history(&database, false, &history, &error), "%s", error.message);
    TEST_ASSERT_SIZE(history.length, 1U);
    TEST_ASSERT_DOUBLE(history.items[0].reading_progress, 1.75, 1e-12);
    baca_history_free(&history);

    entry.reading_progress = NAN;
    TEST_ASSERT(!baca_database_save_progress(&database, &entry, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_error_clear(&error);
    entry.reading_progress = INFINITY;
    TEST_ASSERT(!baca_database_save_progress(&database, &entry, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_database_close(&database);
    return BACA_TEST_PASS;
}

static BacaTestResult test_bookmark_crud(void) {
    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(open_test_database("database/bookmarks.db", true, &database, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_add_bookmark(&database, "/books/one.epub", 0.75, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_add_bookmark(&database, "/books/one.epub", 0.25, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_add_bookmark(&database, "/books/one.epub", 0.25, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_add_bookmark(&database, "/books/two.epub", 0.5, &error), "%s", error.message);
    char *database_path = baca_strdup(database.path, &error);
    TEST_ASSERT_MSG(database_path != NULL, "%s", error.message);
    baca_database_close(&database);
    TEST_ASSERT_MSG(baca_database_open(&database, database_path, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_migrate(&database, &error), "%s", error.message);
    free(database_path);

    BacaBookmarks bookmarks = {0};
    TEST_ASSERT_MSG(baca_database_bookmarks(&database, "/books/one.epub", &bookmarks, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(bookmarks.length, 2U);
    TEST_ASSERT(bookmarks.items[0].id > 0);
    TEST_ASSERT(bookmarks.items[1].id > 0 && bookmarks.items[1].id != bookmarks.items[0].id);
    TEST_ASSERT_DOUBLE(bookmarks.items[0].reading_progress, 0.25, 1e-12);
    TEST_ASSERT_DOUBLE(bookmarks.items[1].reading_progress, 0.75, 1e-12);
    TEST_ASSERT(bookmarks.items[0].created_at != NULL && bookmarks.items[0].created_at[0] != '\0');
    int64_t first_id = bookmarks.items[0].id;
    baca_bookmarks_free(&bookmarks);

    TEST_ASSERT_MSG(baca_database_remove_bookmark(&database, "/books/two.epub", first_id, &error), "%s",
                    error.message);
    TEST_ASSERT_MSG(baca_database_bookmarks(&database, "/books/one.epub", &bookmarks, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(bookmarks.length, 2U);
    baca_bookmarks_free(&bookmarks);
    TEST_ASSERT_MSG(baca_database_remove_bookmark(&database, "/books/one.epub", first_id, &error), "%s",
                    error.message);
    TEST_ASSERT_MSG(baca_database_remove_bookmark(&database, "/books/one.epub", first_id, &error), "%s",
                    error.message);
    TEST_ASSERT_MSG(baca_database_bookmarks(&database, "/books/one.epub", &bookmarks, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(bookmarks.length, 1U);
    TEST_ASSERT_DOUBLE(bookmarks.items[0].reading_progress, 0.75, 1e-12);
    baca_bookmarks_free(&bookmarks);

    TEST_ASSERT(!baca_database_add_bookmark(&database, "/books/one.epub", NAN, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_error_clear(&error);
    TEST_ASSERT(!baca_database_add_bookmark(&database, "/books/one.epub", -0.1, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_error_clear(&error);
    TEST_ASSERT(!baca_database_remove_bookmark(&database, "/books/one.epub", 0, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_database_close(&database);
    return BACA_TEST_PASS;
}

static BacaTestResult test_library_format_preferences(void) {
    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(open_test_database("database/formats.db", true, &database, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_save_format_preference(&database, "/library/one", "author/book",
                                                         "author/book.pdf", &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_save_format_preference(&database, "/library/two", "author/book",
                                                         "author/book.epub", &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_database_save_format_preference(&database, "/library/one", "author/book",
                                                         "author/book.epub", &error), "%s", error.message);

    BacaFormatPreferences preferences = {0};
    TEST_ASSERT_MSG(baca_database_format_preferences(&database, "/library/one", &preferences, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(preferences.length, 1U);
    TEST_ASSERT_STR(preferences.items[0].book_key, "author/book");
    TEST_ASSERT_STR(preferences.items[0].relative_path, "author/book.epub");
    baca_format_preferences_free(&preferences);

    TEST_ASSERT_MSG(baca_database_format_preferences(&database, "/library/missing", &preferences, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(preferences.length, 0U);
    baca_format_preferences_free(&preferences);
    baca_database_close(&database);
    return BACA_TEST_PASS;
}

static BacaTestResult test_control_and_embedded_nul_values(void) {
    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(open_test_database("database/hostile.db", true, &database, &error), "%s", error.message);
    static const char control_title[] = "line\n\ttitle\x1b[31m";
    BacaHistoryEntry entry = history_entry("/hostile/control.epub", control_title, "author\x7fvalue", 0.5,
                                           "2024-01-01\t00:00:00");
    TEST_ASSERT_MSG(baca_database_save_progress(&database, &entry, &error), "%s", error.message);
    BacaHistory history = {0};
    TEST_ASSERT_MSG(baca_database_history(&database, false, &history, &error), "%s", error.message);
    TEST_ASSERT_SIZE(history.length, 1U);
    TEST_ASSERT_STR(history.items[0].title, control_title);
    TEST_ASSERT_STR(history.items[0].author, "author\x7fvalue");
    baca_history_free(&history);

    sqlite3_stmt *statement = NULL;
    static const char sql[] =
        "INSERT INTO reading_history(filepath,title,author,reading_progress,last_read) VALUES(?1,NULL,NULL,0.1,'x')";
    TEST_ASSERT(sqlite3_prepare_v2(database.handle, sql, -1, &statement, NULL) == SQLITE_OK);
    static const char nul_path[] = {'/', 'n', 'u', 'l', '\0', 'x'};
    TEST_ASSERT(sqlite3_bind_text(statement, 1, nul_path, (int)sizeof(nul_path), SQLITE_STATIC) == SQLITE_OK);
    TEST_ASSERT(sqlite3_step(statement) == SQLITE_DONE);
    TEST_ASSERT(sqlite3_finalize(statement) == SQLITE_OK);
    TEST_ASSERT(!baca_database_history(&database, false, &history, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_DATABASE);
    TEST_ASSERT(strstr(error.message, "embedded null byte") != NULL);
    TEST_ASSERT(history.items == NULL && history.length == 0U);
    baca_database_close(&database);
    return BACA_TEST_PASS;
}

static int migration_child(const char *path, int start_descriptor) {
    char token = '\0';
    ssize_t read_count;
    do {
        read_count = read(start_descriptor, &token, 1U);
    } while (read_count < 0 && errno == EINTR);
    if (read_count != 1) {
        return 2;
    }
    BacaDatabase database = {0};
    BacaError error = {0};
    bool migrated = baca_database_open(&database, path, &error) && baca_database_migrate(&database, &error);
    baca_database_close(&database);
    if (!migrated) {
        fprintf(stderr, "concurrent migration child: %s\n", error.message);
    }
    return migrated ? 0 : 1;
}

static BacaTestResult test_concurrent_first_migration(void) {
    char *path = baca_test_path("database/concurrent.db");
    TEST_ASSERT(path != NULL);
    int start_pipe[2];
    TEST_ASSERT(pipe(start_pipe) == 0);
    enum { CHILDREN = 4 };
    pid_t children[CHILDREN] = {0};
    for (size_t index = 0U; index < CHILDREN; index++) {
        children[index] = fork();
        TEST_ASSERT(children[index] >= 0);
        if (children[index] == 0) {
            close(start_pipe[1]);
            int result = migration_child(path, start_pipe[0]);
            close(start_pipe[0]);
            _exit(result);
        }
    }
    close(start_pipe[0]);
    static const char tokens[CHILDREN] = {'1', '2', '3', '4'};
    size_t written = 0U;
    while (written < sizeof(tokens)) {
        ssize_t count = write(start_pipe[1], tokens + written, sizeof(tokens) - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        TEST_ASSERT(count > 0);
        written += (size_t)count;
    }
    TEST_ASSERT(close(start_pipe[1]) == 0);
    for (size_t index = 0U; index < CHILDREN; index++) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(children[index], &status, 0);
        } while (waited < 0 && errno == EINTR);
        TEST_ASSERT(waited == children[index]);
        TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    BacaDatabase database = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_database_open(&database, path, &error), "%s", error.message);
    int metadata_count = -1;
    TEST_ASSERT(sqlite_scalar_int(database.handle, "SELECT count(*) FROM metadata WHERE version=0",
                                  &metadata_count));
    TEST_ASSERT_INT(metadata_count, 1);
    baca_database_close(&database);
    free(path);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_database_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "open_and_migrate_idempotently", .function = test_open_and_migrate_idempotently},
        {.name = "default_path_is_xdg_isolated", .function = test_default_path_is_xdg_isolated},
        {.name = "legacy_peewee_schema", .function = test_legacy_peewee_schema},
        {.name = "save_list_order_nth_fuzzy_and_stale", .function = test_save_list_order_nth_fuzzy_and_stale},
        {.name = "progress_values", .function = test_progress_values},
        {.name = "bookmark_crud", .function = test_bookmark_crud},
        {.name = "library_format_preferences", .function = test_library_format_preferences},
        {.name = "control_and_embedded_nul_values", .function = test_control_and_embedded_nul_values},
        {.name = "concurrent_first_migration", .function = test_concurrent_first_migration},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
