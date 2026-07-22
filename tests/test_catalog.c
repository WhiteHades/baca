#include "test_support.h"

#include "baca/catalog.h"

#include <stdlib.h>

static const BacaCatalogBook *catalog_find_book(const BacaCatalog *catalog, const char *title) {
    for (size_t index = 0U; index < catalog->length; ++index) {
        if (strcmp(catalog->books[index].title, title) == 0) {
            return &catalog->books[index];
        }
    }
    return NULL;
}

static BacaTestResult test_calibre_hierarchy_formats_search_and_progress(void) {
    TEST_ASSERT(baca_test_write_text("catalog-calibre/metadata.db", ""));
    TEST_ASSERT(baca_test_write_text("catalog-calibre/Alice Writer/First Book (42)/first.epub", "epub"));
    TEST_ASSERT(baca_test_write_text("catalog-calibre/Alice Writer/First Book (42)/first.pdf", "pdf"));
    TEST_ASSERT(baca_test_write_text("catalog-calibre/Bob Writer/Second Book (7)/second.pdf", "pdf"));
    TEST_ASSERT(baca_test_write_text("catalog-calibre/Bob Writer/Second Book (7)/cover.jpg", "cover"));
    char *root = baca_test_path("catalog-calibre");
    char *epub = baca_test_path("catalog-calibre/Alice Writer/First Book (42)/first.epub");
    TEST_ASSERT(root != NULL && epub != NULL);
    BacaHistoryEntry entry = {
        .filepath = epub,
        .reading_progress = 0.625,
        .last_read = "2026-07-22 10:00:00",
    };
    BacaHistory history = {.items = &entry, .length = 1U};
    BacaCatalog catalog = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_catalog_open(&catalog, root, &history, false, &error), "%s", error.message);
    TEST_ASSERT(catalog.calibre);
    TEST_ASSERT_SIZE(catalog.length, 2U);
    const BacaCatalogBook *first = catalog_find_book(&catalog, "First Book");
    TEST_ASSERT(first != NULL);
    TEST_ASSERT_STR(first->author, "Alice Writer");
    TEST_ASSERT_SIZE(first->format_count, 2U);
    const BacaCatalogFormat *preferred = baca_catalog_preferred_format(first);
    TEST_ASSERT(preferred != NULL);
    TEST_ASSERT_STR(preferred->name, "EPUB");
    TEST_ASSERT_DOUBLE(preferred->reading_progress, 0.625, 1e-12);
    TEST_ASSERT_STR(preferred->last_read, "2026-07-22 10:00:00");

    BacaCatalogMatches matches = {0};
    TEST_ASSERT_MSG(baca_catalog_search(&catalog, "alice first", &matches, &error), "%s", error.message);
    TEST_ASSERT(matches.length >= 1U);
    TEST_ASSERT_STR(catalog.books[matches.book_indices[0]].title, "First Book");
    baca_catalog_matches_free(&matches);
    baca_catalog_close(&catalog);
    free(epub);
    free(root);
    return BACA_TEST_PASS;
}

static BacaTestResult test_flat_folder_groups_same_stem_only(void) {
    TEST_ASSERT(baca_test_write_text("catalog-flat/alpha.epub", "epub"));
    TEST_ASSERT(baca_test_write_text("catalog-flat/alpha.pdf", "pdf"));
    TEST_ASSERT(baca_test_write_text("catalog-flat/beta.epub", "epub"));
    TEST_ASSERT(baca_test_write_text("catalog-flat/cover.jpg", "cover"));
    char *root = baca_test_path("catalog-flat");
    TEST_ASSERT(root != NULL);
    BacaCatalog catalog = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_catalog_open(&catalog, root, NULL, false, &error), "%s", error.message);
    TEST_ASSERT(!catalog.calibre);
    TEST_ASSERT_SIZE(catalog.length, 2U);
    const BacaCatalogBook *alpha = catalog_find_book(&catalog, "alpha");
    TEST_ASSERT(alpha != NULL);
    TEST_ASSERT_STR(alpha->author, "");
    TEST_ASSERT_SIZE(alpha->format_count, 2U);
    TEST_ASSERT_STR(baca_catalog_preferred_format(alpha)->name, "EPUB");
    baca_catalog_close(&catalog);
    free(root);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_catalog_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "calibre_hierarchy_formats_search_and_progress",
         .function = test_calibre_hierarchy_formats_search_and_progress},
        {.name = "flat_folder_groups_same_stem_only", .function = test_flat_folder_groups_same_stem_only},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
