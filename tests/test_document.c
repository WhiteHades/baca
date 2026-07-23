#include "test_support.h"

#include "mereader-tui/document.h"
#include "mereader-tui/document_backend.h"
#include "mereader-tui/graphics.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zip.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <gdk-pixbuf/gdk-pixbuf.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

typedef struct FixtureMember {
    const char *name;
    const void *data;
    size_t length;
} FixtureMember;

static const char container_xml[] =
    "<?xml version=\"1.0\"?>"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
    "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

static const unsigned char pixel_png[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
    0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x08U, 0x04U, 0x00U, 0x00U, 0x00U, 0xb5U, 0x1cU, 0x0cU,
    0x02U, 0x00U, 0x00U, 0x00U, 0x0bU, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xdaU, 0x63U, 0x64U, 0xf8U, 0x0fU, 0x00U,
    0x01U, 0x05U, 0x01U, 0x01U, 0x27U, 0x18U, 0xe3U, 0x66U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U,
    0xaeU, 0x42U, 0x60U, 0x82U,
};

static const char pixel_jpeg_data_uri[] =
    "data:image/jpeg;base64,/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgICAgMCAgIDAwMDBAYEBAQEBAgGBgUGCQgKCgkICQkKDA8MCgsOCwkJDRENDg8QEBEQCgwSExIQEw8QEBD/2wBDAQMDAwQDBAgEBAgQCwkLEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBD/wAARCAADAAIDAREAAhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAACP/EABQQAQAAAAAAAAAAAAAAAAAAAAD/xAAVAQEBAAAAAAAAAAAAAAAAAAAHCf/EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAMAwEAAhEDEQA/ADoDFU3/2Q==";

static const unsigned char pixel_gif[] = {
    0x47U, 0x49U, 0x46U, 0x38U, 0x39U, 0x61U, 0x02U, 0x00U, 0x03U, 0x00U, 0xf0U,
    0x00U, 0x00U, 0xffU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x21U, 0xf9U, 0x04U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x2cU, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x03U, 0x00U, 0x00U, 0x02U, 0x02U, 0x84U, 0x5fU, 0x00U, 0x3bU,
};

static const unsigned char pixel_webp[] = {
    0x52U, 0x49U, 0x46U, 0x46U, 0x3cU, 0x00U, 0x00U, 0x00U, 0x57U, 0x45U, 0x42U,
    0x50U, 0x56U, 0x50U, 0x38U, 0x20U, 0x30U, 0x00U, 0x00U, 0x00U, 0xd0U, 0x01U,
    0x00U, 0x9dU, 0x01U, 0x2aU, 0x02U, 0x00U, 0x03U, 0x00U, 0x02U, 0x00U, 0x34U,
    0x25U, 0xa0U, 0x02U, 0x74U, 0xbaU, 0x01U, 0xf8U, 0x00U, 0x03U, 0xb0U, 0x00U,
    0xfeU, 0xf0U, 0xc4U, 0x0bU, 0xffU, 0x20U, 0xb9U, 0x61U, 0x75U, 0xc8U, 0xd7U,
    0xffU, 0x20U, 0x3fU, 0xe4U, 0x07U, 0xfcU, 0x80U, 0xffU, 0xf8U, 0xf2U, 0x00U,
    0x00U, 0x00U,
};

static const unsigned char pixel_bmp[] = {
    0x42U, 0x4dU, 0x4eU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x36U,
    0x00U, 0x00U, 0x00U, 0x28U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U,
    0x03U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x18U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x18U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0x00U, 0x00U, 0x00U, 0xffU, 0x00U,
    0x00U, 0xffU, 0x00U, 0x00U, 0x00U, 0x00U, 0xffU, 0x00U, 0x00U, 0xffU, 0x00U,
    0x00U, 0x00U,
};

static const unsigned char pixel_svg[] =
    "\xef\xbb\xbf<?xml version='1.0'?>\n<!-- bounded prefix -->\n"
    "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='3'>"
    "<rect width='2' height='3' fill='red'/></svg>";

typedef struct ProbeBackend {
    size_t loads;
    size_t resource_size;
    const char *override_uri;
    size_t override_size;
    bool corrupt;
} ProbeBackend;

typedef struct StandaloneFormatFixture {
    const char *name;
    const char *extension;
    const char *loader;
    const unsigned char *data;
    size_t length;
} StandaloneFormatFixture;

static bool probe_load_resource(MereaderTuiDocument *document, const char *uri, MereaderTuiResource *resource,
                                MereaderTuiError *error) {
    (void)uri;
    ProbeBackend *backend = document->backend;
    ++backend->loads;
    const size_t allocation = backend->corrupt ? 1U : sizeof(pixel_png);
    resource->data = malloc(allocation);
    resource->mime_type = mereader_tui_strdup("image/png", error);
    if (resource->data == NULL || resource->mime_type == NULL) {
        mereader_tui_resource_free(resource);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "cannot allocate probe fixture");
        return false;
    }
    if (!backend->corrupt) {
        memcpy(resource->data, pixel_png, sizeof(pixel_png));
    } else {
        resource->data[0] = 'x';
    }
    resource->length = allocation;
    return true;
}

static bool probe_resource_size(const MereaderTuiDocument *document, const char *uri, size_t *size) {
    const ProbeBackend *backend = document->backend;
    if (backend == NULL || uri == NULL || size == NULL) {
        return false;
    }
    *size = backend->override_uri != NULL && strcmp(uri, backend->override_uri) == 0 ?
                backend->override_size :
                backend->resource_size;
    return true;
}

static const MereaderTuiDocumentOps probe_document_ops = {
    .load_resource = probe_load_resource,
    .resource_size = probe_resource_size,
};

static bool patch_member_size(const char *path, const char *member_name, uint32_t size);

static bool append_probe_image(MereaderTuiDocument *document, const char *uri, MereaderTuiError *error) {
    MereaderTuiBlock block = {
        .kind = MEREADER_TUI_BLOCK_IMAGE,
        .value.image = {
            .uri = mereader_tui_strdup(uri, error),
            .alt = mereader_tui_strdup(uri, error),
            .page_index = -1,
        },
    };
    if (block.value.image.uri != NULL && block.value.image.alt != NULL &&
        mereader_tui_document_add_image_block(document, &block, error)) {
        return true;
    }
    free(block.value.image.uri);
    free(block.value.image.alt);
    return false;
}

static bool buffer_contains(const unsigned char *data, size_t length, const void *needle, size_t needle_length) {
    if (needle_length == 0U) {
        return true;
    }
    if (data == NULL || needle == NULL || needle_length > length) {
        return false;
    }
    for (size_t offset = 0U; offset <= length - needle_length; offset++) {
        if (memcmp(data + offset, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static MereaderTuiTestResult test_valid_standalone_format(const StandaloneFormatFixture *fixture) {
    bool loader_available = false;
    GSList *formats = gdk_pixbuf_get_formats();
    for (GSList *item = formats; item != NULL; item = item->next) {
        gchar *name = gdk_pixbuf_format_get_name(item->data);
        const bool matches = name != NULL && strcmp(name, fixture->loader) == 0;
        g_free(name);
        if (matches) {
            loader_available = true;
            break;
        }
    }
    g_slist_free(formats);
    if (!loader_available) {
        return mereader_tui_test_fail_at(__FILE__, __LINE__, "GdkPixbuf %s loader unavailable",
                                         fixture->loader);
    }

    char target_relative[96] = {0};
    char alias_relative[96] = {0};
    const int target_length = snprintf(target_relative, sizeof(target_relative),
                                       "document/valid-%s.unknown", fixture->name);
    const int alias_length = snprintf(alias_relative, sizeof(alias_relative),
                                      "document/valid-%s-alias.%s", fixture->name, fixture->extension);
    TEST_ASSERT(target_length > 0 && (size_t)target_length < sizeof(target_relative));
    TEST_ASSERT(alias_length > 0 && (size_t)alias_length < sizeof(alias_relative));
    TEST_ASSERT(mereader_tui_test_write(target_relative, fixture->data, fixture->length));
    char *target = mereader_tui_test_path(target_relative);
    char *alias = mereader_tui_test_path(alias_relative);
    TEST_ASSERT(target != NULL && alias != NULL);

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, target, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_IMAGE);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 2);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 3);

    MereaderTuiResource resource = {0};
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, document.path, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, fixture->length);
    TEST_ASSERT(memcmp(resource.data, fixture->data, fixture->length) == 0);
    mereader_tui_resource_free(&resource);

    MereaderTuiGraphicsContext *graphics = mereader_tui_graphics_create(
        1024U * 1024U, MEREADER_TUI_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
    TEST_ASSERT(graphics != NULL);
    MereaderTuiGraphicsSurface surface = {0};
    TEST_ASSERT_MSG(mereader_tui_graphics_prepare(graphics, &document, 0U, 2, 2, &surface, &error), "%s",
                    error.message);
    TEST_ASSERT_INT(surface.width, 2);
    TEST_ASSERT_INT(surface.height, 4);
    mereader_tui_graphics_surface_release(&surface);
    mereader_tui_graphics_free(graphics);
    mereader_tui_document_close(&document);

    (void)unlink(alias);
    TEST_ASSERT(symlink(target, alias) == 0);
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, alias, &error), "%s", error.message);
    TEST_ASSERT_STR(document.path, target);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_IMAGE);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 2);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 3);
    mereader_tui_document_close(&document);
    free(alias);
    free(target);
    return MEREADER_TUI_TEST_PASS;
}

typedef struct DocumentOpenResult {
    MereaderTuiErrorCode code;
    bool opened;
} DocumentOpenResult;

static bool document_open_returns_without_blocking(const char *path, DocumentOpenResult *result) {
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0) {
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        return false;
    }
    if (child == 0) {
        (void)close(descriptors[0]);
        MereaderTuiDocument document = {0};
        MereaderTuiError error = {0};
        const bool opened = mereader_tui_document_open(&document, path, &error);
        if (opened) {
            mereader_tui_document_close(&document);
        }
        const DocumentOpenResult child_result = {
            .code = error.code,
            .opened = opened,
        };
        const unsigned char *cursor = (const unsigned char *)&child_result;
        size_t remaining = sizeof(child_result);
        while (remaining > 0U) {
            const ssize_t written = write(descriptors[1], cursor, remaining);
            if (written > 0) {
                cursor += (size_t)written;
                remaining -= (size_t)written;
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else {
                _exit(2);
            }
        }
        (void)close(descriptors[1]);
        _exit(0);
    }

    (void)close(descriptors[1]);
    struct pollfd descriptor = {.fd = descriptors[0], .events = POLLIN};
    int ready;
    do {
        ready = poll(&descriptor, 1U, 1000);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || (descriptor.revents & (POLLIN | POLLHUP)) == 0) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
        (void)close(descriptors[0]);
        return false;
    }

    unsigned char *cursor = (unsigned char *)result;
    size_t remaining = sizeof(*result);
    while (remaining > 0U) {
        const ssize_t count = read(descriptors[0], cursor, remaining);
        if (count > 0) {
            cursor += (size_t)count;
            remaining -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    (void)close(descriptors[0]);
    int status = 0;
    const bool waited = waitpid(child, &status, 0) == child;
    return remaining == 0U && waited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool zip_add_bytes(zip_t *archive, const char *name, const void *data, size_t length, bool stored,
                          zip_int64_t *added_index) {
    void *owned = malloc(length == 0U ? 1U : length);
    if (owned == NULL) {
        return false;
    }
    if (length != 0U) {
        memcpy(owned, data, length);
    }
    zip_source_t *source = zip_source_buffer(archive, owned, length, 1);
    if (source == NULL) {
        free(owned);
        return false;
    }
    zip_int64_t index = zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
    if (index < 0) {
        zip_source_free(source);
        return false;
    }
    if (stored && zip_set_file_compression(archive, (zip_uint64_t)index, ZIP_CM_STORE, 0U) != 0) {
        return false;
    }
    if (added_index != NULL) {
        *added_index = index;
    }
    return true;
}

static bool close_archive(zip_t *archive) {
    if (zip_close(archive) == 0) {
        return true;
    }
    fprintf(stderr, "EPUB fixture close: %s\n", zip_strerror(archive));
    zip_discard(archive);
    return false;
}

static bool create_numbered_probe_archive(const char *relative, const char *prefix, size_t member_count,
                                          const void *data, size_t data_length, char **output_path) {
    char *path = mereader_tui_test_path(relative);
    if (path == NULL) {
        return false;
    }
    MereaderTuiError error = {0};
    char *directory = mereader_tui_path_dirname(path, &error);
    if (directory == NULL || !mereader_tui_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return false;
    }
    free(directory);

    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    if (archive == NULL) {
        free(path);
        return false;
    }
    bool built = true;
    for (size_t index = 0U; index < member_count && built; ++index) {
        char name[64] = {0};
        const int length = snprintf(name, sizeof(name), "%s-%zu.png", prefix, index);
        built = length > 0 && (size_t)length < sizeof(name) &&
                zip_add_bytes(archive, name, data, data_length, false, NULL);
    }
    if (!built) {
        zip_discard(archive);
        free(path);
        return false;
    }
    if (!close_archive(archive)) {
        free(path);
        return false;
    }
    *output_path = path;
    return true;
}

static bool create_epub(const char *relative, const char *opf, const FixtureMember *members, size_t member_count,
                        char **output_path) {
    char *path = mereader_tui_test_path(relative);
    if (path == NULL) {
        return false;
    }
    MereaderTuiError error = {0};
    char *directory = mereader_tui_path_dirname(path, &error);
    if (directory == NULL || !mereader_tui_mkdirs(directory, &error)) {
        fprintf(stderr, "EPUB fixture directory: %s\n", error.message);
        free(directory);
        free(path);
        return false;
    }
    free(directory);

    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    if (archive == NULL) {
        fprintf(stderr, "EPUB fixture open failed with libzip error %d\n", zip_error);
        free(path);
        return false;
    }
    bool built = zip_add_bytes(archive, "mimetype", "application/epub+zip", 20U, true, NULL) &&
                 zip_add_bytes(archive, "META-INF/container.xml", container_xml, strlen(container_xml), false,
                               NULL) &&
                 zip_add_bytes(archive, "OEBPS/content.opf", opf, strlen(opf), false, NULL);
    for (size_t index = 0U; index < member_count && built; index++) {
        built = zip_add_bytes(archive, members[index].name, members[index].data, members[index].length, false, NULL);
    }
    if (!built) {
        fprintf(stderr, "EPUB fixture add: %s\n", zip_strerror(archive));
        zip_discard(archive);
        free(path);
        return false;
    }
    if (!close_archive(archive)) {
        free(path);
        return false;
    }
    *output_path = path;
    return true;
}

static bool document_has_anchor(const MereaderTuiDocument *document, const char *target) {
    for (size_t block_index = 0U; block_index < document->block_count; block_index++) {
        const MereaderTuiBlock *block = &document->blocks[block_index];
        if (block->kind == MEREADER_TUI_BLOCK_TEXT) {
            for (size_t anchor_index = 0U; anchor_index < block->value.text.anchor_count; anchor_index++) {
                if (strcmp(block->value.text.anchors[anchor_index].target, target) == 0) {
                    return true;
                }
            }
        } else if (block->kind == MEREADER_TUI_BLOCK_IMAGE && block->value.image.anchor != NULL &&
                   strcmp(block->value.image.anchor, target) == 0) {
            return true;
        }
    }
    return false;
}

static const MereaderTuiTextBlock *find_text(const MereaderTuiDocument *document, const char *text) {
    for (size_t index = 0U; index < document->block_count; index++) {
        if (document->blocks[index].kind == MEREADER_TUI_BLOCK_TEXT && document->blocks[index].value.text.text != NULL &&
            strcmp(document->blocks[index].value.text.text, text) == 0) {
            return &document->blocks[index].value.text;
        }
    }
    return NULL;
}

static const MereaderTuiImageBlock *find_image(const MereaderTuiDocument *document, const char *alt) {
    for (size_t index = 0U; index < document->block_count; index++) {
        if (document->blocks[index].kind == MEREADER_TUI_BLOCK_IMAGE && document->blocks[index].value.image.alt != NULL &&
            strcmp(document->blocks[index].value.image.alt, alt) == 0) {
            return &document->blocks[index].value.image;
        }
    }
    return NULL;
}

static bool text_has_span(const MereaderTuiTextBlock *text, MereaderTuiTextStyle style, const char *link) {
    if (text == NULL) {
        return false;
    }
    for (size_t index = 0U; index < text->span_count; index++) {
        const MereaderTuiTextSpan *span = &text->spans[index];
        bool style_matches = (span->style & style) == style;
        bool link_matches = link == NULL ? span->link == NULL : span->link != NULL && strcmp(span->link, link) == 0;
        if (style_matches && link_matches && span->start < span->end && span->end <= strlen(text->text)) {
            return true;
        }
    }
    return false;
}

static bool build_epub2(char **path) {
    static const char opf[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"book-id\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>Harness Book</dc:title><dc:creator>Test Author</dc:creator>"
        "<dc:description>Generated locally</dc:description><dc:publisher>mereader-tui Tests</dc:publisher>"
        "<dc:date>2026-07-18</dc:date><dc:language>en</dc:language><dc:format>EPUB</dc:format>"
        "<dc:identifier id=\"book-id\">urn:test:epub2</dc:identifier><dc:source>fixture</dc:source>"
        "</metadata><manifest>"
        "<item id=\"c1\" href=\"Text/chapter%23one%3F.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c2\" href=\"Text/nested/chapter2.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
        "<item id=\"pixel\" href=\"Images/pixel.png\" media-type=\"image/png\"/>"
        "<item id=\"high\" href=\"Images/high.png\" media-type=\"image/png\"/>"
        "<item id=\"broken\" href=\"Images/broken.png\" media-type=\"image/png\"/>"
        "<item id=\"vector\" href=\"Images/vector.svg\" media-type=\"image/svg+xml\"/>"
        "</manifest><spine toc=\"ncx\"><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>";
    static const char chapter1[] =
        "<!doctype html><html><head><title>ignored</title></head><body id=\"root-body\">"
        "<h1 id=\"top\"> Chapter <em>One</em> </h1>"
        "<p id=\"intro\"> Hello   <strong>bold</strong> "
        "<a id=\"next\" name=\"legacy-next\" href=\"nested/chapter2.xhtml#deep\">next</a> "
        "<a href=\"https://example.test/reference\">outside</a>. </p>"
        "<img id=\"raster\" src=\"../Images/pixel.png\" alt=\"Pixel\"/>"
        "<img id=\"broken\" src=\"../Images/broken.png\" alt=\"Broken\"/>"
        "<img id=\"data-image\" alt=\"Data\" "
        "src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=\"/>"
        "<a id=\"image-link\" href=\"nested/chapter2.xhtml#deep\">"
        "<img id=\"linked\" src=\"../Images/pixel.png\" alt=\"Linked\"/></a>"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
        "<image id=\"svg-image\" xlink:href=\"../Images/vector.svg\" aria-label=\"Vector\"/>"
        "<image id=\"svg-href-image\" href=\"../Images/pixel.png\" aria-label=\"SVG Href\"/></svg>"
        "<picture id=\"picture\"><source id=\"picture-source\" "
        "srcset=\"../Images/high.png 2x, ../Images/pixel.png 1x\"/>"
        "<img id=\"picture-fallback\" alt=\"Responsive\"/></picture>"
        "<a id=\"tail\" name=\"tail-name\"></a></body></html>";
    static const char chapter2[] =
        "<html><body><h2 id=\"deep\">Second Section</h2>"
        "<pre id=\"code\">a\tb\n界é</pre><p><a href=\"../chapter%23one%3F.xhtml#top\">back</a></p>"
        "</body></html>";
    static const char ncx[] =
        "<?xml version=\"1.0\"?>"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
        "<navMap><navPoint id=\"n1\"><navLabel><text> Chapter   One </text></navLabel>"
        "<content src=\"Text/chapter%23one%3F.xhtml#top\"/>"
        "<navPoint id=\"n2\"><navLabel><text>Second Section</text></navLabel>"
        "<content src=\"Text/nested/chapter2.xhtml#deep\"/></navPoint></navPoint></navMap></ncx>";
    static const char vector[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\">"
        "<rect width=\"1\" height=\"1\"/></svg>";
    const FixtureMember members[] = {
        {.name = "OEBPS/Text/chapter#one?.xhtml", .data = chapter1, .length = sizeof(chapter1) - 1U},
        {.name = "OEBPS/Text/nested/chapter2.xhtml", .data = chapter2, .length = sizeof(chapter2) - 1U},
        {.name = "OEBPS/toc.ncx", .data = ncx, .length = sizeof(ncx) - 1U},
        {.name = "OEBPS/Images/pixel.png", .data = pixel_png, .length = sizeof(pixel_png)},
        {.name = "OEBPS/Images/high.png", .data = pixel_png, .length = sizeof(pixel_png)},
        {.name = "OEBPS/Images/broken.png", .data = "not a PNG", .length = 9U},
        {.name = "OEBPS/Images/vector.svg", .data = vector, .length = sizeof(vector) - 1U},
    };
    return create_epub("document/epub2.epub", opf, members, MEREADER_TUI_ARRAY_LEN(members), path);
}

static MereaderTuiTestResult test_epub2_normalization_toc_resources_and_cleanup(void) {
    char *path = NULL;
    TEST_ASSERT(build_epub2(&path));
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_EPUB);
    TEST_ASSERT_STR(document.metadata.title, "Harness Book");
    TEST_ASSERT_STR(document.metadata.creator, "Test Author");
    TEST_ASSERT_STR(document.metadata.description, "Generated locally");
    TEST_ASSERT_STR(document.metadata.publisher, "mereader-tui Tests");
    TEST_ASSERT_STR(document.metadata.date, "2026-07-18");
    TEST_ASSERT_STR(document.metadata.language, "en");
    TEST_ASSERT_STR(document.metadata.format, "EPUB");
    TEST_ASSERT_STR(document.metadata.identifier, "urn:test:epub2");
    TEST_ASSERT_STR(document.metadata.source, "fixture");
    TEST_ASSERT_SIZE(document.section_count, 2U);
    TEST_ASSERT_STR(document.sections[0].id, "OEBPS/Text/chapter%23one%3F.xhtml");
    TEST_ASSERT_STR(document.sections[1].id, "OEBPS/Text/nested/chapter2.xhtml");
    TEST_ASSERT(document.sections[0].linear && document.sections[1].linear);
    TEST_ASSERT(document.sections[1].search_offset > document.sections[0].search_offset);

    const MereaderTuiTextBlock *heading = find_text(&document, "Chapter One");
    const MereaderTuiTextBlock *paragraph = find_text(&document, "Hello bold next outside.");
    TEST_ASSERT(heading != NULL && paragraph != NULL);
    TEST_ASSERT_INT((int)heading->heading_level, 1);
    TEST_ASSERT(text_has_span(heading, MEREADER_TUI_STYLE_HEADING, NULL));
    TEST_ASSERT(text_has_span(heading, (MereaderTuiTextStyle)(MEREADER_TUI_STYLE_HEADING | MEREADER_TUI_STYLE_ITALIC), NULL));
    TEST_ASSERT(text_has_span(paragraph, MEREADER_TUI_STYLE_BOLD, NULL));
    TEST_ASSERT(text_has_span(paragraph, MEREADER_TUI_STYLE_NONE, "OEBPS/Text/nested/chapter2.xhtml#deep"));
    TEST_ASSERT(text_has_span(paragraph, MEREADER_TUI_STYLE_NONE, "https://example.test/reference"));

    const MereaderTuiImageBlock *pixel = find_image(&document, "Pixel");
    const MereaderTuiImageBlock *linked = find_image(&document, "Linked");
    const MereaderTuiImageBlock *vector = find_image(&document, "Vector");
    const MereaderTuiImageBlock *svg_href = find_image(&document, "SVG Href");
    const MereaderTuiImageBlock *responsive = find_image(&document, "Responsive");
    const MereaderTuiImageBlock *broken = find_image(&document, "Broken");
    const MereaderTuiImageBlock *data = find_image(&document, "Data");
    TEST_ASSERT(pixel != NULL && linked != NULL && vector != NULL && svg_href != NULL && responsive != NULL &&
                broken != NULL && data != NULL);
    TEST_ASSERT_STR(pixel->uri, "OEBPS/Images/pixel.png");
    TEST_ASSERT_INT(pixel->intrinsic_width, 1);
    TEST_ASSERT_INT(pixel->intrinsic_height, 1);
    TEST_ASSERT(!pixel->broken);
    TEST_ASSERT_STR(linked->link, "OEBPS/Text/nested/chapter2.xhtml#deep");
    TEST_ASSERT_STR(vector->uri, "OEBPS/Images/vector.svg");
    TEST_ASSERT_STR(svg_href->uri, "OEBPS/Images/pixel.png");
    TEST_ASSERT_STR(responsive->uri, "OEBPS/Images/high.png");
    TEST_ASSERT(!vector->broken && vector->intrinsic_width == 1 && vector->intrinsic_height == 1);
    TEST_ASSERT(broken->broken && broken->intrinsic_width == 0 && broken->intrinsic_height == 0);
    TEST_ASSERT(!data->broken && data->intrinsic_width == 1 && data->intrinsic_height == 1);

    static const char *const anchors[] = {
        "root-body", "top", "intro", "next", "legacy-next", "raster", "image-link", "linked",
        "svg-image", "svg-href-image", "picture", "picture-source", "picture-fallback", "tail", "tail-name",
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(anchors); index++) {
        MereaderTuiString target = {0};
        TEST_ASSERT(mereader_tui_string_append(&target, "OEBPS/Text/chapter%23one%3F.xhtml#", &error));
        TEST_ASSERT(mereader_tui_string_append(&target, anchors[index], &error));
        TEST_ASSERT_MSG(document_has_anchor(&document, target.data), "missing anchor %s", target.data);
        mereader_tui_string_free(&target);
    }
    TEST_ASSERT(document_has_anchor(&document, "OEBPS/Text/nested/chapter2.xhtml#deep"));
    TEST_ASSERT_SIZE(document.toc_count, 2U);
    TEST_ASSERT_STR(document.toc[0].label, "Chapter One");
    TEST_ASSERT_STR(document.toc[0].target, "OEBPS/Text/chapter%23one%3F.xhtml#top");
    TEST_ASSERT_INT((int)document.toc[0].depth, 0);
    TEST_ASSERT_SIZE(document.toc[0].section_index, 0U);
    TEST_ASSERT_STR(document.toc[1].target, "OEBPS/Text/nested/chapter2.xhtml#deep");
    TEST_ASSERT_INT((int)document.toc[1].depth, 1);
    TEST_ASSERT_SIZE(document.toc[1].section_index, 1U);
    TEST_ASSERT(mereader_tui_document_current_toc(&document, document.sections[1].id) == &document.toc[1]);

    MereaderTuiResource resource = {0};
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, "OEBPS/Images/pixel.png", &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    mereader_tui_resource_free(&resource);
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, "OEBPS/Images/vector.svg", &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_STR(resource.mime_type, "image/svg+xml");
    TEST_ASSERT(buffer_contains(resource.data, resource.length, "<svg", 4U));
    mereader_tui_resource_free(&resource);
    mereader_tui_document_close(&document);
    TEST_ASSERT(document.path == NULL && document.backend == NULL && document.blocks == NULL);

    TEST_ASSERT(mereader_tui_test_write_text("document/owned-cleanup/marker", "owned"));
    char *cleanup = mereader_tui_test_path("document/owned-cleanup");
    TEST_ASSERT(cleanup != NULL);
    TEST_ASSERT_MSG(mereader_tui_epub_open(&document, path, cleanup, &error), "%s", error.message);
    TEST_ASSERT(mereader_tui_directory_exists(cleanup));
    mereader_tui_document_close(&document);
    TEST_ASSERT(!mereader_tui_directory_exists(cleanup));
    free(cleanup);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_epub_image_probe_uses_held_archive(void) {
    char *path = NULL;
    TEST_ASSERT(build_epub2(&path));
    MereaderTuiError error = {0};
    MereaderTuiString held_string = {0};
    TEST_ASSERT(mereader_tui_string_append(&held_string, path, &error));
    TEST_ASSERT(mereader_tui_string_append(&held_string, ".held", &error));
    char *held_path = mereader_tui_string_take(&held_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);

    MereaderTuiDocument document = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    const MereaderTuiImageBlock *pixel = find_image(&document, "Pixel");
    TEST_ASSERT(pixel != NULL && pixel->intrinsic_width == 0 && pixel->intrinsic_height == 0);
    TEST_ASSERT(rename(path, held_path) == 0);
    TEST_ASSERT(unlink(held_path) == 0);
    TEST_ASSERT(mereader_tui_write_file(path, "replacement", 11U, &error));

    mereader_tui_document_probe_images(&document, true);
    pixel = find_image(&document, "Pixel");
    TEST_ASSERT(pixel != NULL && !pixel->broken);
    TEST_ASSERT_INT(pixel->intrinsic_width, 1);
    TEST_ASSERT_INT(pixel->intrinsic_height, 1);
    MereaderTuiResource resource = {0};
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, pixel->uri, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    mereader_tui_resource_free(&resource);
    mereader_tui_document_close(&document);
    free(held_path);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static bool build_epub3(char **path) {
    static const char opf[] =
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>EPUB Three</dc:title>"
        "<dc:identifier>urn:test:epub3</dc:identifier><dc:language>en</dc:language></metadata>"
        "<manifest><item id=\"one\" href=\"one.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"page\" href=\"Images/page.svg\" media-type=\"image/svg+xml\"/>"
        "<item id=\"two\" href=\"two.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
        "</manifest><spine><itemref idref=\"one\"/><itemref idref=\"page\" linear=\"no\"/>"
        "<itemref idref=\"two\"/></spine></package>";
    static const char one[] = "<html><body><h1 id=\"start\">EPUB Three</h1><p>First section.</p></body></html>";
    static const char two[] = "<html><body><h2 id=\"second\">Second</h2><p>Last section.</p></body></html>";
    static const char page[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"2\"><rect width=\"2\" "
        "height=\"2\"/></svg>";
    static const char nav[] =
        "<?xml version=\"1.0\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
        "<body><nav epub:type=\"toc\"><ol><li><a href=\"one.xhtml#start\">One</a><ol>"
        "<li><a href=\"Images/page.svg\">Picture</a></li></ol></li>"
        "<li><a href=\"two.xhtml#second\">Two</a></li></ol></nav></body></html>";
    const FixtureMember members[] = {
        {.name = "OEBPS/one.xhtml", .data = one, .length = sizeof(one) - 1U},
        {.name = "OEBPS/two.xhtml", .data = two, .length = sizeof(two) - 1U},
        {.name = "OEBPS/Images/page.svg", .data = page, .length = sizeof(page) - 1U},
        {.name = "OEBPS/nav.xhtml", .data = nav, .length = sizeof(nav) - 1U},
    };
    return create_epub("document/epub3.epub", opf, members, MEREADER_TUI_ARRAY_LEN(members), path);
}

static MereaderTuiTestResult test_epub3_nav_and_svg_spine(void) {
    char *path = NULL;
    TEST_ASSERT(build_epub3(&path));
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(document.section_count, 3U);
    TEST_ASSERT_STR(document.sections[1].id, "OEBPS/Images/page.svg");
    TEST_ASSERT(!document.sections[1].linear);
    TEST_ASSERT_SIZE(document.sections[1].block_count, 1U);
    const MereaderTuiBlock *svg_block = &document.blocks[document.sections[1].first_block];
    TEST_ASSERT_INT(svg_block->kind, MEREADER_TUI_BLOCK_IMAGE);
    TEST_ASSERT_STR(svg_block->value.image.uri, "OEBPS/Images/page.svg");
    TEST_ASSERT_INT(svg_block->value.image.intrinsic_width, 2);
    TEST_ASSERT_INT(svg_block->value.image.intrinsic_height, 2);
    TEST_ASSERT(!svg_block->value.image.broken);
    TEST_ASSERT_SIZE(document.toc_count, 3U);
    TEST_ASSERT_STR(document.toc[0].label, "One");
    TEST_ASSERT_INT((int)document.toc[1].depth, 1);
    TEST_ASSERT_STR(document.toc[1].target, "OEBPS/Images/page.svg");
    TEST_ASSERT_STR(document.toc[2].target, "OEBPS/two.xhtml#second");
    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static bool build_minimal_epub(const char *relative, const char *extra_manifest, const char *extra_spine,
                               const FixtureMember *extra_members, size_t extra_count, char **path) {
    static const char prefix[] =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Minimal</dc:title>"
        "<dc:identifier>urn:test:minimal</dc:identifier><dc:language>en</dc:language></metadata><manifest>"
        "<item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>";
    static const char middle[] = "</manifest><spine><itemref idref=\"chapter\"/>";
    static const char suffix[] = "</spine></package>";
    MereaderTuiString opf = {0};
    MereaderTuiError error = {0};
    bool built = mereader_tui_string_append(&opf, prefix, &error) && mereader_tui_string_append(&opf, extra_manifest, &error) &&
                 mereader_tui_string_append(&opf, middle, &error) && mereader_tui_string_append(&opf, extra_spine, &error) &&
                 mereader_tui_string_append(&opf, suffix, &error);
    static const char chapter[] = "<html><body><p id=\"start\">Minimal body</p></body></html>";
    FixtureMember *members = calloc(extra_count + 1U, sizeof(*members));
    if (!built || members == NULL) {
        mereader_tui_string_free(&opf);
        free(members);
        return false;
    }
    members[0] = (FixtureMember){
        .name = "OEBPS/chapter.xhtml",
        .data = chapter,
        .length = sizeof(chapter) - 1U,
    };
    for (size_t index = 0U; index < extra_count; index++) {
        members[index + 1U] = extra_members[index];
    }
    bool created = create_epub(relative, opf.data, members, extra_count + 1U, path);
    free(members);
    mereader_tui_string_free(&opf);
    return created;
}

static MereaderTuiTestResult test_missing_toc_fallback(void) {
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/no-toc.epub", "", "", NULL, 0U, &path));
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.toc_count, 0U);
    TEST_ASSERT_SIZE(document.section_count, 1U);
    TEST_ASSERT(find_text(&document, "Minimal body") != NULL);
    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_document_instance_identity_changes_on_reopen(void) {
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/instance-id.epub", "", "", NULL, 0U, &path));
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    const uint64_t first_instance_id = document.instance_id;
    TEST_ASSERT(first_instance_id != 0U);
    mereader_tui_document_close(&document);
    TEST_ASSERT(document.instance_id == 0U);

    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT(document.instance_id != 0U && document.instance_id != first_instance_id);
    mereader_tui_document_close(&document);

    document.instance_id = first_instance_id;
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    document.instance_id = 0U;
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_data_resources(void) {
    MereaderTuiDocument document = {0};
    MereaderTuiResource resource = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(mereader_tui_document_load_resource(&document, "data:image/png;base64,iVBORw0KGgo=", &resource, &error));
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    TEST_ASSERT_SIZE(resource.length, 8U);
    TEST_ASSERT(memcmp(resource.data, "\x89PNG\r\n\x1a\n", 8U) == 0);
    mereader_tui_resource_free(&resource);
    TEST_ASSERT(mereader_tui_document_load_resource(&document, "data:text/plain,a%20b%00c", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, 5U);
    TEST_ASSERT(memcmp(resource.data, "a b\0c", 5U) == 0);
    mereader_tui_resource_free(&resource);
    TEST_ASSERT(!mereader_tui_document_load_resource(&document, "data:text/plain;base64,%%%", &resource, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_png_structure_resource_and_probe(void) {
    static const char relative[] = "document/standalone image.png";
    TEST_ASSERT(mereader_tui_test_write(relative, pixel_png, sizeof(pixel_png)));
    char *path = mereader_tui_test_path(relative);
    TEST_ASSERT(path != NULL);

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_IMAGE);
    TEST_ASSERT_STR(mereader_tui_document_format_name(document.format), "image");
    TEST_ASSERT_STR(document.metadata.title, "standalone image.png");
    TEST_ASSERT_STR(document.metadata.format, "Image");
    TEST_ASSERT_SIZE(document.section_count, 1U);
    TEST_ASSERT_SIZE(document.block_count, 1U);
    TEST_ASSERT_STR(document.sections[0].id, path);
    TEST_ASSERT_SIZE(document.sections[0].first_block, 0U);
    TEST_ASSERT_SIZE(document.sections[0].block_count, 1U);
    TEST_ASSERT(document.sections[0].linear);
    TEST_ASSERT_INT(document.blocks[0].kind, MEREADER_TUI_BLOCK_IMAGE);
    TEST_ASSERT_STR(document.blocks[0].section_id, path);
    TEST_ASSERT_STR(document.blocks[0].value.image.uri, path);
    TEST_ASSERT_STR(document.blocks[0].value.image.alt, "standalone image.png");
    TEST_ASSERT_INT(document.blocks[0].value.image.page_index, -1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 0);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 0);
    TEST_ASSERT(!document.blocks[0].value.image.broken);

    MereaderTuiString wrong_uri = {0};
    TEST_ASSERT(mereader_tui_string_append(&wrong_uri, path, &error));
    TEST_ASSERT(mereader_tui_string_append(&wrong_uri, "#other", &error));
    MereaderTuiResource resource = {0};
    TEST_ASSERT(!mereader_tui_document_load_resource(&document, wrong_uri.data, &resource, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_NOT_FOUND);
    mereader_tui_string_free(&wrong_uri);

    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, path, &resource, &error), "%s", error.message);
    TEST_ASSERT(resource.data != NULL && resource.data != pixel_png);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(resource.mime_type == NULL);
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    resource.data[0] = 0U;
    mereader_tui_resource_free(&resource);
    TEST_ASSERT(resource.data == NULL && resource.length == 0U && resource.mime_type == NULL);
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, path, &resource, &error), "%s", error.message);
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    mereader_tui_resource_free(&resource);

    mereader_tui_document_probe_images(&document, false);
    TEST_ASSERT(document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 0);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 0);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 1);

    mereader_tui_document_close(&document);
    TEST_ASSERT(document.path == NULL && document.blocks == NULL && document.sections == NULL);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_snapshot_survives_replacement_and_mutation(void) {
    static const char relative[] = "document/stable-snapshot.png";
    TEST_ASSERT(mereader_tui_test_write(relative, pixel_png, sizeof(pixel_png)));
    char *path = mereader_tui_test_path(relative);
    TEST_ASSERT(path != NULL);
    MereaderTuiString held_path_string = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(mereader_tui_string_append(&held_path_string, path, &error));
    TEST_ASSERT(mereader_tui_string_append(&held_path_string, ".held", &error));
    char *held_path = mereader_tui_string_take(&held_path_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);

    struct stat original_status;
    TEST_ASSERT(stat(path, &original_status) == 0);
    MereaderTuiDocument document = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_STR(document.path, path);
    TEST_ASSERT(rename(path, held_path) == 0);

    unsigned char replacement[sizeof(pixel_png)];
    memset(replacement, 0x5a, sizeof(replacement));
    TEST_ASSERT(mereader_tui_write_file(path, replacement, sizeof(replacement), &error));
    struct stat held_status;
    struct stat replacement_status;
    TEST_ASSERT(stat(held_path, &held_status) == 0);
    TEST_ASSERT(stat(path, &replacement_status) == 0);
    TEST_ASSERT(held_status.st_dev == original_status.st_dev && held_status.st_ino == original_status.st_ino);
    TEST_ASSERT(replacement_status.st_dev != original_status.st_dev ||
                replacement_status.st_ino != original_status.st_ino);
    TEST_ASSERT(replacement_status.st_size == original_status.st_size);

    MereaderTuiResource resource = {0};
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, document.path, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    mereader_tui_resource_free(&resource);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 1);

    unsigned char mutation[sizeof(pixel_png)];
    memset(mutation, 0xa5, sizeof(mutation));
    TEST_ASSERT(mereader_tui_write_file(held_path, mutation, sizeof(mutation), &error));
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, document.path, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    mereader_tui_resource_free(&resource);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 1);

    TEST_ASSERT(unlink(held_path) == 0);
    char *export_directory = mereader_tui_make_temp_directory("mereader-tui-image-test-", &error);
    TEST_ASSERT_MSG(export_directory != NULL, "%s", error.message);
    char *export_path = mereader_tui_path_join(export_directory, "stable-snapshot.png", &error);
    TEST_ASSERT(export_path != NULL);
    TEST_ASSERT_MSG(mereader_tui_image_export_original(&document, export_path, &error), "%s", error.message);
    struct stat export_status;
    TEST_ASSERT(stat(export_path, &export_status) == 0 && S_ISREG(export_status.st_mode));
    TEST_ASSERT((export_status.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    TEST_ASSERT_SIZE((size_t)export_status.st_size, sizeof(pixel_png));
    TEST_ASSERT(!mereader_tui_image_export_original(&document, export_path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_IO);

    mereader_tui_document_close(&document);
    int reused_descriptor = open(path, O_RDONLY | O_CLOEXEC);
    TEST_ASSERT(reused_descriptor >= 0);
    TEST_ASSERT(close(reused_descriptor) == 0);

    MereaderTuiBuffer exported = {0};
    MereaderTuiBuffer replacement_bytes = {0};
    TEST_ASSERT_MSG(mereader_tui_read_file(export_path, &exported, &error), "%s", error.message);
    TEST_ASSERT_MSG(mereader_tui_read_file(path, &replacement_bytes, &error), "%s", error.message);
    TEST_ASSERT_SIZE(exported.length, sizeof(pixel_png));
    TEST_ASSERT_SIZE(replacement_bytes.length, sizeof(replacement));
    TEST_ASSERT(memcmp(exported.data, pixel_png, sizeof(pixel_png)) == 0);
    TEST_ASSERT(memcmp(replacement_bytes.data, replacement, sizeof(replacement)) == 0);
    mereader_tui_buffer_free(&replacement_bytes);
    mereader_tui_buffer_free(&exported);
    TEST_ASSERT(mereader_tui_remove_tree(export_directory, &error));
    free(export_path);
    free(export_directory);
    free(held_path);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_backend_rejects_detected_identity_mismatch(void) {
    static const char relative[] = "document/identity-mismatch.png";
    TEST_ASSERT(mereader_tui_test_write(relative, pixel_png, sizeof(pixel_png)));
    char *path = mereader_tui_test_path(relative);
    TEST_ASSERT(path != NULL);
    MereaderTuiError error = {0};
    MereaderTuiString held_string = {0};
    TEST_ASSERT(mereader_tui_string_append(&held_string, path, &error));
    TEST_ASSERT(mereader_tui_string_append(&held_string, ".held", &error));
    char *held_path = mereader_tui_string_take(&held_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);

    struct stat detected_identity;
    TEST_ASSERT(stat(path, &detected_identity) == 0);
    TEST_ASSERT(rename(path, held_path) == 0);
    TEST_ASSERT(mereader_tui_write_file(path, pixel_png, sizeof(pixel_png), &error));

    MereaderTuiDocument document = {.path = mereader_tui_strdup(path, &error)};
    TEST_ASSERT(document.path != NULL);
    TEST_ASSERT(!mereader_tui_image_open(&document, document.path, &detected_identity, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "format detection") != NULL);
    TEST_ASSERT(document.backend == NULL && document.ops == NULL);
    mereader_tui_document_close(&document);
    free(held_path);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_large_export_streams_held_original(void) {
    static const char relative[] = "document/large-export.bmp";
    static const unsigned char start_marker[] = {'B', 'M', 'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
    static const unsigned char end_marker[] = {'e', 'n', 'd', '-', 'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
    TEST_ASSERT(mereader_tui_test_write(relative, NULL, 0U));
    char *path = mereader_tui_test_path(relative);
    TEST_ASSERT(path != NULL);
    const off_t size = (off_t)MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES + 1;
    int source = open(path, O_WRONLY | O_CLOEXEC);
    TEST_ASSERT(source >= 0);
    TEST_ASSERT(ftruncate(source, size) == 0);
    TEST_ASSERT(pwrite(source, start_marker, sizeof(start_marker), 0) == (ssize_t)sizeof(start_marker));
    TEST_ASSERT(pwrite(source, end_marker, sizeof(end_marker), size - (off_t)sizeof(end_marker)) ==
                (ssize_t)sizeof(end_marker));
    TEST_ASSERT(close(source) == 0);

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    MereaderTuiString held_string = {0};
    TEST_ASSERT(mereader_tui_string_append(&held_string, path, &error));
    TEST_ASSERT(mereader_tui_string_append(&held_string, ".held", &error));
    char *held_path = mereader_tui_string_take(&held_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);
    TEST_ASSERT(rename(path, held_path) == 0);
    TEST_ASSERT(unlink(held_path) == 0);
    TEST_ASSERT(mereader_tui_write_file(path, pixel_png, sizeof(pixel_png), &error));

    char *directory = mereader_tui_make_temp_directory("mereader-tui-image-large-test-", &error);
    TEST_ASSERT_MSG(directory != NULL, "%s", error.message);
    char *destination = mereader_tui_path_join(directory, "large-export.bmp", &error);
    TEST_ASSERT(destination != NULL);
    TEST_ASSERT_MSG(mereader_tui_image_export_original(&document, destination, &error), "%s", error.message);
    mereader_tui_document_close(&document);

    struct stat exported_status;
    TEST_ASSERT(stat(destination, &exported_status) == 0 && S_ISREG(exported_status.st_mode));
    TEST_ASSERT(exported_status.st_size == size);
    int exported = open(destination, O_RDONLY | O_CLOEXEC);
    TEST_ASSERT(exported >= 0);
    unsigned char actual_start[sizeof(start_marker)] = {0};
    unsigned char actual_end[sizeof(end_marker)] = {0};
    TEST_ASSERT(pread(exported, actual_start, sizeof(actual_start), 0) == (ssize_t)sizeof(actual_start));
    TEST_ASSERT(pread(exported, actual_end, sizeof(actual_end), size - (off_t)sizeof(actual_end)) ==
                (ssize_t)sizeof(actual_end));
    TEST_ASSERT(close(exported) == 0);
    TEST_ASSERT(memcmp(actual_start, start_marker, sizeof(start_marker)) == 0);
    TEST_ASSERT(memcmp(actual_end, end_marker, sizeof(end_marker)) == 0);
    TEST_ASSERT(mereader_tui_remove_tree(directory, &error));
    free(destination);
    free(directory);
    free(held_path);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_export_cap_and_partial_cleanup(void) {
    static const char over_cap_relative[] = "document/export-over-cap.bmp";
    TEST_ASSERT(mereader_tui_test_write(over_cap_relative, NULL, 0U));
    char *over_cap_path = mereader_tui_test_path(over_cap_relative);
    TEST_ASSERT(over_cap_path != NULL);
    int source = open(over_cap_path, O_WRONLY | O_CLOEXEC);
    TEST_ASSERT(source >= 0);
    TEST_ASSERT(ftruncate(source, (off_t)MEREADER_TUI_DOCUMENT_MAX_RETAINED_BYTES + 1) == 0);
    TEST_ASSERT(close(source) == 0);

    MereaderTuiError error = {0};
    MereaderTuiDocument over_cap = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&over_cap, over_cap_path, &error), "%s", error.message);
    char *directory = mereader_tui_make_temp_directory("mereader-tui-image-cap-test-", &error);
    TEST_ASSERT_MSG(directory != NULL, "%s", error.message);
    char *destination = mereader_tui_path_join(directory, "over-cap.bmp", &error);
    TEST_ASSERT(destination != NULL);
    TEST_ASSERT(!mereader_tui_image_export_original(&over_cap, destination, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "256 MiB") != NULL);
    errno = 0;
    TEST_ASSERT(lstat(destination, &(struct stat){0}) != 0 && errno == ENOENT);
    mereader_tui_document_close(&over_cap);
    TEST_ASSERT(mereader_tui_remove_tree(directory, &error));
    free(destination);
    free(directory);
    free(over_cap_path);

    static const char partial_relative[] = "document/export-partial.bmp";
    TEST_ASSERT(mereader_tui_test_write(partial_relative, NULL, 0U));
    char *partial_path = mereader_tui_test_path(partial_relative);
    TEST_ASSERT(partial_path != NULL);
    source = open(partial_path, O_WRONLY | O_CLOEXEC);
    TEST_ASSERT(source >= 0);
    TEST_ASSERT(ftruncate(source, 1024 * 1024) == 0);
    TEST_ASSERT(close(source) == 0);
    MereaderTuiDocument partial = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&partial, partial_path, &error), "%s", error.message);
    directory = mereader_tui_make_temp_directory("mereader-tui-image-partial-test-", &error);
    TEST_ASSERT_MSG(directory != NULL, "%s", error.message);
    destination = mereader_tui_path_join(directory, "partial.bmp", &error);
    TEST_ASSERT(destination != NULL);

    const pid_t child = fork();
    TEST_ASSERT(child >= 0);
    if (child == 0) {
        const struct rlimit limit = {.rlim_cur = 4096U, .rlim_max = 4096U};
        MereaderTuiError child_error = {0};
        const bool ready = signal(SIGXFSZ, SIG_IGN) != SIG_ERR &&
                           setrlimit(RLIMIT_FSIZE, &limit) == 0;
        const bool exported = ready && mereader_tui_image_export_original(&partial, destination, &child_error);
        errno = 0;
        const bool removed = lstat(destination, &(struct stat){0}) != 0 && errno == ENOENT;
        _exit(!exported && child_error.code == MEREADER_TUI_ERROR_IO && removed ? 0 : 1);
    }
    int status = 0;
    TEST_ASSERT(waitpid(child, &status, 0) == child);
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    errno = 0;
    TEST_ASSERT(lstat(destination, &(struct stat){0}) != 0 && errno == ENOENT);
    mereader_tui_document_close(&partial);
    TEST_ASSERT(mereader_tui_remove_tree(directory, &error));
    free(destination);
    free(directory);
    free(partial_path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_corrupt_extensions_use_placeholder(void) {
    static const char corrupt[] = "not an image";
    static const char *const extensions[] = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "svg"};
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(extensions); ++index) {
        char relative[64] = {0};
        const int length = snprintf(relative, sizeof(relative), "document/corrupt.%s", extensions[index]);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(relative));
        TEST_ASSERT(mereader_tui_test_write(relative, corrupt, sizeof(corrupt) - 1U));
        char *path = mereader_tui_test_path(relative);
        TEST_ASSERT(path != NULL);

        MereaderTuiDocument document = {0};
        MereaderTuiError error = {0};
        TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
        TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_IMAGE);
        TEST_ASSERT(!document.blocks[0].value.image.broken);
        mereader_tui_document_probe_images(&document, true);
        TEST_ASSERT(document.blocks[0].value.image.broken);
        TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 0);
        TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 0);

        MereaderTuiResource resource = {0};
        TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, path, &resource, &error), "%s", error.message);
        TEST_ASSERT_SIZE(resource.length, sizeof(corrupt) - 1U);
        TEST_ASSERT(resource.mime_type == NULL);
        TEST_ASSERT(memcmp(resource.data, corrupt, sizeof(corrupt) - 1U) == 0);
        mereader_tui_resource_free(&resource);
        mereader_tui_document_close(&document);
        free(path);
    }
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_png_magic_without_image_extension(void) {
    static const char relative[] = "document/png-magic.unknown";
    TEST_ASSERT(mereader_tui_test_write(relative, pixel_png, sizeof(pixel_png)));
    char *path = mereader_tui_test_path(relative);
    TEST_ASSERT(path != NULL);

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_IMAGE);
    TEST_ASSERT_STR(document.blocks[0].value.image.uri, path);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 1);
    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_valid_jpeg_decode_and_probe(void) {
    MereaderTuiDocument decoder = {0};
    MereaderTuiResource jpeg = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&decoder, pixel_jpeg_data_uri, &jpeg, &error), "%s",
                    error.message);
    int width = 0;
    int height = 0;
    TEST_ASSERT_MSG(mereader_tui_graphics_probe_resource(&jpeg, &width, &height, &error), "%s", error.message);
    TEST_ASSERT_INT(width, 2);
    TEST_ASSERT_INT(height, 3);
    const StandaloneFormatFixture fixture = {
        .name = "jpeg",
        .extension = "jpg",
        .loader = "jpeg",
        .data = jpeg.data,
        .length = jpeg.length,
    };
    const MereaderTuiTestResult result = test_valid_standalone_format(&fixture);
    mereader_tui_resource_free(&jpeg);
    return result;
}

static MereaderTuiTestResult test_standalone_valid_static_gif_decode_and_probe(void) {
    const StandaloneFormatFixture fixture = {
        .name = "gif",
        .extension = "gif",
        .loader = "gif",
        .data = pixel_gif,
        .length = sizeof(pixel_gif),
    };
    return test_valid_standalone_format(&fixture);
}

static MereaderTuiTestResult test_standalone_valid_webp_magic_decode_and_probe(void) {
    const StandaloneFormatFixture fixture = {
        .name = "webp",
        .extension = "webp",
        .loader = "webp",
        .data = pixel_webp,
        .length = sizeof(pixel_webp),
    };
    return test_valid_standalone_format(&fixture);
}

static MereaderTuiTestResult test_standalone_valid_bmp_magic_decode_and_probe(void) {
    const StandaloneFormatFixture fixture = {
        .name = "bmp",
        .extension = "bmp",
        .loader = "bmp",
        .data = pixel_bmp,
        .length = sizeof(pixel_bmp),
    };
    return test_valid_standalone_format(&fixture);
}

static MereaderTuiTestResult test_standalone_valid_svg_magic_decode_and_probe(void) {
    const StandaloneFormatFixture fixture = {
        .name = "svg",
        .extension = "svg",
        .loader = "svg",
        .data = pixel_svg,
        .length = sizeof(pixel_svg) - 1U,
    };
    return test_valid_standalone_format(&fixture);
}

static MereaderTuiTestResult test_standalone_avif_is_unsupported(void) {
    static const unsigned char avif[] = {
        0x00U, 0x00U, 0x00U, 0x18U, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f',
        0x00U, 0x00U, 0x00U, 0x00U, 'a', 'v', 'i', 'f', 'm', 'i', 'f', '1',
    };
    static const char relative[] = "document/unsupported.avif";
    TEST_ASSERT(mereader_tui_test_write(relative, avif, sizeof(avif)));
    char *path = mereader_tui_test_path(relative);
    TEST_ASSERT(path != NULL);

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    TEST_ASSERT(document.path == NULL && document.format == MEREADER_TUI_FORMAT_UNKNOWN);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_standalone_oversized_input_uses_placeholder(void) {
    static const char relative[] = "document/oversized.bmp";
    TEST_ASSERT(mereader_tui_test_write(relative, NULL, 0U));
    char *path = mereader_tui_test_path(relative);
    TEST_ASSERT(path != NULL);
    int descriptor = open(path, O_WRONLY | O_CLOEXEC);
    TEST_ASSERT(descriptor >= 0);
    const off_t oversized = (off_t)MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES + 1;
    TEST_ASSERT(ftruncate(descriptor, oversized) == 0);
    TEST_ASSERT(close(descriptor) == 0);

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_IMAGE);

    MereaderTuiString held_path_string = {0};
    TEST_ASSERT(mereader_tui_string_append(&held_path_string, path, &error));
    TEST_ASSERT(mereader_tui_string_append(&held_path_string, ".held", &error));
    char *held_path = mereader_tui_string_take(&held_path_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);
    TEST_ASSERT(rename(path, held_path) == 0);
    TEST_ASSERT(mereader_tui_write_file(path, pixel_png, sizeof(pixel_png), &error));

    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 0);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 0);

    MereaderTuiResource resource = {0};
    TEST_ASSERT(!mereader_tui_document_load_resource(&document, path, &resource, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "size limit") != NULL);
    TEST_ASSERT(resource.data == NULL && resource.length == 0U && resource.mime_type == NULL);
    mereader_tui_document_close(&document);
    free(held_path);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_fifo_and_special_files_are_rejected_without_blocking(void) {
    char *fifo = mereader_tui_test_path("document/nonblocking.fifo");
    TEST_ASSERT(fifo != NULL);
    char *directory = mereader_tui_path_dirname(fifo, NULL);
    TEST_ASSERT(directory != NULL);
    TEST_ASSERT(mereader_tui_mkdirs(directory, NULL));
    free(directory);
    (void)unlink(fifo);
    TEST_ASSERT(mkfifo(fifo, 0600) == 0);

    DocumentOpenResult result = {0};
    TEST_ASSERT_MSG(document_open_returns_without_blocking(fifo, &result),
                    "opening a FIFO blocked instead of rejecting it");
    TEST_ASSERT(!result.opened);
    TEST_ASSERT(result.code != MEREADER_TUI_ERROR_NONE);

    result = (DocumentOpenResult){0};
    TEST_ASSERT_MSG(document_open_returns_without_blocking("/dev/null", &result),
                    "opening a special file did not return");
    TEST_ASSERT(!result.opened);
    TEST_ASSERT(result.code != MEREADER_TUI_ERROR_NONE);
    free(fifo);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_image_probe_dedupe_placeholder_and_aggregate_limit(void) {
    char *path = NULL;
    TEST_ASSERT(create_numbered_probe_archive("document/probe-same.zip", "same", 1U, pixel_png,
                                              sizeof(pixel_png), &path));
    ProbeBackend backend = {.resource_size = sizeof(pixel_png)};
    MereaderTuiDocument document = {
        .path = path,
        .format = MEREADER_TUI_FORMAT_EPUB,
        .backend = &backend,
        .ops = &probe_document_ops,
    };
    MereaderTuiError error = {0};
    TEST_ASSERT(append_probe_image(&document, "same-0.png", &error));
    TEST_ASSERT(append_probe_image(&document, "same-0.png", &error));
    mereader_tui_document_probe_images(&document, false);
    TEST_ASSERT_SIZE(backend.loads, 0U);
    TEST_ASSERT(document.blocks[0].value.image.broken && document.blocks[1].value.image.broken);

    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(backend.loads, 1U);
    TEST_ASSERT(!document.blocks[0].value.image.broken && !document.blocks[1].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[1].value.image.intrinsic_height, 1);
    mereader_tui_document_close(&document);

    path = NULL;
    TEST_ASSERT(create_numbered_probe_archive("document/probe-count.zip", "image",
                                              MEREADER_TUI_GRAPHICS_MAX_PROBES + 5U, "x", 1U, &path));
    backend = (ProbeBackend){.resource_size = 1U, .corrupt = true};
    document = (MereaderTuiDocument){
        .path = path,
        .format = MEREADER_TUI_FORMAT_EPUB,
        .backend = &backend,
        .ops = &probe_document_ops,
    };
    for (size_t index = 0U; index < MEREADER_TUI_GRAPHICS_MAX_PROBES + 5U; ++index) {
        char uri[64] = {0};
        const int length = snprintf(uri, sizeof(uri), "image-%zu.png", index);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(uri));
        TEST_ASSERT(append_probe_image(&document, uri, &error));
    }
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(backend.loads, MEREADER_TUI_GRAPHICS_MAX_PROBES);
    TEST_ASSERT(document.blocks[0].value.image.broken);
    TEST_ASSERT(document.blocks[document.block_count - 1U].value.image.broken);
    mereader_tui_document_close(&document);

    path = NULL;
    TEST_ASSERT(create_numbered_probe_archive("document/probe-budget.zip", "budget", 6U, "x", 1U,
                                              &path));
    char *oversized_path = mereader_tui_strdup(path, &error);
    TEST_ASSERT(oversized_path != NULL);

    backend = (ProbeBackend){.resource_size = MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES, .corrupt = true};
    document = (MereaderTuiDocument){
        .path = path,
        .format = MEREADER_TUI_FORMAT_EPUB,
        .backend = &backend,
        .ops = &probe_document_ops,
    };
    for (size_t index = 0U; index < 5U; ++index) {
        char uri[64] = {0};
        const int length = snprintf(uri, sizeof(uri), "budget-%zu.png", index);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(uri));
        TEST_ASSERT(append_probe_image(&document, uri, &error));
    }
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(backend.loads, MEREADER_TUI_GRAPHICS_MAX_PROBE_BYTES / MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES);
    mereader_tui_document_close(&document);

    backend = (ProbeBackend){
        .resource_size = 1U,
        .override_uri = "budget-5.png",
        .override_size = MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES + 1U,
        .corrupt = true,
    };
    document = (MereaderTuiDocument){
        .path = oversized_path,
        .format = MEREADER_TUI_FORMAT_EPUB,
        .backend = &backend,
        .ops = &probe_document_ops,
    };
    TEST_ASSERT(append_probe_image(&document, "budget-5.png", &error));
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(backend.loads, 0U);
    TEST_ASSERT(document.blocks[0].value.image.broken);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_traversal_errors(void) {
    char *path = mereader_tui_test_path("document/archive-traversal.epub");
    TEST_ASSERT(path != NULL);
    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    TEST_ASSERT(archive != NULL);
    TEST_ASSERT(zip_add_bytes(archive, "mimetype", "application/epub+zip", 20U, true, NULL));
    TEST_ASSERT(zip_add_bytes(archive, "../escape", "x", 1U, false, NULL));
    TEST_ASSERT(close_archive(archive));
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "unsafe EPUB archive path") != NULL);
    free(path);

    static const char unsafe_opf[] =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>x</dc:title></metadata>"
        "<manifest><item id=\"bad\" href=\"../../escape.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine><itemref idref=\"bad\"/></spine></package>";
    path = NULL;
    TEST_ASSERT(create_epub("document/reference-traversal.epub", unsafe_opf, NULL, 0U, &path));
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "escapes the document root") != NULL);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static bool patch_member_size(const char *path, const char *member_name, uint32_t size) {
    MereaderTuiError error = {0};
    MereaderTuiBuffer archive = {0};
    if (!mereader_tui_read_file(path, &archive, &error)) {
        return false;
    }
    bool patched = false;
    size_t name_length = strlen(member_name);
    for (size_t offset = 0U; offset + 46U <= archive.length; offset++) {
        unsigned char *entry = archive.data + offset;
        if (memcmp(entry, "PK\001\002", 4U) != 0) {
            continue;
        }
        size_t filename_length = (size_t)entry[28] | ((size_t)entry[29] << 8U);
        size_t extra_length = (size_t)entry[30] | ((size_t)entry[31] << 8U);
        size_t comment_length = (size_t)entry[32] | ((size_t)entry[33] << 8U);
        if (offset + 46U + filename_length + extra_length + comment_length > archive.length) {
            break;
        }
        if (filename_length == name_length && memcmp(entry + 46U, member_name, name_length) == 0) {
            entry[24] = (unsigned char)(size & 0xffU);
            entry[25] = (unsigned char)((size >> 8U) & 0xffU);
            entry[26] = (unsigned char)((size >> 16U) & 0xffU);
            entry[27] = (unsigned char)((size >> 24U) & 0xffU);
            patched = true;
            break;
        }
        offset += 45U + filename_length + extra_length + comment_length;
    }
    bool written = patched && mereader_tui_write_file(path, archive.data, archive.length, &error);
    mereader_tui_buffer_free(&archive);
    return written;
}

static MereaderTuiTestResult test_oversized_member_error(void) {
    static const FixtureMember extra[] = {
        {.name = "OEBPS/huge.bin", .data = "x", .length = 1U},
    };
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/oversized.epub",
                                   "<item id=\"huge\" href=\"huge.bin\" media-type=\"application/octet-stream\"/>",
                                   "", extra, MEREADER_TUI_ARRAY_LEN(extra), &path));
    TEST_ASSERT(patch_member_size(path, "OEBPS/huge.bin", 64U * 1024U * 1024U + 1U));
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "size limit") != NULL);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_encryption_document_error(void) {
    static const char encryption[] =
        "<?xml version=\"1.0\"?>"
        "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
        "<EncryptionMethod Algorithm=\"urn:test:drm\"/><CipherData>"
        "<CipherReference URI=\"../OEBPS/chapter.xhtml\"/></CipherData></EncryptedData></encryption>";
    const FixtureMember extra[] = {
        {.name = "META-INF/encryption.xml", .data = encryption, .length = sizeof(encryption) - 1U},
    };
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/encrypted-metadata.epub", "", "", extra, MEREADER_TUI_ARRAY_LEN(extra),
                                   &path));
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    TEST_ASSERT(strstr(error.message, "encrypted EPUB content") != NULL);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_encrypted_zip_member_error(void) {
    char *path = NULL;
    static const char opf[] =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>x</dc:title></metadata>"
        "<manifest><item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"c\"/></spine></package>";
    TEST_ASSERT(create_epub("document/encrypted-member.epub", opf, NULL, 0U, &path));
    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE, &zip_error);
    TEST_ASSERT(archive != NULL);
    zip_int64_t index = -1;
    TEST_ASSERT(zip_add_bytes(archive, "OEBPS/secret.bin", "secret", 6U, false, &index));
    TEST_ASSERT(zip_file_set_encryption(archive, (zip_uint64_t)index, ZIP_EM_AES_256,
                                        "test-password") == 0);
    TEST_ASSERT(close_archive(archive));
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    TEST_ASSERT(strstr(error.message, "encrypted EPUB members") != NULL);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static bool install_fake_mobitool(const char *script, char **old_path, MereaderTuiError *error) {
    if (!mereader_tui_test_mkdir("mobi-bin")) {
        return false;
    }
    char *shim = mereader_tui_test_path("mobi-bin/mobitool");
    char *bin = mereader_tui_test_path("mobi-bin");
    if (shim == NULL || bin == NULL || !mereader_tui_test_write_text("mobi-bin/mobitool", script) ||
        chmod(shim, 0700) != 0) {
        free(shim);
        free(bin);
        return false;
    }
    const char *old_path_value = getenv("PATH");
    *old_path = mereader_tui_strdup(old_path_value == NULL ? "" : old_path_value, error);
    if (*old_path == NULL) {
        free(shim);
        free(bin);
        return false;
    }
    MereaderTuiString path_value = {0};
    const bool installed = mereader_tui_string_append(&path_value, bin, error) &&
                           mereader_tui_string_append_char(&path_value, ':', error) &&
                           mereader_tui_string_append(&path_value, *old_path, error) &&
                           setenv("PATH", path_value.data, 1) == 0;
    mereader_tui_string_free(&path_value);
    free(shim);
    free(bin);
    if (!installed) {
        free(*old_path);
        *old_path = NULL;
    }
    return installed;
}

static void restore_fake_mobitool(char *old_path) {
    if (old_path != NULL) {
        (void)setenv("PATH", old_path, 1);
    }
    free(old_path);
}

static char *create_fake_mobi_input(const char *extension) {
    unsigned char header[68] = {0};
    memcpy(header + 60U, "BOOKMOBI", 8U);
    char relative[64] = {0};
    const int length = snprintf(relative, sizeof(relative), "mobi-input/book%s", extension);
    if (length <= 0 || (size_t)length >= sizeof(relative) ||
        !mereader_tui_test_write(relative, header, sizeof(header))) {
        return NULL;
    }
    return mereader_tui_test_path(relative);
}

static bool create_fake_mobitool_epub(char **path) {
    static const char opf[] =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Converted Book</dc:title>"
        "<dc:creator>Generated Fixture</dc:creator></metadata>"
        "<manifest><item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"chapter\"/></spine></package>";
    static const char chapter[] =
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>Converted text.</p></body></html>";
    const FixtureMember member = {
        .name = "OEBPS/chapter.xhtml",
        .data = chapter,
        .length = sizeof(chapter) - 1U,
    };
    return create_epub("mobi-output/template.epub", opf, &member, 1U, path);
}

static MereaderTuiTestResult test_fake_mobitool_converts_all_shared_extensions(void) {
    static const char script[] =
        "#!/bin/sh\n"
        "input=$4\n"
        "name=${input##*/}\n"
        "name=${name%.*}.epub\n"
        "/bin/cp \"$MEREADER_TUI_TEST_FAKE_EPUB\" \"$3/$name\"\n";
    static const struct {
        const char *extension;
        MereaderTuiDocumentFormat format;
    } cases[] = {
        {".mobi", MEREADER_TUI_FORMAT_MOBI},
        {".prc", MEREADER_TUI_FORMAT_MOBI},
        {".azw", MEREADER_TUI_FORMAT_AZW},
        {".azw3", MEREADER_TUI_FORMAT_AZW},
    };
    MereaderTuiError error = {0};
    char *epub = NULL;
    char *old_path = NULL;
    TEST_ASSERT(create_fake_mobitool_epub(&epub));
    TEST_ASSERT(setenv("MEREADER_TUI_TEST_FAKE_EPUB", epub, 1) == 0);
    TEST_ASSERT(install_fake_mobitool(script, &old_path, &error));

    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(cases); ++index) {
        char *input = create_fake_mobi_input(cases[index].extension);
        TEST_ASSERT(input != NULL);
        const size_t before = mereader_tui_test_count_directories("tmp", "mereader-tui-mobi-");
        MereaderTuiDocument document = {0};
        TEST_ASSERT_MSG(mereader_tui_document_open(&document, input, &error), "%s", error.message);
        TEST_ASSERT_INT(document.format, cases[index].format);
        TEST_ASSERT(find_text(&document, "Converted text.") != NULL);
        mereader_tui_document_close(&document);
        TEST_ASSERT_SIZE(mereader_tui_test_count_directories("tmp", "mereader-tui-mobi-"), before);
        free(input);
    }
    restore_fake_mobitool(old_path);
    TEST_ASSERT(unsetenv("MEREADER_TUI_TEST_FAKE_EPUB") == 0);
    free(epub);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_fake_mobitool_rejects_outputs_and_drm(void) {
    static const struct {
        const char *script;
        MereaderTuiErrorCode code;
        const char *message;
    } cases[] = {
        {"#!/bin/sh\nexit 0\n", MEREADER_TUI_ERROR_EXTERNAL, "without producing"},
        {"#!/bin/sh\ninput=$4\nname=${input##*/}\nname=${name%.*}.epub\nprintf bad >\"$3/$name\"\n",
         MEREADER_TUI_ERROR_CORRUPT, "EPUB"},
        {"#!/bin/sh\ninput=$4\nname=${input##*/}\nname=${name%.*}.epub\nmkdir \"$3/$name\"\n",
         MEREADER_TUI_ERROR_EXTERNAL, "non-regular"},
        {"#!/bin/sh\ninput=$4\nname=${input##*/}\nname=${name%.*}.epub\n/usr/bin/truncate -s 1073741825 \"$3/$name\"\n",
         MEREADER_TUI_ERROR_EXTERNAL, "size limit"},
        {"#!/bin/sh\nprintf 'Document is encrypted\\n' >&2\nexit 1\n",
         MEREADER_TUI_ERROR_UNSUPPORTED, "DRM protected"},
        {"#!/bin/sh\nprintf 'AZW4 print replica book\\n' >&2\nexit 1\n",
         MEREADER_TUI_ERROR_UNSUPPORTED, "Print Replica"},
    };
    MereaderTuiError error = {0};
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(cases); ++index) {
        char *old_path = NULL;
        TEST_ASSERT(install_fake_mobitool(cases[index].script, &old_path, &error));
        char *input = create_fake_mobi_input(".mobi");
        TEST_ASSERT(input != NULL);
        const size_t before = mereader_tui_test_count_directories("tmp", "mereader-tui-mobi-");
        MereaderTuiDocument document = {0};
        TEST_ASSERT(!mereader_tui_document_open(&document, input, &error));
        TEST_ASSERT_ERROR(error, cases[index].code);
        TEST_ASSERT(strstr(error.message, cases[index].message) != NULL);
        TEST_ASSERT_SIZE(mereader_tui_test_count_directories("tmp", "mereader-tui-mobi-"), before);
        restore_fake_mobitool(old_path);
        mereader_tui_error_clear(&error);
        free(input);
    }
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_fake_mobitool_bounds_and_sanitizes_diagnostics(void) {
    static const char oversized_script[] =
        "#!/bin/sh\n"
        "/usr/bin/head -c 65537 /dev/zero\n"
        "exit 1\n";
    static const char hostile_script[] =
        "#!/bin/sh\n"
        "/usr/bin/printf 'bad\\n\\t\\033[31m\\x9B\\xC2\\x9Btail\\177' >&2\n"
        "exit 7\n";
    MereaderTuiError error = {0};
    char *input = create_fake_mobi_input(".mobi");
    char *old_path = NULL;
    TEST_ASSERT(input != NULL);
    TEST_ASSERT(install_fake_mobitool(oversized_script, &old_path, &error));
    MereaderTuiDocument document = {0};
    size_t before = mereader_tui_test_count_directories("tmp", "mereader-tui-mobi-");
    TEST_ASSERT(!mereader_tui_document_open(&document, input, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_EXTERNAL);
    TEST_ASSERT(strstr(error.message, "diagnostic output limit") != NULL);
    TEST_ASSERT_SIZE(mereader_tui_test_count_directories("tmp", "mereader-tui-mobi-"), before);
    restore_fake_mobitool(old_path);
    mereader_tui_error_clear(&error);

    old_path = NULL;
    TEST_ASSERT(install_fake_mobitool(hostile_script, &old_path, &error));
    TEST_ASSERT(!mereader_tui_document_open(&document, input, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_EXTERNAL);
    TEST_ASSERT(strchr(error.message, '\n') == NULL && strchr(error.message, '\r') == NULL &&
                strchr(error.message, '\t') == NULL && strchr(error.message, '\033') == NULL &&
                strchr(error.message, '\177') == NULL &&
                memchr(error.message, 0x9b, strlen(error.message)) == NULL &&
                strstr(error.message, "\302\233") == NULL);
    restore_fake_mobitool(old_path);
    free(input);
    return MEREADER_TUI_TEST_PASS;
}

static bool wait_for_process_exit(pid_t process) {
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        if (kill(process, 0) != 0 && errno == ESRCH) {
            return true;
        }
        struct timespec pause = {.tv_nsec = 10000000L};
        (void)nanosleep(&pause, NULL);
    }
    return false;
}

static MereaderTuiTestResult test_fake_mobitool_timeout_cleans_process_group(void) {
    static const char script[] =
        "#!/bin/sh\n"
        "trap '' TERM\n"
        "/bin/sh -c 'trap \"\" TERM; while :; do /bin/sleep 1; done' &\n"
        "descendant=$!\n"
        "printf '%s %s\\n' \"$$\" \"$descendant\" >\"$MEREADER_TUI_TEST_MOBITOOL_PID_FILE\"\n"
        "wait\n";
    MereaderTuiError error = {0};
    char *input = create_fake_mobi_input(".mobi");
    char *pid_path = mereader_tui_test_path("mobi-output/processes");
    char *old_path = NULL;
    TEST_ASSERT(input != NULL && pid_path != NULL);
    TEST_ASSERT(setenv("MEREADER_TUI_TEST_MOBITOOL_TIMEOUT_MS", "200", 1) == 0);
    TEST_ASSERT(setenv("MEREADER_TUI_TEST_MOBITOOL_PID_FILE", pid_path, 1) == 0);
    TEST_ASSERT(install_fake_mobitool(script, &old_path, &error));
    const size_t before = mereader_tui_test_count_directories("tmp", "mereader-tui-mobi-");
    MereaderTuiDocument document = {0};
    TEST_ASSERT(!mereader_tui_document_open(&document, input, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_EXTERNAL);
    TEST_ASSERT(strstr(error.message, "200 ms timeout") != NULL);
    TEST_ASSERT_SIZE(mereader_tui_test_count_directories("tmp", "mereader-tui-mobi-"), before);
    MereaderTuiBuffer pids = {0};
    TEST_ASSERT(mereader_tui_read_file(pid_path, &pids, &error));
    long parent = 0L;
    long descendant = 0L;
    TEST_ASSERT(sscanf((const char *)pids.data, "%ld %ld", &parent, &descendant) == 2);
    TEST_ASSERT(parent > 0L && descendant > 0L);
    TEST_ASSERT(wait_for_process_exit((pid_t)parent));
    TEST_ASSERT(wait_for_process_exit((pid_t)descendant));
    mereader_tui_buffer_free(&pids);
    restore_fake_mobitool(old_path);
    TEST_ASSERT(unsetenv("MEREADER_TUI_TEST_MOBITOOL_TIMEOUT_MS") == 0);
    TEST_ASSERT(unsetenv("MEREADER_TUI_TEST_MOBITOOL_PID_FILE") == 0);
    free(pid_path);
    free(input);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_document_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "epub2_normalization_toc_resources_and_cleanup",
         .function = test_epub2_normalization_toc_resources_and_cleanup},
        {.name = "epub_image_probe_uses_held_archive",
         .function = test_epub_image_probe_uses_held_archive},
        {.name = "epub3_nav_and_svg_spine", .function = test_epub3_nav_and_svg_spine},
        {.name = "missing_toc_fallback", .function = test_missing_toc_fallback},
        {.name = "document_instance_identity_changes_on_reopen",
         .function = test_document_instance_identity_changes_on_reopen},
        {.name = "data_resources", .function = test_data_resources},
        {.name = "standalone_png_structure_resource_and_probe",
         .function = test_standalone_png_structure_resource_and_probe},
        {.name = "standalone_snapshot_survives_replacement_and_mutation",
         .function = test_standalone_snapshot_survives_replacement_and_mutation},
        {.name = "standalone_backend_rejects_detected_identity_mismatch",
         .function = test_standalone_backend_rejects_detected_identity_mismatch},
        {.name = "standalone_large_export_streams_held_original",
         .function = test_standalone_large_export_streams_held_original},
        {.name = "standalone_export_cap_and_partial_cleanup",
         .function = test_standalone_export_cap_and_partial_cleanup},
        {.name = "standalone_corrupt_extensions_use_placeholder",
         .function = test_standalone_corrupt_extensions_use_placeholder},
        {.name = "standalone_png_magic_without_image_extension",
         .function = test_standalone_png_magic_without_image_extension},
        {.name = "standalone_valid_jpeg_decode_and_probe",
         .function = test_standalone_valid_jpeg_decode_and_probe},
        {.name = "standalone_valid_static_gif_decode_and_probe",
         .function = test_standalone_valid_static_gif_decode_and_probe},
        {.name = "standalone_valid_webp_magic_decode_and_probe",
         .function = test_standalone_valid_webp_magic_decode_and_probe},
        {.name = "standalone_valid_bmp_magic_decode_and_probe",
         .function = test_standalone_valid_bmp_magic_decode_and_probe},
        {.name = "standalone_valid_svg_magic_decode_and_probe",
         .function = test_standalone_valid_svg_magic_decode_and_probe},
        {.name = "standalone_avif_is_unsupported", .function = test_standalone_avif_is_unsupported},
        {.name = "standalone_oversized_input_uses_placeholder",
         .function = test_standalone_oversized_input_uses_placeholder},
        {.name = "fifo_and_special_files_are_rejected_without_blocking",
         .function = test_fifo_and_special_files_are_rejected_without_blocking},
        {.name = "image_probe_dedupe_placeholder_and_aggregate_limit",
         .function = test_image_probe_dedupe_placeholder_and_aggregate_limit},
        {.name = "traversal_errors", .function = test_traversal_errors},
        {.name = "oversized_member_error", .function = test_oversized_member_error},
        {.name = "encryption_document_error", .function = test_encryption_document_error},
        {.name = "encrypted_zip_member_error", .function = test_encrypted_zip_member_error},
        {.name = "fake_mobitool_converts_all_shared_extensions",
         .function = test_fake_mobitool_converts_all_shared_extensions},
        {.name = "fake_mobitool_rejects_outputs_and_drm",
         .function = test_fake_mobitool_rejects_outputs_and_drm},
        {.name = "fake_mobitool_bounds_and_sanitizes_diagnostics",
         .function = test_fake_mobitool_bounds_and_sanitizes_diagnostics},
        {.name = "fake_mobitool_timeout_cleans_process_group",
         .function = test_fake_mobitool_timeout_cleans_process_group},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
