#pragma once

#include "baca/common.h"

#include <sqlite3.h>

typedef struct BacaDatabase {
    sqlite3 *handle;
    char *path;
} BacaDatabase;

typedef struct BacaHistoryEntry {
    char *filepath;
    char *title;
    char *author;
    double reading_progress;
    char *last_read;
} BacaHistoryEntry;

typedef struct BacaHistory {
    BacaHistoryEntry *items;
    size_t length;
    size_t capacity;
} BacaHistory;

typedef struct BacaBookmark {
    int64_t id;
    double reading_progress;
    char *created_at;
} BacaBookmark;

typedef struct BacaBookmarks {
    BacaBookmark *items;
    size_t length;
    size_t capacity;
} BacaBookmarks;

typedef struct BacaFormatPreference {
    char *book_key;
    char *relative_path;
} BacaFormatPreference;

typedef struct BacaFormatPreferences {
    BacaFormatPreference *items;
    size_t length;
    size_t capacity;
} BacaFormatPreferences;

[[nodiscard]] bool baca_database_open(BacaDatabase *database, const char *path, BacaError *error);
[[nodiscard]] bool baca_database_open_default(BacaDatabase *database, BacaError *error);
void baca_database_close(BacaDatabase *database);
[[nodiscard]] bool baca_database_migrate(BacaDatabase *database, BacaError *error);
[[nodiscard]] bool baca_database_history(BacaDatabase *database, bool remove_missing, BacaHistory *history,
                                         BacaError *error);
[[nodiscard]] bool baca_database_save_progress(BacaDatabase *database, const BacaHistoryEntry *entry,
                                               BacaError *error);
[[nodiscard]] bool baca_database_remove_history(BacaDatabase *database, const char *filepath, BacaError *error);
void baca_history_free(BacaHistory *history);
[[nodiscard]] const BacaHistoryEntry *baca_history_nth(const BacaHistory *history, size_t one_based_index);
[[nodiscard]] const BacaHistoryEntry *baca_history_best_match(const BacaHistory *history, const char *query);
[[nodiscard]] bool baca_database_add_bookmark(BacaDatabase *database, const char *filepath, double reading_progress,
                                              BacaError *error);
[[nodiscard]] bool baca_database_bookmarks(BacaDatabase *database, const char *filepath, BacaBookmarks *bookmarks,
                                           BacaError *error);
[[nodiscard]] bool baca_database_remove_bookmark(BacaDatabase *database, const char *filepath, int64_t id,
                                                 BacaError *error);
void baca_bookmarks_free(BacaBookmarks *bookmarks);
[[nodiscard]] bool baca_database_format_preferences(BacaDatabase *database, const char *library_root,
                                                    BacaFormatPreferences *preferences, BacaError *error);
[[nodiscard]] bool baca_database_save_format_preference(BacaDatabase *database, const char *library_root,
                                                        const char *book_key, const char *relative_path,
                                                        BacaError *error);
void baca_format_preferences_free(BacaFormatPreferences *preferences);
