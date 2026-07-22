#pragma once

#include "baca/database.h"
#include "baca/search.h"

typedef struct BacaCatalogFormat {
    char *path;
    char *relative_path;
    char *name;
    double reading_progress;
    char *last_read;
} BacaCatalogFormat;

typedef struct BacaCatalogBook {
    char *title;
    char *author;
    char *directory;
    char *group_key;
    BacaCatalogFormat *formats;
    size_t format_count;
    size_t format_capacity;
    size_t preferred_format;
} BacaCatalogBook;

typedef struct BacaCatalog {
    BacaSearchIndex search;
    BacaCatalogBook *books;
    size_t length;
    size_t capacity;
    bool calibre;
} BacaCatalog;

typedef struct BacaCatalogMatches {
    size_t *book_indices;
    size_t length;
} BacaCatalogMatches;

[[nodiscard]] bool baca_catalog_open(BacaCatalog *catalog, const char *root, const BacaHistory *history, bool watch,
                                     BacaError *error);
[[nodiscard]] bool baca_catalog_refresh(BacaCatalog *catalog, const BacaHistory *history, BacaError *error);
void baca_catalog_close(BacaCatalog *catalog);
[[nodiscard]] bool baca_catalog_search(BacaCatalog *catalog, const char *query, BacaCatalogMatches *matches,
                                       BacaError *error);
void baca_catalog_matches_free(BacaCatalogMatches *matches);
[[nodiscard]] const BacaCatalogFormat *baca_catalog_preferred_format(const BacaCatalogBook *book);
