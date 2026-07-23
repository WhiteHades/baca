#include "test_support.h"

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    if (getenv("BACA_IMAGE_PTY_CHILD") != NULL) {
        const int result = baca_image_pty_child();
        cairo_debug_reset_static_data();
        return result;
    }
    if (getenv("BACA_PDF_PTY_CHILD") != NULL) {
        const int result = baca_pdf_pty_child();
        cairo_debug_reset_static_data();
        return result;
    }
    if (!baca_test_support_init()) {
        return EXIT_FAILURE;
    }

    size_t common_count = 0U;
    size_t catalog_count = 0U;
    size_t comic_count = 0U;
    size_t config_count = 0U;
    size_t database_count = 0U;
    size_t document_count = 0U;
    size_t fb2_count = 0U;
    size_t graphics_count = 0U;
    size_t library_count = 0U;
    size_t library_shelf_count = 0U;
    size_t layout_count = 0U;
    size_t pdf_count = 0U;
    size_t remote_count = 0U;
    size_t search_count = 0U;
    size_t state_migration_count = 0U;
    size_t text_count = 0U;
    const BacaTestCase *common_cases = baca_common_test_cases(&common_count);
    const BacaTestCase *catalog_cases = baca_catalog_test_cases(&catalog_count);
    const BacaTestCase *comic_cases = baca_comic_test_cases(&comic_count);
    const BacaTestCase *config_cases = baca_config_test_cases(&config_count);
    const BacaTestCase *database_cases = baca_database_test_cases(&database_count);
    const BacaTestCase *document_cases = baca_document_test_cases(&document_count);
    const BacaTestCase *fb2_cases = baca_fb2_test_cases(&fb2_count);
    const BacaTestCase *graphics_cases = baca_graphics_test_cases(&graphics_count);
    const BacaTestCase *library_cases = baca_library_test_cases(&library_count);
    const BacaTestCase *library_shelf_cases = baca_library_shelf_test_cases(&library_shelf_count);
    const BacaTestCase *layout_cases = baca_layout_test_cases(&layout_count);
    const BacaTestCase *pdf_cases = baca_pdf_test_cases(&pdf_count);
    const BacaTestCase *remote_cases = baca_remote_test_cases(&remote_count);
    const BacaTestCase *search_cases = baca_search_test_cases(&search_count);
    const BacaTestCase *state_migration_cases = baca_state_migration_test_cases(&state_migration_count);
    const BacaTestCase *text_cases = baca_text_test_cases(&text_count);
    const BacaTestSuite suites[] = {
        {.name = "common", .cases = common_cases, .count = common_count},
        {.name = "catalog", .cases = catalog_cases, .count = catalog_count},
        {.name = "comic", .cases = comic_cases, .count = comic_count},
        {.name = "config", .cases = config_cases, .count = config_count},
        {.name = "database", .cases = database_cases, .count = database_count},
        {.name = "document", .cases = document_cases, .count = document_count},
        {.name = "fb2", .cases = fb2_cases, .count = fb2_count},
        {.name = "graphics", .cases = graphics_cases, .count = graphics_count},
        {.name = "library", .cases = library_cases, .count = library_count},
        {.name = "library_shelf", .cases = library_shelf_cases, .count = library_shelf_count},
        {.name = "layout", .cases = layout_cases, .count = layout_count},
        {.name = "pdf", .cases = pdf_cases, .count = pdf_count},
        {.name = "remote", .cases = remote_cases, .count = remote_count},
        {.name = "search", .cases = search_cases, .count = search_count},
        {.name = "text", .cases = text_cases, .count = text_count},
        {.name = "state_migration", .cases = state_migration_cases, .count = state_migration_count},
    };
    int result = baca_test_run(suites, BACA_ARRAY_LEN(suites));
    baca_test_support_cleanup();
    cairo_debug_reset_static_data();
    FcFini();
    return result;
}
