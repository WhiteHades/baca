#include "test_support.h"

#include "baca/document.h"
#include "baca/layout.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char comic_pixel_png[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U,
    0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 0x04U, 0x00U, 0x00U,
    0x00U, 0xb5U, 0x1cU, 0x0cU, 0x02U, 0x00U, 0x00U, 0x00U, 0x0bU, 0x49U, 0x44U, 0x41U, 0x54U, 0x78U,
    0xdaU, 0x63U, 0x64U, 0xf8U, 0x0fU, 0x00U, 0x01U, 0x05U, 0x01U, 0x01U, 0x27U, 0x18U, 0xe3U, 0x66U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
};

typedef enum ComicFixtureFormat {
    COMIC_FIXTURE_ZIP = 0,
    COMIC_FIXTURE_7ZIP,
    COMIC_FIXTURE_TAR,
} ComicFixtureFormat;

typedef enum ComicFixtureResult {
    COMIC_FIXTURE_FAILED = -1,
    COMIC_FIXTURE_OK = 0,
    COMIC_FIXTURE_UNSUPPORTED,
} ComicFixtureResult;

typedef struct ComicFixtureEntry {
    const char *path;
    const void *data;
    size_t length;
    mode_t filetype;
    const char *symlink;
} ComicFixtureEntry;

static bool comic_write_all(struct archive *archive, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length > 0U) {
        const la_ssize_t written = archive_write_data(archive, cursor, length);
        if (written <= 0) {
            return false;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return true;
}

static ComicFixtureResult comic_create_fixture(const char *relative, ComicFixtureFormat format,
                                               const ComicFixtureEntry *entries, size_t entry_count,
                                               char **output_path) {
    *output_path = NULL;
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return COMIC_FIXTURE_FAILED;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return COMIC_FIXTURE_FAILED;
    }
    free(directory);

    struct archive *archive = archive_write_new();
    if (archive == NULL) {
        free(path);
        return COMIC_FIXTURE_FAILED;
    }
    int format_status = ARCHIVE_FATAL;
    if (format == COMIC_FIXTURE_ZIP) {
        format_status = archive_write_set_format_zip(archive);
    } else if (format == COMIC_FIXTURE_7ZIP) {
        format_status = archive_write_set_format_7zip(archive);
    } else {
        format_status = archive_write_set_format_pax_restricted(archive);
    }
    if (format_status != ARCHIVE_OK) {
        (void)archive_write_free(archive);
        free(path);
        return format == COMIC_FIXTURE_7ZIP ? COMIC_FIXTURE_UNSUPPORTED : COMIC_FIXTURE_FAILED;
    }
    if (archive_write_open_filename(archive, path) != ARCHIVE_OK) {
        (void)archive_write_free(archive);
        free(path);
        return COMIC_FIXTURE_FAILED;
    }

    bool success = true;
    for (size_t index = 0U; index < entry_count && success; ++index) {
        const ComicFixtureEntry *fixture = &entries[index];
        struct archive_entry *entry = archive_entry_new();
        if (entry == NULL) {
            success = false;
            break;
        }
        archive_entry_set_pathname(entry, fixture->path);
        const mode_t filetype = fixture->filetype == 0 ? AE_IFREG : fixture->filetype;
        archive_entry_set_filetype(entry, filetype);
        archive_entry_set_perm(entry, filetype == AE_IFDIR ? 0700 : 0600);
        archive_entry_set_size(entry, filetype == AE_IFREG ? (la_int64_t)fixture->length : 0);
        if (fixture->symlink != NULL) {
            archive_entry_set_symlink(entry, fixture->symlink);
        }
        success = archive_write_header(archive, entry) == ARCHIVE_OK;
        if (success && filetype == AE_IFREG && fixture->length > 0U) {
            success = comic_write_all(archive, fixture->data, fixture->length);
        }
        archive_entry_free(entry);
    }
    const int close_status = archive_write_close(archive);
    const int free_status = archive_write_free(archive);
    if (!success || close_status != ARCHIVE_OK || free_status != ARCHIVE_OK) {
        (void)unlink(path);
        free(path);
        return COMIC_FIXTURE_FAILED;
    }
    *output_path = path;
    return COMIC_FIXTURE_OK;
}

static BacaTestResult test_cbz_natural_order_resources_probe_and_held_archive(void) {
    static const ComicFixtureEntry entries[] = {
        {.path = "10.png", .data = comic_pixel_png, .length = sizeof(comic_pixel_png)},
        {.path = "notes.txt", .data = "ignored", .length = 7U},
        {.path = "nested", .filetype = AE_IFDIR},
        {.path = "nested/01.png", .data = comic_pixel_png, .length = sizeof(comic_pixel_png)},
        {.path = "2.png", .data = comic_pixel_png, .length = sizeof(comic_pixel_png)},
        {.path = "cover.jpg", .data = comic_pixel_png, .length = sizeof(comic_pixel_png)},
        {.path = "../escape.png", .data = comic_pixel_png, .length = sizeof(comic_pixel_png)},
        {.path = "linked.png", .filetype = AE_IFLNK, .symlink = "2.png"},
    };
    char *path = NULL;
    TEST_ASSERT_INT(
        comic_create_fixture("comic/natural.cbz", COMIC_FIXTURE_ZIP, entries, BACA_ARRAY_LEN(entries), &path),
        COMIC_FIXTURE_OK);
    TEST_ASSERT(path != NULL);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_COMIC);
    TEST_ASSERT_STR(document.metadata.title, "natural.cbz");
    TEST_ASSERT_STR(document.metadata.format, "CBZ");
    TEST_ASSERT_SIZE(document.block_count, 4U);
    TEST_ASSERT_SIZE(document.section_count, 4U);
    TEST_ASSERT_SIZE(document.toc_count, 4U);
    TEST_ASSERT_STR(document.blocks[0].value.image.alt, "2.png");
    TEST_ASSERT_STR(document.blocks[1].value.image.alt, "10.png");
    TEST_ASSERT_STR(document.blocks[2].value.image.alt, "cover.jpg");
    TEST_ASSERT_STR(document.blocks[3].value.image.alt, "01.png");
    TEST_ASSERT_STR(document.blocks[0].value.image.uri, "comic://page/0.png");
    TEST_ASSERT_STR(document.blocks[2].value.image.uri, "comic://page/2.jpg");
    TEST_ASSERT_SIZE(document.toc[3].section_index, 3U);

    BacaResource resource = {0};
    TEST_ASSERT_MSG(baca_document_load_resource(&document, "comic://page/0.png", &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(comic_pixel_png));
    TEST_ASSERT(memcmp(resource.data, comic_pixel_png, sizeof(comic_pixel_png)) == 0);
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    resource.data[0] = 0U;
    baca_resource_free(&resource);
    TEST_ASSERT(baca_document_load_resource(&document, "comic://page/0.png", &resource, &error));
    TEST_ASSERT_INT(resource.data[0], 0x89);
    baca_resource_free(&resource);
    TEST_ASSERT(!baca_document_load_resource(&document, "comic://page/0.jpg", &resource, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_NOT_FOUND);

    baca_document_probe_images(&document, true);
    for (size_t index = 0U; index < document.block_count; ++index) {
        TEST_ASSERT(!document.blocks[index].value.image.broken);
        TEST_ASSERT_INT(document.blocks[index].value.image.intrinsic_width, 1);
        TEST_ASSERT_INT(document.blocks[index].value.image.intrinsic_height, 1);
    }
    BacaLayout layout = {0};
    TEST_ASSERT_MSG(baca_layout_build(&layout, &document, 40, BACA_JUSTIFY_LEFT, &error), "%s", error.message);
    TEST_ASSERT_SIZE(baca_layout_target_line(&layout, "comic://page/3"), 63U);
    baca_layout_free(&layout);

    char *held = baca_test_path("comic/natural-held.cbz");
    TEST_ASSERT(held != NULL);
    (void)unlink(held);
    TEST_ASSERT(rename(path, held) == 0);
    TEST_ASSERT(baca_test_write_text("comic/natural.cbz", "replacement"));
    TEST_ASSERT(baca_document_load_resource(&document, "comic://page/1.png", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, sizeof(comic_pixel_png));
    TEST_ASSERT_INT(resource.data[0], 0x89);
    baca_resource_free(&resource);

    baca_document_close(&document);
    free(held);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_duplicate_member_names_keep_physical_identity(void) {
    static const char first[] = "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'/>";
    static const char second[] = "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='1'/>";
    const ComicFixtureEntry entries[] = {
        {.path = "same.svg", .data = first, .length = sizeof(first) - 1U},
        {.path = "same.svg", .data = second, .length = sizeof(second) - 1U},
    };
    char *path = NULL;
    TEST_ASSERT_INT(
        comic_create_fixture("comic/duplicates.cbz", COMIC_FIXTURE_ZIP, entries, BACA_ARRAY_LEN(entries), &path),
        COMIC_FIXTURE_OK);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.block_count, 2U);
    BacaResource resource = {0};
    TEST_ASSERT(baca_document_load_resource(&document, "comic://page/0.svg", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, sizeof(first) - 1U);
    TEST_ASSERT(memcmp(resource.data, first, sizeof(first) - 1U) == 0);
    baca_resource_free(&resource);
    TEST_ASSERT(baca_document_load_resource(&document, "comic://page/1.svg", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, sizeof(second) - 1U);
    TEST_ASSERT(memcmp(resource.data, second, sizeof(second) - 1U) == 0);
    baca_resource_free(&resource);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_cb7_magic_detection(void) {
    const ComicFixtureEntry entry = {
        .path = "page.png",
        .data = comic_pixel_png,
        .length = sizeof(comic_pixel_png),
    };
    char *path = NULL;
    const ComicFixtureResult created =
        comic_create_fixture("comic/seven-zip.unknown", COMIC_FIXTURE_7ZIP, &entry, 1U, &path);
    if (created == COMIC_FIXTURE_UNSUPPORTED) {
        return baca_test_skip("installed libarchive cannot write 7-Zip fixtures");
    }
    TEST_ASSERT_INT(created, COMIC_FIXTURE_OK);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_COMIC);
    TEST_ASSERT_STR(document.metadata.format, "CB7");
    TEST_ASSERT_SIZE(document.block_count, 1U);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_empty_unsupported_and_malformed_archives(void) {
    static const ComicFixtureEntry text_entry = {
        .path = "readme.txt",
        .data = "no pages",
        .length = 8U,
    };
    char *path = NULL;
    TEST_ASSERT_INT(comic_create_fixture("comic/empty.cbz", COMIC_FIXTURE_ZIP, &text_entry, 1U, &path),
                    COMIC_FIXTURE_OK);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    free(path);

    const ComicFixtureEntry image_entry = {
        .path = "page.png",
        .data = comic_pixel_png,
        .length = sizeof(comic_pixel_png),
    };
    TEST_ASSERT_INT(comic_create_fixture("comic/not-comic.cbr", COMIC_FIXTURE_TAR, &image_entry, 1U, &path),
                    COMIC_FIXTURE_OK);
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
    free(path);

    static const unsigned char truncated_rar[] = {'R', 'a', 'r', '!', 0x1aU, 0x07U, 0x00U};
    TEST_ASSERT(baca_test_write("comic/truncated.bin", truncated_rar, sizeof(truncated_rar)));
    path = baca_test_path("comic/truncated.bin");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    free(path);

    static const unsigned char truncated_rar5[] = {'R', 'a', 'r', '!', 0x1aU, 0x07U, 0x01U, 0x00U};
    TEST_ASSERT(baca_test_write("comic/truncated-rar5.bin", truncated_rar5, sizeof(truncated_rar5)));
    path = baca_test_path("comic/truncated-rar5.bin");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_in_place_archive_mutation_is_rejected(void) {
    const ComicFixtureEntry entry = {
        .path = "page.png",
        .data = comic_pixel_png,
        .length = sizeof(comic_pixel_png),
    };
    char *path = NULL;
    TEST_ASSERT_INT(comic_create_fixture("comic/mutate.cbz", COMIC_FIXTURE_ZIP, &entry, 1U, &path), COMIC_FIXTURE_OK);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(baca_document_open(&document, path, &error));
    struct stat status;
    TEST_ASSERT(stat(path, &status) == 0 && status.st_size > 0);
    FILE *file = fopen(path, "r+b");
    TEST_ASSERT(file != NULL);
    TEST_ASSERT(fputc('x', file) != EOF);
    TEST_ASSERT(fclose(file) == 0);
    BacaResource resource = {0};
    TEST_ASSERT(!baca_document_load_resource(&document, "comic://page/0.png", &resource, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_optional_real_cbr(void) {
    const char *configured = getenv("MEREADER_TUI_TEST_CBR_SAMPLE");
    if (configured == NULL || configured[0] == '\0') {
        return baca_test_skip("set MEREADER_TUI_TEST_CBR_SAMPLE for real RAR decoding");
    }
    BacaError error = {0};
    char *path = baca_realpath(configured, &error);
    TEST_ASSERT_MSG(path != NULL, "%s", error.message);

    BacaDocument document = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_COMIC);
    TEST_ASSERT_STR(document.metadata.format, "CBR");
    TEST_ASSERT(document.block_count > 0U);
    TEST_ASSERT(document.blocks[0].kind == BACA_BLOCK_IMAGE);

    BacaResource resource = {0};
    TEST_ASSERT_MSG(baca_document_load_resource(&document, document.blocks[0].value.image.uri, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT(resource.length > 0U);
    TEST_ASSERT(strncmp(resource.mime_type, "image/", 6U) == 0);
    baca_resource_free(&resource);
    baca_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);

    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_comic_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "cbz_natural_order_resources_probe_and_held_archive",
         .function = test_cbz_natural_order_resources_probe_and_held_archive},
        {.name = "duplicate_member_names_keep_physical_identity",
         .function = test_duplicate_member_names_keep_physical_identity},
        {.name = "cb7_magic_detection", .function = test_cb7_magic_detection},
        {.name = "empty_unsupported_and_malformed_archives", .function = test_empty_unsupported_and_malformed_archives},
        {.name = "in_place_archive_mutation_is_rejected", .function = test_in_place_archive_mutation_is_rejected},
        {.name = "optional_real_cbr", .function = test_optional_real_cbr},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
