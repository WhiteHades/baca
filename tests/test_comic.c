#include "test_support.h"

#include "mereader-tui/document.h"
#include "mereader-tui/layout.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zip.h>

static const unsigned char comic_pixel_png[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U,
    0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 0x04U, 0x00U, 0x00U,
    0x00U, 0xb5U, 0x1cU, 0x0cU, 0x02U, 0x00U, 0x00U, 0x00U, 0x0bU, 0x49U, 0x44U, 0x41U, 0x54U, 0x78U,
    0xdaU, 0x63U, 0x64U, 0xf8U, 0x0fU, 0x00U, 0x01U, 0x05U, 0x01U, 0x01U, 0x27U, 0x18U, 0xe3U, 0x66U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
};

static const unsigned char generated_minimal_cbr[] = {
    0x52U, 0x61U, 0x72U, 0x21U, 0x1aU, 0x07U, 0x01U, 0x00U, 0x33U, 0x92U, 0xb5U, 0xe5U,
    0x0aU, 0x01U, 0x05U, 0x06U, 0x00U, 0x05U, 0x01U, 0x01U, 0x80U, 0x80U, 0x00U, 0x3cU,
    0x57U, 0xbdU, 0xc6U, 0x26U, 0x02U, 0x03U, 0x0bU, 0xe1U, 0x00U, 0x04U, 0xf0U, 0x00U,
    0xa4U, 0x83U, 0x02U, 0x9cU, 0x7fU, 0x34U, 0x09U, 0x80U, 0x03U, 0x01U, 0x08U, 0x70U,
    0x61U, 0x67U, 0x65U, 0x2eU, 0x73U, 0x76U, 0x67U, 0x0aU, 0x03U, 0x13U, 0x3bU, 0x20U,
    0x62U, 0x6aU, 0x1cU, 0xa1U, 0x39U, 0x04U, 0xc3U, 0xc7U, 0x5eU, 0x30U, 0x44U, 0x42U,
    0x3fU, 0x74U, 0x03U, 0x39U, 0xddU, 0x15U, 0xc8U, 0x73U, 0x9cU, 0xfcU, 0x46U, 0xd0U,
    0xddU, 0x32U, 0xd8U, 0xc0U, 0x48U, 0x7eU, 0x10U, 0x72U, 0x7cU, 0xefU, 0xf8U, 0x38U,
    0x26U, 0x29U, 0x4dU, 0x87U, 0x71U, 0x2dU, 0xc5U, 0xb5U, 0x4eU, 0xe4U, 0x3aU, 0xb3U,
    0x2aU, 0x66U, 0x4fU, 0xe7U, 0x07U, 0xfdU, 0x47U, 0xb9U, 0xeaU, 0xa7U, 0x91U, 0x7bU,
    0xa0U, 0x89U, 0x99U, 0xacU, 0xf3U, 0x5fU, 0x92U, 0x70U, 0xe5U, 0x82U, 0x41U, 0xa3U,
    0x94U, 0x07U, 0x0eU, 0x09U, 0xe8U, 0x98U, 0xa0U, 0xbdU, 0xb0U, 0x72U, 0x2dU, 0x4bU,
    0x6bU, 0x80U, 0x19U, 0x26U, 0xeeU, 0x56U, 0xf5U, 0x0eU, 0x61U, 0x18U, 0xa0U, 0x5fU,
    0x4eU, 0x14U, 0x28U, 0xccU, 0xafU, 0x74U, 0xa0U, 0x1dU, 0x77U, 0x56U, 0x51U, 0x03U,
    0x05U, 0x04U, 0x00U,
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
    char *path = mereader_tui_test_path(relative);
    if (path == NULL) {
        return COMIC_FIXTURE_FAILED;
    }
    MereaderTuiError error = {0};
    char *directory = mereader_tui_path_dirname(path, &error);
    if (directory == NULL || !mereader_tui_mkdirs(directory, &error)) {
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

static MereaderTuiTestResult test_cbz_natural_order_resources_probe_and_held_archive(void) {
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
        comic_create_fixture("comic/natural.cbz", COMIC_FIXTURE_ZIP, entries, MEREADER_TUI_ARRAY_LEN(entries), &path),
        COMIC_FIXTURE_OK);
    TEST_ASSERT(path != NULL);

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_COMIC);
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

    MereaderTuiResource resource = {0};
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, "comic://page/0.png", &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(comic_pixel_png));
    TEST_ASSERT(memcmp(resource.data, comic_pixel_png, sizeof(comic_pixel_png)) == 0);
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    resource.data[0] = 0U;
    mereader_tui_resource_free(&resource);
    TEST_ASSERT(mereader_tui_document_load_resource(&document, "comic://page/0.png", &resource, &error));
    TEST_ASSERT_INT(resource.data[0], 0x89);
    mereader_tui_resource_free(&resource);
    TEST_ASSERT(!mereader_tui_document_load_resource(&document, "comic://page/0.jpg", &resource, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_NOT_FOUND);

    mereader_tui_document_probe_images(&document, true);
    for (size_t index = 0U; index < document.block_count; ++index) {
        TEST_ASSERT(!document.blocks[index].value.image.broken);
        TEST_ASSERT_INT(document.blocks[index].value.image.intrinsic_width, 1);
        TEST_ASSERT_INT(document.blocks[index].value.image.intrinsic_height, 1);
    }
    MereaderTuiLayout layout = {0};
    TEST_ASSERT_MSG(mereader_tui_layout_build(&layout, &document, 40, MEREADER_TUI_JUSTIFY_LEFT, &error), "%s", error.message);
    TEST_ASSERT_SIZE(mereader_tui_layout_target_line(&layout, "comic://page/3"), 63U);
    mereader_tui_layout_free(&layout);

    char *held = mereader_tui_test_path("comic/natural-held.cbz");
    TEST_ASSERT(held != NULL);
    (void)unlink(held);
    TEST_ASSERT(rename(path, held) == 0);
    TEST_ASSERT(mereader_tui_test_write_text("comic/natural.cbz", "replacement"));
    TEST_ASSERT(mereader_tui_document_load_resource(&document, "comic://page/1.png", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, sizeof(comic_pixel_png));
    TEST_ASSERT_INT(resource.data[0], 0x89);
    mereader_tui_resource_free(&resource);

    mereader_tui_document_close(&document);
    free(held);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_duplicate_member_names_keep_physical_identity(void) {
    static const char first[] = "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'/>";
    static const char second[] = "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='1'/>";
    const ComicFixtureEntry entries[] = {
        {.path = "same.svg", .data = first, .length = sizeof(first) - 1U},
        {.path = "same.svg", .data = second, .length = sizeof(second) - 1U},
    };
    char *path = NULL;
    TEST_ASSERT_INT(
        comic_create_fixture("comic/duplicates.cbz", COMIC_FIXTURE_ZIP, entries, MEREADER_TUI_ARRAY_LEN(entries), &path),
        COMIC_FIXTURE_OK);
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.block_count, 2U);
    MereaderTuiResource resource = {0};
    TEST_ASSERT(mereader_tui_document_load_resource(&document, "comic://page/0.svg", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, sizeof(first) - 1U);
    TEST_ASSERT(memcmp(resource.data, first, sizeof(first) - 1U) == 0);
    mereader_tui_resource_free(&resource);
    TEST_ASSERT(mereader_tui_document_load_resource(&document, "comic://page/1.svg", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, sizeof(second) - 1U);
    TEST_ASSERT(memcmp(resource.data, second, sizeof(second) - 1U) == 0);
    mereader_tui_resource_free(&resource);
    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_cb7_magic_detection(void) {
    const ComicFixtureEntry entry = {
        .path = "page.png",
        .data = comic_pixel_png,
        .length = sizeof(comic_pixel_png),
    };
    char *path = NULL;
    const ComicFixtureResult created =
        comic_create_fixture("comic/seven-zip.unknown", COMIC_FIXTURE_7ZIP, &entry, 1U, &path);
    TEST_ASSERT_INT(created, COMIC_FIXTURE_OK);
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_COMIC);
    TEST_ASSERT_STR(document.metadata.format, "CB7");
    TEST_ASSERT_SIZE(document.block_count, 1U);
    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_empty_unsupported_and_malformed_archives(void) {
    static const ComicFixtureEntry text_entry = {
        .path = "readme.txt",
        .data = "no pages",
        .length = 8U,
    };
    char *path = NULL;
    TEST_ASSERT_INT(comic_create_fixture("comic/empty.cbz", COMIC_FIXTURE_ZIP, &text_entry, 1U, &path),
                    COMIC_FIXTURE_OK);
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    free(path);

    const ComicFixtureEntry image_entry = {
        .path = "page.png",
        .data = comic_pixel_png,
        .length = sizeof(comic_pixel_png),
    };
    TEST_ASSERT_INT(comic_create_fixture("comic/not-comic.cbr", COMIC_FIXTURE_TAR, &image_entry, 1U, &path),
                    COMIC_FIXTURE_OK);
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    free(path);

    static const unsigned char truncated_rar[] = {'R', 'a', 'r', '!', 0x1aU, 0x07U, 0x00U};
    TEST_ASSERT(mereader_tui_test_write("comic/truncated.bin", truncated_rar, sizeof(truncated_rar)));
    path = mereader_tui_test_path("comic/truncated.bin");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    free(path);

    static const unsigned char truncated_rar5[] = {'R', 'a', 'r', '!', 0x1aU, 0x07U, 0x01U, 0x00U};
    TEST_ASSERT(mereader_tui_test_write("comic/truncated-rar5.bin", truncated_rar5, sizeof(truncated_rar5)));
    path = mereader_tui_test_path("comic/truncated-rar5.bin");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_in_place_archive_mutation_is_rejected(void) {
    const ComicFixtureEntry entry = {
        .path = "page.png",
        .data = comic_pixel_png,
        .length = sizeof(comic_pixel_png),
    };
    char *path = NULL;
    TEST_ASSERT_INT(comic_create_fixture("comic/mutate.cbz", COMIC_FIXTURE_ZIP, &entry, 1U, &path), COMIC_FIXTURE_OK);
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(mereader_tui_document_open(&document, path, &error));
    struct stat status;
    TEST_ASSERT(stat(path, &status) == 0 && status.st_size > 0);
    FILE *file = fopen(path, "r+b");
    TEST_ASSERT(file != NULL);
    TEST_ASSERT(fputc('x', file) != EOF);
    TEST_ASSERT(fclose(file) == 0);
    MereaderTuiResource resource = {0};
    TEST_ASSERT(!mereader_tui_document_load_resource(&document, "comic://page/0.png", &resource, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_encrypted_comic_is_rejected(void) {
    char *path = mereader_tui_test_path("comic/encrypted.cbz");
    TEST_ASSERT(path != NULL);
    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    TEST_ASSERT(archive != NULL);
    zip_source_t *source = zip_source_buffer(archive, comic_pixel_png, sizeof(comic_pixel_png), 0);
    TEST_ASSERT(source != NULL);
    const zip_int64_t index = zip_file_add(archive, "page.png", source, ZIP_FL_ENC_UTF_8);
    TEST_ASSERT(index >= 0);
    TEST_ASSERT(zip_file_set_encryption(archive, (zip_uint64_t)index, ZIP_EM_AES_256,
                                        "generated-test-password") == 0);
    TEST_ASSERT(zip_close(archive) == 0);

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    TEST_ASSERT(strstr(error.message, "encrypted comic archives") != NULL);
    TEST_ASSERT(document.path == NULL && document.backend == NULL);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_generated_cbr_decodes_real_rar(void) {
    TEST_ASSERT(mereader_tui_test_write("comic/generated.cbr", generated_minimal_cbr,
                                        sizeof(generated_minimal_cbr)));
    char *path = mereader_tui_test_path("comic/generated.cbr");
    TEST_ASSERT(path != NULL);

    MereaderTuiError error = {0};
    MereaderTuiDocument document = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_COMIC);
    TEST_ASSERT_STR(document.metadata.format, "CBR");
    TEST_ASSERT(document.block_count > 0U);
    TEST_ASSERT(document.blocks[0].kind == MEREADER_TUI_BLOCK_IMAGE);

    MereaderTuiResource resource = {0};
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, document.blocks[0].value.image.uri, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT(resource.length > 0U);
    TEST_ASSERT(strncmp(resource.mime_type, "image/", 6U) == 0);
    mereader_tui_resource_free(&resource);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);

    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_comic_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "cbz_natural_order_resources_probe_and_held_archive",
         .function = test_cbz_natural_order_resources_probe_and_held_archive},
        {.name = "duplicate_member_names_keep_physical_identity",
         .function = test_duplicate_member_names_keep_physical_identity},
        {.name = "cb7_magic_detection", .function = test_cb7_magic_detection},
        {.name = "empty_unsupported_and_malformed_archives", .function = test_empty_unsupported_and_malformed_archives},
        {.name = "in_place_archive_mutation_is_rejected", .function = test_in_place_archive_mutation_is_rejected},
        {.name = "encrypted_comic_is_rejected", .function = test_encrypted_comic_is_rejected},
        {.name = "generated_cbr_decodes_real_rar",
         .function = test_generated_cbr_decodes_real_rar},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
