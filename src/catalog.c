#include "baca/catalog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACA_CATALOG_PAGE_SIZE 4096U
#define BACA_CATALOG_MAX_DEPTH 256U

typedef struct CatalogMapSlot {
    const char *key;
    size_t value;
} CatalogMapSlot;

typedef struct CatalogMap {
    CatalogMapSlot *slots;
    size_t capacity;
    size_t length;
} CatalogMap;

static uint64_t catalog_hash(const char *value) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool catalog_map_init(CatalogMap *map, size_t item_count, BacaError *error) {
    size_t capacity = 16U;
    const size_t needed = item_count > SIZE_MAX / 2U ? SIZE_MAX : item_count * 2U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            baca_error_set(error, BACA_ERROR_MEMORY, "library map is too large");
            return false;
        }
        capacity *= 2U;
    }
    map->slots = baca_reallocarray(NULL, capacity, sizeof(*map->slots), error);
    if (map->slots == NULL) {
        return false;
    }
    memset(map->slots, 0, capacity * sizeof(*map->slots));
    map->capacity = capacity;
    return true;
}

static bool catalog_map_lookup(const CatalogMap *map, const char *key, size_t *value) {
    if (map->capacity == 0U) {
        return false;
    }
    size_t slot = (size_t)(catalog_hash(key) & (uint64_t)(map->capacity - 1U));
    while (map->slots[slot].key != NULL) {
        if (strcmp(map->slots[slot].key, key) == 0) {
            if (value != NULL) {
                *value = map->slots[slot].value;
            }
            return true;
        }
        slot = (slot + 1U) & (map->capacity - 1U);
    }
    return false;
}

static bool catalog_map_grow(CatalogMap *map, BacaError *error) {
    if (map->capacity > SIZE_MAX / 2U) {
        baca_error_set(error, BACA_ERROR_MEMORY, "library map is too large");
        return false;
    }
    const size_t capacity = map->capacity * 2U;
    CatalogMapSlot *slots = baca_reallocarray(NULL, capacity, sizeof(*slots), error);
    if (slots == NULL) {
        return false;
    }
    memset(slots, 0, capacity * sizeof(*slots));
    for (size_t index = 0U; index < map->capacity; ++index) {
        if (map->slots[index].key == NULL) {
            continue;
        }
        size_t slot = (size_t)(catalog_hash(map->slots[index].key) & (uint64_t)(capacity - 1U));
        while (slots[slot].key != NULL) {
            slot = (slot + 1U) & (capacity - 1U);
        }
        slots[slot] = map->slots[index];
    }
    free(map->slots);
    map->slots = slots;
    map->capacity = capacity;
    return true;
}

static bool catalog_map_insert(CatalogMap *map, const char *key, size_t value, BacaError *error) {
    if (map->capacity == 0U) {
        baca_error_set(error, BACA_ERROR_INTERNAL, "library map is not initialized");
        return false;
    }
    if (map->length >= map->capacity / 2U && !catalog_map_grow(map, error)) {
        return false;
    }
    size_t slot = (size_t)(catalog_hash(key) & (uint64_t)(map->capacity - 1U));
    size_t inspected = 0U;
    while (map->slots[slot].key != NULL) {
        if (strcmp(map->slots[slot].key, key) == 0) {
            map->slots[slot].value = value;
            return true;
        }
        slot = (slot + 1U) & (map->capacity - 1U);
        if (++inspected == map->capacity) {
            baca_error_set(error, BACA_ERROR_MEMORY, "library map is full");
            return false;
        }
    }
    map->slots[slot] = (CatalogMapSlot){.key = key, .value = value};
    ++map->length;
    return true;
}

static void catalog_map_free(CatalogMap *map) {
    free(map->slots);
    *map = (CatalogMap){0};
}

static const char *catalog_supported_extension(const char *path) {
    static const char *extensions[] = {
        ".epub", ".epub3", ".pdf", ".mobi", ".prc", ".azw", ".azw3", ".azw4",
        ".fb2",  ".txt",   ".md",  ".cbz",  ".cbr", ".cb7",
    };
    const char *extension = baca_path_extension(path);
    for (size_t index = 0U; extension != NULL && index < BACA_ARRAY_LEN(extensions); ++index) {
        if (baca_casecmp(extension, extensions[index]) == 0) {
            return extensions[index];
        }
    }
    return NULL;
}

static size_t catalog_format_rank(const char *name) {
    if (baca_casecmp(name, "epub") == 0 || baca_casecmp(name, "epub3") == 0) {
        return 0U;
    }
    if (baca_casecmp(name, "pdf") == 0) {
        return 1U;
    }
    if (baca_casecmp(name, "mobi") == 0 || baca_casecmp(name, "azw") == 0 ||
        baca_casecmp(name, "azw3") == 0 || baca_casecmp(name, "azw4") == 0 ||
        baca_casecmp(name, "prc") == 0) {
        return 2U;
    }
    return 3U;
}

static char *catalog_format_name(const char *extension, BacaError *error) {
    const char *start = extension[0] == '.' ? extension + 1 : extension;
    char *name = baca_strdup(start, error);
    if (name == NULL) {
        return NULL;
    }
    for (char *cursor = name; *cursor != '\0'; ++cursor) {
        *cursor = (char)toupper((unsigned char)*cursor);
    }
    return name;
}

static char *catalog_stem(const char *path, BacaError *error) {
    char *basename = baca_path_basename(path, error);
    if (basename == NULL) {
        return NULL;
    }
    char *extension = strrchr(basename, '.');
    if (extension != NULL) {
        *extension = '\0';
    }
    return basename;
}

static char *catalog_calibre_title(const char *directory, BacaError *error) {
    char *title = baca_path_basename(directory, error);
    if (title == NULL) {
        return NULL;
    }
    char *suffix = strrchr(title, '(');
    if (suffix == NULL || suffix == title || suffix[-1] != ' ') {
        return title;
    }
    char *cursor = suffix + 1;
    if (!isdigit((unsigned char)*cursor)) {
        return title;
    }
    while (isdigit((unsigned char)*cursor)) {
        ++cursor;
    }
    if (cursor[0] == ')' && cursor[1] == '\0') {
        suffix[-1] = '\0';
    }
    return title;
}

static char *catalog_calibre_author(const char *directory, BacaError *error) {
    char *parent = baca_path_dirname(directory, error);
    if (parent == NULL) {
        return NULL;
    }
    char *author = strcmp(parent, ".") == 0 ? baca_strdup("", error) : baca_path_basename(parent, error);
    free(parent);
    return author;
}

static bool catalog_detect_calibre(const char *root, BacaError *error) {
    char *metadata = baca_path_join(root, "metadata.db", error);
    if (metadata == NULL) {
        return false;
    }
    struct stat status;
    const bool calibre = stat(metadata, &status) == 0 && S_ISREG(status.st_mode);
    free(metadata);
    return calibre;
}

static void catalog_format_free(BacaCatalogFormat *format) {
    free(format->path);
    free(format->relative_path);
    free(format->name);
    free(format->last_read);
    *format = (BacaCatalogFormat){0};
}

static void catalog_book_free(BacaCatalogBook *book) {
    free(book->title);
    free(book->author);
    free(book->directory);
    free(book->group_key);
    for (size_t index = 0U; index < book->format_count; ++index) {
        catalog_format_free(&book->formats[index]);
    }
    free(book->formats);
    *book = (BacaCatalogBook){0};
}

static void catalog_books_free(BacaCatalog *catalog) {
    for (size_t index = 0U; index < catalog->length; ++index) {
        catalog_book_free(&catalog->books[index]);
    }
    free(catalog->books);
    catalog->books = NULL;
    catalog->length = 0U;
    catalog->capacity = 0U;
}

void baca_catalog_close(BacaCatalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    catalog_books_free(catalog);
    baca_search_index_close(&catalog->search);
    *catalog = (BacaCatalog){0};
}

bool baca_catalog_is_open(const BacaCatalog *catalog) {
    return catalog != NULL && catalog->search.handle != NULL;
}

static bool catalog_add_book(BacaCatalog *catalog, const char *key, const char *relative_path, CatalogMap *groups,
                             size_t *book_index, BacaError *error) {
    if (catalog_map_lookup(groups, key, book_index)) {
        return true;
    }
    BacaCatalogBook *books = baca_array_reserve(catalog->books, &catalog->capacity, sizeof(*catalog->books),
                                                catalog->length + 1U, error);
    if (books == NULL) {
        return false;
    }
    catalog->books = books;

    char *directory = baca_path_dirname(relative_path, error);
    char *title = NULL;
    char *author = NULL;
    char *group_key = baca_strdup(key, error);
    if (directory != NULL && catalog->calibre) {
        title = catalog_calibre_title(directory, error);
        author = title == NULL ? NULL : catalog_calibre_author(directory, error);
    } else if (directory != NULL) {
        title = catalog_stem(relative_path, error);
        author = title == NULL ? NULL : baca_strdup("", error);
    }
    if (directory == NULL || title == NULL || author == NULL || group_key == NULL) {
        free(directory);
        free(title);
        free(author);
        free(group_key);
        return false;
    }

    *book_index = catalog->length;
    catalog->books[catalog->length++] = (BacaCatalogBook){
        .title = title,
        .author = author,
        .directory = directory,
        .group_key = group_key,
    };
    return catalog_map_insert(groups, catalog->books[*book_index].group_key, *book_index, error);
}

static bool catalog_add_format(BacaCatalog *catalog, const char *relative_path, const char *extension,
                               CatalogMap *groups, BacaError *error) {
    char *directory = baca_path_dirname(relative_path, error);
    char *stem = directory == NULL ? NULL : catalog_stem(relative_path, error);
    char *key = NULL;
    if (directory != NULL && stem != NULL) {
        key = catalog->calibre ? baca_strdup(directory, error) : baca_path_join(directory, stem, error);
    }
    free(directory);
    free(stem);
    if (key == NULL) {
        return false;
    }

    size_t book_index = 0U;
    const bool book_ready = catalog_add_book(catalog, key, relative_path, groups, &book_index, error);
    free(key);
    if (!book_ready) {
        return false;
    }
    BacaCatalogBook *book = &catalog->books[book_index];
    BacaCatalogFormat *formats = baca_array_reserve(book->formats, &book->format_capacity, sizeof(*book->formats),
                                                    book->format_count + 1U, error);
    if (formats == NULL) {
        return false;
    }
    book->formats = formats;

    char *path = baca_path_join(catalog->search.root, relative_path, error);
    char *stored_relative = path == NULL ? NULL : baca_strdup(relative_path, error);
    char *name = stored_relative == NULL ? NULL : catalog_format_name(extension, error);
    if (path == NULL || stored_relative == NULL || name == NULL) {
        free(path);
        free(stored_relative);
        free(name);
        return false;
    }
    const size_t new_rank = catalog_format_rank(name);
    if (book->format_count == 0U || new_rank < catalog_format_rank(book->formats[book->preferred_format].name)) {
        book->preferred_format = book->format_count;
    }
    book->formats[book->format_count++] = (BacaCatalogFormat){
        .path = path,
        .relative_path = stored_relative,
        .name = name,
    };
    return true;
}

static int catalog_compare_books(const void *left_pointer, const void *right_pointer) {
    const BacaCatalogBook *left = left_pointer;
    const BacaCatalogBook *right = right_pointer;
    const int author = baca_casecmp(left->author, right->author);
    if (author != 0) {
        return author;
    }
    const int title = baca_casecmp(left->title, right->title);
    return title == 0 ? strcmp(left->directory, right->directory) : title;
}

static int catalog_compare_formats(const void *left_pointer, const void *right_pointer) {
    const BacaCatalogFormat *left = left_pointer;
    const BacaCatalogFormat *right = right_pointer;
    const size_t left_rank = catalog_format_rank(left->name);
    const size_t right_rank = catalog_format_rank(right->name);
    if (left_rank != right_rank) {
        return left_rank < right_rank ? -1 : 1;
    }
    const int name = baca_casecmp(left->name, right->name);
    return name == 0 ? strcmp(left->relative_path, right->relative_path) : name;
}

static bool catalog_walk_directory(BacaCatalog *catalog, int descriptor, const char *relative_directory, size_t depth,
                                   CatalogMap *groups, BacaError *error) {
    if (depth > BACA_CATALOG_MAX_DEPTH) {
        (void)close(descriptor);
        baca_error_set(error, BACA_ERROR_IO, "library nesting exceeds %u directories",
                       (unsigned)BACA_CATALOG_MAX_DEPTH);
        return false;
    }
    DIR *stream = fdopendir(descriptor);
    if (stream == NULL) {
        const int status = errno;
        (void)close(descriptor);
        baca_error_set(error, BACA_ERROR_IO, "could not scan library directory '%s': %s", relative_directory,
                       strerror(status));
        return false;
    }

    bool success = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (entry == NULL) {
            if (errno != 0) {
                baca_error_set(error, BACA_ERROR_IO, "could not finish scanning library directory '%s': %s",
                               relative_directory, strerror(errno));
                success = false;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *relative_path = relative_directory[0] == '\0' ? baca_strdup(entry->d_name, error)
                                                             : baca_path_join(relative_directory, entry->d_name, error);
        if (relative_path == NULL) {
            free(relative_path);
            success = false;
            break;
        }
        struct stat status;
        if (fstatat(dirfd(stream), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
            const int stat_error = errno;
            free(relative_path);
            if (stat_error == EACCES || stat_error == ENOENT) {
                continue;
            }
            baca_error_set(error, BACA_ERROR_IO, "could not inspect library entry '%s': %s", entry->d_name,
                           strerror(stat_error));
            success = false;
            break;
        }
        if (S_ISDIR(status.st_mode)) {
            const int child = openat(dirfd(stream), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child >= 0) {
                success = catalog_walk_directory(catalog, child, relative_path, depth + 1U, groups, error);
            } else if (errno != EACCES && errno != ENOENT && errno != ELOOP) {
                baca_error_set(error, BACA_ERROR_IO, "could not open library directory '%s': %s", relative_path,
                               strerror(errno));
                success = false;
            }
        } else if (S_ISREG(status.st_mode)) {
            const char *extension = catalog_supported_extension(relative_path);
            success = extension == NULL || catalog_add_format(catalog, relative_path, extension, groups, error);
        }
        free(relative_path);
        if (!success) {
            break;
        }
    }
    if (closedir(stream) != 0 && success) {
        baca_error_set(error, BACA_ERROR_IO, "could not close library directory '%s': %s", relative_directory,
                       strerror(errno));
        success = false;
    }
    return success;
}

static bool catalog_collect_files(BacaCatalog *catalog, BacaError *error) {
    CatalogMap groups = {0};
    const int root = open(catalog->search.root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) {
        baca_error_set(error, BACA_ERROR_IO, "could not open library root '%s': %s", catalog->search.root,
                       strerror(errno));
        return false;
    }
    if (!catalog_map_init(&groups, 0U, error)) {
        (void)close(root);
        return false;
    }
    if (!catalog_walk_directory(catalog, root, "", 0U, &groups, error)) {
        catalog_map_free(&groups);
        return false;
    }
    catalog_map_free(&groups);
    for (size_t index = 0U; index < catalog->length; ++index) {
        BacaCatalogBook *book = &catalog->books[index];
        if (book->format_count > 1U) {
            qsort(book->formats, book->format_count, sizeof(*book->formats), catalog_compare_formats);
        }
        book->preferred_format = 0U;
    }
    if (catalog->length > 1U) {
        qsort(catalog->books, catalog->length, sizeof(*catalog->books), catalog_compare_books);
    }
    return true;
}

static bool catalog_apply_history(BacaCatalog *catalog, const BacaHistory *history, BacaError *error) {
    size_t format_count = 0U;
    for (size_t index = 0U; index < catalog->length; ++index) {
        if (format_count > SIZE_MAX - catalog->books[index].format_count) {
            baca_error_set(error, BACA_ERROR_MEMORY, "library format count overflow");
            return false;
        }
        format_count += catalog->books[index].format_count;
    }
    CatalogMap paths = {0};
    BacaCatalogFormat **formats = format_count == 0U ? NULL : baca_reallocarray(NULL, format_count, sizeof(*formats),
                                                                                error);
    if (!catalog_map_init(&paths, format_count, error) || (format_count > 0U && formats == NULL)) {
        free(formats);
        catalog_map_free(&paths);
        return false;
    }
    size_t flat_index = 0U;
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        BacaCatalogBook *book = &catalog->books[book_index];
        for (size_t format_index = 0U; format_index < book->format_count; ++format_index) {
            formats[flat_index] = &book->formats[format_index];
            if (!catalog_map_insert(&paths, book->formats[format_index].path, flat_index++, error)) {
                free(formats);
                catalog_map_free(&paths);
                return false;
            }
        }
    }
    for (size_t history_index = 0U; history != NULL && history_index < history->length; ++history_index) {
        const BacaHistoryEntry *entry = &history->items[history_index];
        size_t target = 0U;
        if (entry->filepath == NULL || !catalog_map_lookup(&paths, entry->filepath, &target)) {
            continue;
        }
        BacaCatalogFormat *format = formats[target];
        format->reading_progress = entry->reading_progress;
        free(format->last_read);
        format->last_read = entry->last_read == NULL ? NULL : baca_strdup(entry->last_read, error);
        if (entry->last_read != NULL && format->last_read == NULL) {
            free(formats);
            catalog_map_free(&paths);
            return false;
        }
    }
    free(formats);
    catalog_map_free(&paths);
    return true;
}

bool baca_catalog_open(BacaCatalog *catalog, const char *root, const BacaHistory *history, bool watch,
                       BacaError *error) {
    if (catalog == NULL || root == NULL || root[0] == '\0') {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid library catalog root");
        return false;
    }
    if (catalog->search.handle != NULL || catalog->books != NULL || catalog->length != 0U) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "library catalog output is not empty");
        return false;
    }
    if (!baca_search_index_open(&catalog->search, root, watch, error)) {
        return false;
    }
    catalog->calibre = catalog_detect_calibre(catalog->search.root, error);
    if (baca_error_is_set(error) || !catalog_collect_files(catalog, error) ||
        !catalog_apply_history(catalog, history, error)) {
        baca_catalog_close(catalog);
        return false;
    }
    return true;
}

bool baca_catalog_refresh(BacaCatalog *catalog, const BacaHistory *history, BacaError *error) {
    if (catalog == NULL || catalog->search.handle == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "library catalog is not open");
        return false;
    }
    if (!baca_search_index_refresh(&catalog->search, error)) {
        return false;
    }
    catalog_books_free(catalog);
    catalog->calibre = catalog_detect_calibre(catalog->search.root, error);
    if (baca_error_is_set(error) || !catalog_collect_files(catalog, error) ||
        !catalog_apply_history(catalog, history, error)) {
        catalog_books_free(catalog);
        return false;
    }
    return true;
}

bool baca_catalog_update_progress(BacaCatalog *catalog, const BacaHistory *history, BacaError *error) {
    if (catalog == NULL || catalog->search.handle == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "library catalog is not open");
        return false;
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        BacaCatalogBook *book = &catalog->books[book_index];
        for (size_t format_index = 0U; format_index < book->format_count; ++format_index) {
            book->formats[format_index].reading_progress = 0.0;
            free(book->formats[format_index].last_read);
            book->formats[format_index].last_read = NULL;
        }
    }
    return catalog_apply_history(catalog, history, error);
}

const BacaCatalogFormat *baca_catalog_preferred_format(const BacaCatalogBook *book) {
    if (book == NULL || book->format_count == 0U || book->preferred_format >= book->format_count) {
        return NULL;
    }
    return &book->formats[book->preferred_format];
}

bool baca_catalog_apply_format_preferences(BacaCatalog *catalog, const BacaFormatPreferences *preferences,
                                           BacaError *error) {
    if (catalog == NULL || preferences == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid catalog format preferences");
        return false;
    }
    CatalogMap books = {0};
    if (!catalog_map_init(&books, catalog->length, error)) {
        return false;
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        if (!catalog_map_insert(&books, catalog->books[book_index].group_key, book_index, error)) {
            catalog_map_free(&books);
            return false;
        }
    }
    for (size_t preference_index = 0U; preference_index < preferences->length; ++preference_index) {
        const BacaFormatPreference *preference = &preferences->items[preference_index];
        size_t book_index = 0U;
        if (!catalog_map_lookup(&books, preference->book_key, &book_index)) {
            continue;
        }
        BacaCatalogBook *book = &catalog->books[book_index];
        for (size_t format_index = 0U; format_index < book->format_count; ++format_index) {
            if (strcmp(book->formats[format_index].relative_path, preference->relative_path) == 0) {
                book->preferred_format = format_index;
                break;
            }
        }
    }
    catalog_map_free(&books);
    return true;
}

bool baca_catalog_prefer_path(BacaCatalog *catalog, const char *path, const char **book_key,
                              const char **relative_path) {
    if (catalog == NULL || path == NULL) {
        return false;
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        BacaCatalogBook *book = &catalog->books[book_index];
        for (size_t format_index = 0U; format_index < book->format_count; ++format_index) {
            if (strcmp(book->formats[format_index].path, path) != 0) {
                continue;
            }
            book->preferred_format = format_index;
            if (book_key != NULL) {
                *book_key = book->group_key;
            }
            if (relative_path != NULL) {
                *relative_path = book->formats[format_index].relative_path;
            }
            return true;
        }
    }
    return false;
}

void baca_catalog_matches_free(BacaCatalogMatches *matches) {
    if (matches == NULL) {
        return;
    }
    free(matches->book_indices);
    *matches = (BacaCatalogMatches){0};
}

static bool catalog_text_matches(const char *text, const char *token, size_t token_length) {
    if (text == NULL || token_length == 0U) {
        return false;
    }
    const size_t text_length = strlen(text);
    if (token_length > text_length) {
        return false;
    }
    for (size_t offset = 0U; offset <= text_length - token_length; ++offset) {
        size_t index = 0U;
        while (index < token_length &&
               tolower((unsigned char)text[offset + index]) == tolower((unsigned char)token[index])) {
            ++index;
        }
        if (index == token_length) {
            return true;
        }
    }
    size_t token_index = 0U;
    for (size_t text_index = 0U; text_index < text_length && token_index < token_length; ++text_index) {
        if (tolower((unsigned char)text[text_index]) == tolower((unsigned char)token[token_index])) {
            ++token_index;
        }
    }
    return token_index == token_length;
}

static bool catalog_book_matches(const BacaCatalogBook *book, const char *query) {
    const char *cursor = query;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const char *token = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const size_t token_length = (size_t)(cursor - token);
        if (token_length == 0U) {
            continue;
        }
        bool found = catalog_text_matches(book->title, token, token_length) ||
                     catalog_text_matches(book->author, token, token_length) ||
                     catalog_text_matches(book->directory, token, token_length);
        for (size_t index = 0U; !found && index < book->format_count; ++index) {
            found = catalog_text_matches(book->formats[index].relative_path, token, token_length) ||
                    catalog_text_matches(book->formats[index].name, token, token_length);
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool baca_catalog_search(BacaCatalog *catalog, const char *query, BacaCatalogMatches *matches, BacaError *error) {
    if (catalog == NULL || catalog->search.handle == NULL || query == NULL || matches == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid catalog search request");
        return false;
    }
    if (matches->book_indices != NULL || matches->length != 0U) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "catalog search output is not empty");
        return false;
    }
    size_t *indices = catalog->length == 0U ? NULL : baca_reallocarray(NULL, catalog->length, sizeof(*indices), error);
    if (catalog->length > 0U && indices == NULL) {
        return false;
    }
    if (query[0] == '\0') {
        for (size_t index = 0U; index < catalog->length; ++index) {
            indices[index] = index;
        }
        *matches = (BacaCatalogMatches){.book_indices = indices, .length = catalog->length};
        return true;
    }

    size_t format_count = 0U;
    for (size_t index = 0U; index < catalog->length; ++index) {
        if (format_count > SIZE_MAX - catalog->books[index].format_count) {
            free(indices);
            baca_error_set(error, BACA_ERROR_MEMORY, "library format count overflow");
            return false;
        }
        format_count += catalog->books[index].format_count;
    }
    CatalogMap paths = {0};
    bool *seen = catalog->length == 0U ? NULL : calloc(catalog->length, sizeof(*seen));
    if (!catalog_map_init(&paths, format_count, error) || (catalog->length > 0U && seen == NULL)) {
        catalog_map_free(&paths);
        free(seen);
        free(indices);
        if (!baca_error_is_set(error)) {
            baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate catalog search state");
        }
        return false;
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        for (size_t format_index = 0U; format_index < catalog->books[book_index].format_count; ++format_index) {
            if (!catalog_map_insert(&paths, catalog->books[book_index].formats[format_index].relative_path,
                                    book_index, error)) {
                catalog_map_free(&paths);
                free(seen);
                free(indices);
                return false;
            }
        }
    }

    size_t length = 0U;
    size_t offset = 0U;
    bool complete = false;
    while (!complete) {
        BacaSearchFiles files = {0};
        if (!baca_search_files(&catalog->search, query, offset, BACA_CATALOG_PAGE_SIZE, &files, error)) {
            catalog_map_free(&paths);
            free(seen);
            free(indices);
            return false;
        }
        for (size_t index = 0U; index < files.length; ++index) {
            size_t book_index = 0U;
            if (catalog_map_lookup(&paths, files.items[index].relative_path, &book_index) && !seen[book_index]) {
                seen[book_index] = true;
                indices[length++] = book_index;
            }
        }
        if (files.length == 0U || offset + files.length >= files.total) {
            complete = true;
        } else {
            offset += files.length;
        }
        baca_search_files_free(&files);
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        if (!seen[book_index] && catalog_book_matches(&catalog->books[book_index], query)) {
            seen[book_index] = true;
            indices[length++] = book_index;
        }
    }
    catalog_map_free(&paths);
    free(seen);
    *matches = (BacaCatalogMatches){.book_indices = indices, .length = length};
    return true;
}
