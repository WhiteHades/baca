#include "test_support.h"

#include "baca/document.h"
#include "baca/document_backend.h"
#include "baca/graphics.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
    bool optional_loader;
} StandaloneFormatFixture;

static bool probe_load_resource(BacaDocument *document, const char *uri, BacaResource *resource,
                                BacaError *error) {
    (void)uri;
    ProbeBackend *backend = document->backend;
    ++backend->loads;
    const size_t allocation = backend->corrupt ? 1U : sizeof(pixel_png);
    resource->data = malloc(allocation);
    resource->mime_type = baca_strdup("image/png", error);
    if (resource->data == NULL || resource->mime_type == NULL) {
        baca_resource_free(resource);
        baca_error_set(error, BACA_ERROR_MEMORY, "cannot allocate probe fixture");
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

static bool probe_resource_size(const BacaDocument *document, const char *uri, size_t *size) {
    const ProbeBackend *backend = document->backend;
    if (backend == NULL || uri == NULL || size == NULL) {
        return false;
    }
    *size = backend->override_uri != NULL && strcmp(uri, backend->override_uri) == 0 ?
                backend->override_size :
                backend->resource_size;
    return true;
}

static const BacaDocumentOps probe_document_ops = {
    .load_resource = probe_load_resource,
    .resource_size = probe_resource_size,
};

static bool patch_member_size(const char *path, const char *member_name, uint32_t size);

static bool append_probe_image(BacaDocument *document, const char *uri, BacaError *error) {
    BacaBlock block = {
        .kind = BACA_BLOCK_IMAGE,
        .value.image = {
            .uri = baca_strdup(uri, error),
            .alt = baca_strdup(uri, error),
            .page_index = -1,
        },
    };
    if (block.value.image.uri != NULL && block.value.image.alt != NULL &&
        baca_document_add_image_block(document, &block, error)) {
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

static BacaTestResult test_valid_standalone_format(const StandaloneFormatFixture *fixture) {
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
        return fixture->optional_loader
                   ? baca_test_skip("GdkPixbuf %s loader unavailable", fixture->loader)
                   : baca_test_fail_at(__FILE__, __LINE__, "GdkPixbuf %s loader unavailable", fixture->loader);
    }

    char target_relative[96] = {0};
    char alias_relative[96] = {0};
    const int target_length = snprintf(target_relative, sizeof(target_relative),
                                       "document/valid-%s.unknown", fixture->name);
    const int alias_length = snprintf(alias_relative, sizeof(alias_relative),
                                      "document/valid-%s-alias.%s", fixture->name, fixture->extension);
    TEST_ASSERT(target_length > 0 && (size_t)target_length < sizeof(target_relative));
    TEST_ASSERT(alias_length > 0 && (size_t)alias_length < sizeof(alias_relative));
    TEST_ASSERT(baca_test_write(target_relative, fixture->data, fixture->length));
    char *target = baca_test_path(target_relative);
    char *alias = baca_test_path(alias_relative);
    TEST_ASSERT(target != NULL && alias != NULL);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, target, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_IMAGE);
    baca_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 2);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 3);

    BacaResource resource = {0};
    TEST_ASSERT_MSG(baca_document_load_resource(&document, document.path, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, fixture->length);
    TEST_ASSERT(memcmp(resource.data, fixture->data, fixture->length) == 0);
    baca_resource_free(&resource);

    BacaGraphicsContext *graphics = baca_graphics_create(
        1024U * 1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
    TEST_ASSERT(graphics != NULL);
    BacaGraphicsSurface surface = {0};
    TEST_ASSERT_MSG(baca_graphics_prepare(graphics, &document, 0U, 2, 2, &surface, &error), "%s",
                    error.message);
    TEST_ASSERT_INT(surface.width, 2);
    TEST_ASSERT_INT(surface.height, 4);
    baca_graphics_surface_release(&surface);
    baca_graphics_free(graphics);
    baca_document_close(&document);

    (void)unlink(alias);
    TEST_ASSERT(symlink(target, alias) == 0);
    TEST_ASSERT_MSG(baca_document_open(&document, alias, &error), "%s", error.message);
    TEST_ASSERT_STR(document.path, target);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_IMAGE);
    baca_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 2);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 3);
    baca_document_close(&document);
    free(alias);
    free(target);
    return BACA_TEST_PASS;
}

typedef struct DocumentOpenResult {
    BacaErrorCode code;
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
        BacaDocument document = {0};
        BacaError error = {0};
        const bool opened = baca_document_open(&document, path, &error);
        if (opened) {
            baca_document_close(&document);
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
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return false;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
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
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return false;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
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

static bool document_has_anchor(const BacaDocument *document, const char *target) {
    for (size_t block_index = 0U; block_index < document->block_count; block_index++) {
        const BacaBlock *block = &document->blocks[block_index];
        if (block->kind == BACA_BLOCK_TEXT) {
            for (size_t anchor_index = 0U; anchor_index < block->value.text.anchor_count; anchor_index++) {
                if (strcmp(block->value.text.anchors[anchor_index].target, target) == 0) {
                    return true;
                }
            }
        } else if (block->kind == BACA_BLOCK_IMAGE && block->value.image.anchor != NULL &&
                   strcmp(block->value.image.anchor, target) == 0) {
            return true;
        }
    }
    return false;
}

static const BacaTextBlock *find_text(const BacaDocument *document, const char *text) {
    for (size_t index = 0U; index < document->block_count; index++) {
        if (document->blocks[index].kind == BACA_BLOCK_TEXT && document->blocks[index].value.text.text != NULL &&
            strcmp(document->blocks[index].value.text.text, text) == 0) {
            return &document->blocks[index].value.text;
        }
    }
    return NULL;
}

static const BacaImageBlock *find_image(const BacaDocument *document, const char *alt) {
    for (size_t index = 0U; index < document->block_count; index++) {
        if (document->blocks[index].kind == BACA_BLOCK_IMAGE && document->blocks[index].value.image.alt != NULL &&
            strcmp(document->blocks[index].value.image.alt, alt) == 0) {
            return &document->blocks[index].value.image;
        }
    }
    return NULL;
}

static bool text_has_span(const BacaTextBlock *text, BacaTextStyle style, const char *link) {
    if (text == NULL) {
        return false;
    }
    for (size_t index = 0U; index < text->span_count; index++) {
        const BacaTextSpan *span = &text->spans[index];
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
    return create_epub("document/epub2.epub", opf, members, BACA_ARRAY_LEN(members), path);
}

static BacaTestResult test_epub2_normalization_toc_resources_and_cleanup(void) {
    char *path = NULL;
    TEST_ASSERT(build_epub2(&path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    baca_document_probe_images(&document, true);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_EPUB);
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

    const BacaTextBlock *heading = find_text(&document, "Chapter One");
    const BacaTextBlock *paragraph = find_text(&document, "Hello bold next outside.");
    TEST_ASSERT(heading != NULL && paragraph != NULL);
    TEST_ASSERT_INT((int)heading->heading_level, 1);
    TEST_ASSERT(text_has_span(heading, BACA_STYLE_HEADING, NULL));
    TEST_ASSERT(text_has_span(heading, (BacaTextStyle)(BACA_STYLE_HEADING | BACA_STYLE_ITALIC), NULL));
    TEST_ASSERT(text_has_span(paragraph, BACA_STYLE_BOLD, NULL));
    TEST_ASSERT(text_has_span(paragraph, BACA_STYLE_NONE, "OEBPS/Text/nested/chapter2.xhtml#deep"));
    TEST_ASSERT(text_has_span(paragraph, BACA_STYLE_NONE, "https://example.test/reference"));

    const BacaImageBlock *pixel = find_image(&document, "Pixel");
    const BacaImageBlock *linked = find_image(&document, "Linked");
    const BacaImageBlock *vector = find_image(&document, "Vector");
    const BacaImageBlock *svg_href = find_image(&document, "SVG Href");
    const BacaImageBlock *responsive = find_image(&document, "Responsive");
    const BacaImageBlock *broken = find_image(&document, "Broken");
    const BacaImageBlock *data = find_image(&document, "Data");
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
    for (size_t index = 0U; index < BACA_ARRAY_LEN(anchors); index++) {
        BacaString target = {0};
        TEST_ASSERT(baca_string_append(&target, "OEBPS/Text/chapter%23one%3F.xhtml#", &error));
        TEST_ASSERT(baca_string_append(&target, anchors[index], &error));
        TEST_ASSERT_MSG(document_has_anchor(&document, target.data), "missing anchor %s", target.data);
        baca_string_free(&target);
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
    TEST_ASSERT(baca_document_current_toc(&document, document.sections[1].id) == &document.toc[1]);

    BacaResource resource = {0};
    TEST_ASSERT_MSG(baca_document_load_resource(&document, "OEBPS/Images/pixel.png", &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    baca_resource_free(&resource);
    TEST_ASSERT_MSG(baca_document_load_resource(&document, "OEBPS/Images/vector.svg", &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_STR(resource.mime_type, "image/svg+xml");
    TEST_ASSERT(buffer_contains(resource.data, resource.length, "<svg", 4U));
    baca_resource_free(&resource);
    baca_document_close(&document);
    TEST_ASSERT(document.path == NULL && document.backend == NULL && document.blocks == NULL);

    TEST_ASSERT(baca_test_write_text("document/owned-cleanup/marker", "owned"));
    char *cleanup = baca_test_path("document/owned-cleanup");
    TEST_ASSERT(cleanup != NULL);
    TEST_ASSERT_MSG(baca_epub_open(&document, path, cleanup, &error), "%s", error.message);
    TEST_ASSERT(baca_directory_exists(cleanup));
    baca_document_close(&document);
    TEST_ASSERT(!baca_directory_exists(cleanup));
    free(cleanup);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_epub_image_probe_uses_held_archive(void) {
    char *path = NULL;
    TEST_ASSERT(build_epub2(&path));
    BacaError error = {0};
    BacaString held_string = {0};
    TEST_ASSERT(baca_string_append(&held_string, path, &error));
    TEST_ASSERT(baca_string_append(&held_string, ".held", &error));
    char *held_path = baca_string_take(&held_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);

    BacaDocument document = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    const BacaImageBlock *pixel = find_image(&document, "Pixel");
    TEST_ASSERT(pixel != NULL && pixel->intrinsic_width == 0 && pixel->intrinsic_height == 0);
    TEST_ASSERT(rename(path, held_path) == 0);
    TEST_ASSERT(unlink(held_path) == 0);
    TEST_ASSERT(baca_write_file(path, "replacement", 11U, &error));

    baca_document_probe_images(&document, true);
    pixel = find_image(&document, "Pixel");
    TEST_ASSERT(pixel != NULL && !pixel->broken);
    TEST_ASSERT_INT(pixel->intrinsic_width, 1);
    TEST_ASSERT_INT(pixel->intrinsic_height, 1);
    BacaResource resource = {0};
    TEST_ASSERT_MSG(baca_document_load_resource(&document, pixel->uri, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    baca_resource_free(&resource);
    baca_document_close(&document);
    free(held_path);
    free(path);
    return BACA_TEST_PASS;
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
    return create_epub("document/epub3.epub", opf, members, BACA_ARRAY_LEN(members), path);
}

static BacaTestResult test_epub3_nav_and_svg_spine(void) {
    char *path = NULL;
    TEST_ASSERT(build_epub3(&path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    baca_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(document.section_count, 3U);
    TEST_ASSERT_STR(document.sections[1].id, "OEBPS/Images/page.svg");
    TEST_ASSERT(!document.sections[1].linear);
    TEST_ASSERT_SIZE(document.sections[1].block_count, 1U);
    const BacaBlock *svg_block = &document.blocks[document.sections[1].first_block];
    TEST_ASSERT_INT(svg_block->kind, BACA_BLOCK_IMAGE);
    TEST_ASSERT_STR(svg_block->value.image.uri, "OEBPS/Images/page.svg");
    TEST_ASSERT_INT(svg_block->value.image.intrinsic_width, 2);
    TEST_ASSERT_INT(svg_block->value.image.intrinsic_height, 2);
    TEST_ASSERT(!svg_block->value.image.broken);
    TEST_ASSERT_SIZE(document.toc_count, 3U);
    TEST_ASSERT_STR(document.toc[0].label, "One");
    TEST_ASSERT_INT((int)document.toc[1].depth, 1);
    TEST_ASSERT_STR(document.toc[1].target, "OEBPS/Images/page.svg");
    TEST_ASSERT_STR(document.toc[2].target, "OEBPS/two.xhtml#second");
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
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
    BacaString opf = {0};
    BacaError error = {0};
    bool built = baca_string_append(&opf, prefix, &error) && baca_string_append(&opf, extra_manifest, &error) &&
                 baca_string_append(&opf, middle, &error) && baca_string_append(&opf, extra_spine, &error) &&
                 baca_string_append(&opf, suffix, &error);
    static const char chapter[] = "<html><body><p id=\"start\">Minimal body</p></body></html>";
    FixtureMember *members = calloc(extra_count + 1U, sizeof(*members));
    if (!built || members == NULL) {
        baca_string_free(&opf);
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
    baca_string_free(&opf);
    return created;
}

static BacaTestResult test_missing_toc_fallback(void) {
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/no-toc.epub", "", "", NULL, 0U, &path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.toc_count, 0U);
    TEST_ASSERT_SIZE(document.section_count, 1U);
    TEST_ASSERT(find_text(&document, "Minimal body") != NULL);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_document_instance_identity_changes_on_reopen(void) {
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/instance-id.epub", "", "", NULL, 0U, &path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    const uint64_t first_instance_id = document.instance_id;
    TEST_ASSERT(first_instance_id != 0U);
    baca_document_close(&document);
    TEST_ASSERT(document.instance_id == 0U);

    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT(document.instance_id != 0U && document.instance_id != first_instance_id);
    baca_document_close(&document);

    document.instance_id = first_instance_id;
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    document.instance_id = 0U;
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_data_resources(void) {
    BacaDocument document = {0};
    BacaResource resource = {0};
    BacaError error = {0};
    TEST_ASSERT(baca_document_load_resource(&document, "data:image/png;base64,iVBORw0KGgo=", &resource, &error));
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    TEST_ASSERT_SIZE(resource.length, 8U);
    TEST_ASSERT(memcmp(resource.data, "\x89PNG\r\n\x1a\n", 8U) == 0);
    baca_resource_free(&resource);
    TEST_ASSERT(baca_document_load_resource(&document, "data:text/plain,a%20b%00c", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, 5U);
    TEST_ASSERT(memcmp(resource.data, "a b\0c", 5U) == 0);
    baca_resource_free(&resource);
    TEST_ASSERT(!baca_document_load_resource(&document, "data:text/plain;base64,%%%", &resource, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_png_structure_resource_and_probe(void) {
    static const char relative[] = "document/standalone image.png";
    TEST_ASSERT(baca_test_write(relative, pixel_png, sizeof(pixel_png)));
    char *path = baca_test_path(relative);
    TEST_ASSERT(path != NULL);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_IMAGE);
    TEST_ASSERT_STR(baca_document_format_name(document.format), "image");
    TEST_ASSERT_STR(document.metadata.title, "standalone image.png");
    TEST_ASSERT_STR(document.metadata.format, "Image");
    TEST_ASSERT_SIZE(document.section_count, 1U);
    TEST_ASSERT_SIZE(document.block_count, 1U);
    TEST_ASSERT_STR(document.sections[0].id, path);
    TEST_ASSERT_SIZE(document.sections[0].first_block, 0U);
    TEST_ASSERT_SIZE(document.sections[0].block_count, 1U);
    TEST_ASSERT(document.sections[0].linear);
    TEST_ASSERT_INT(document.blocks[0].kind, BACA_BLOCK_IMAGE);
    TEST_ASSERT_STR(document.blocks[0].section_id, path);
    TEST_ASSERT_STR(document.blocks[0].value.image.uri, path);
    TEST_ASSERT_STR(document.blocks[0].value.image.alt, "standalone image.png");
    TEST_ASSERT_INT(document.blocks[0].value.image.page_index, -1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 0);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 0);
    TEST_ASSERT(!document.blocks[0].value.image.broken);

    BacaString wrong_uri = {0};
    TEST_ASSERT(baca_string_append(&wrong_uri, path, &error));
    TEST_ASSERT(baca_string_append(&wrong_uri, "#other", &error));
    BacaResource resource = {0};
    TEST_ASSERT(!baca_document_load_resource(&document, wrong_uri.data, &resource, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_NOT_FOUND);
    baca_string_free(&wrong_uri);

    TEST_ASSERT_MSG(baca_document_load_resource(&document, path, &resource, &error), "%s", error.message);
    TEST_ASSERT(resource.data != NULL && resource.data != pixel_png);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(resource.mime_type == NULL);
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    resource.data[0] = 0U;
    baca_resource_free(&resource);
    TEST_ASSERT(resource.data == NULL && resource.length == 0U && resource.mime_type == NULL);
    TEST_ASSERT_MSG(baca_document_load_resource(&document, path, &resource, &error), "%s", error.message);
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    baca_resource_free(&resource);

    baca_document_probe_images(&document, false);
    TEST_ASSERT(document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 0);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 0);
    baca_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 1);

    baca_document_close(&document);
    TEST_ASSERT(document.path == NULL && document.blocks == NULL && document.sections == NULL);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_snapshot_survives_replacement_and_mutation(void) {
    static const char relative[] = "document/stable-snapshot.png";
    TEST_ASSERT(baca_test_write(relative, pixel_png, sizeof(pixel_png)));
    char *path = baca_test_path(relative);
    TEST_ASSERT(path != NULL);
    BacaString held_path_string = {0};
    BacaError error = {0};
    TEST_ASSERT(baca_string_append(&held_path_string, path, &error));
    TEST_ASSERT(baca_string_append(&held_path_string, ".held", &error));
    char *held_path = baca_string_take(&held_path_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);

    struct stat original_status;
    TEST_ASSERT(stat(path, &original_status) == 0);
    BacaDocument document = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_STR(document.path, path);
    TEST_ASSERT(rename(path, held_path) == 0);

    unsigned char replacement[sizeof(pixel_png)];
    memset(replacement, 0x5a, sizeof(replacement));
    TEST_ASSERT(baca_write_file(path, replacement, sizeof(replacement), &error));
    struct stat held_status;
    struct stat replacement_status;
    TEST_ASSERT(stat(held_path, &held_status) == 0);
    TEST_ASSERT(stat(path, &replacement_status) == 0);
    TEST_ASSERT(held_status.st_dev == original_status.st_dev && held_status.st_ino == original_status.st_ino);
    TEST_ASSERT(replacement_status.st_dev != original_status.st_dev ||
                replacement_status.st_ino != original_status.st_ino);
    TEST_ASSERT(replacement_status.st_size == original_status.st_size);

    BacaResource resource = {0};
    TEST_ASSERT_MSG(baca_document_load_resource(&document, document.path, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    baca_resource_free(&resource);
    baca_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 1);

    unsigned char mutation[sizeof(pixel_png)];
    memset(mutation, 0xa5, sizeof(mutation));
    TEST_ASSERT(baca_write_file(held_path, mutation, sizeof(mutation), &error));
    TEST_ASSERT_MSG(baca_document_load_resource(&document, document.path, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    baca_resource_free(&resource);
    baca_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 1);

    TEST_ASSERT(unlink(held_path) == 0);
    char *export_directory = baca_make_temp_directory("mereader-tui-image-test-", &error);
    TEST_ASSERT_MSG(export_directory != NULL, "%s", error.message);
    char *export_path = baca_path_join(export_directory, "stable-snapshot.png", &error);
    TEST_ASSERT(export_path != NULL);
    TEST_ASSERT_MSG(baca_image_export_original(&document, export_path, &error), "%s", error.message);
    struct stat export_status;
    TEST_ASSERT(stat(export_path, &export_status) == 0 && S_ISREG(export_status.st_mode));
    TEST_ASSERT((export_status.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    TEST_ASSERT_SIZE((size_t)export_status.st_size, sizeof(pixel_png));
    TEST_ASSERT(!baca_image_export_original(&document, export_path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_IO);

    baca_document_close(&document);
    int reused_descriptor = open(path, O_RDONLY | O_CLOEXEC);
    TEST_ASSERT(reused_descriptor >= 0);
    TEST_ASSERT(close(reused_descriptor) == 0);

    BacaBuffer exported = {0};
    BacaBuffer replacement_bytes = {0};
    TEST_ASSERT_MSG(baca_read_file(export_path, &exported, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_read_file(path, &replacement_bytes, &error), "%s", error.message);
    TEST_ASSERT_SIZE(exported.length, sizeof(pixel_png));
    TEST_ASSERT_SIZE(replacement_bytes.length, sizeof(replacement));
    TEST_ASSERT(memcmp(exported.data, pixel_png, sizeof(pixel_png)) == 0);
    TEST_ASSERT(memcmp(replacement_bytes.data, replacement, sizeof(replacement)) == 0);
    baca_buffer_free(&replacement_bytes);
    baca_buffer_free(&exported);
    TEST_ASSERT(baca_remove_tree(export_directory, &error));
    free(export_path);
    free(export_directory);
    free(held_path);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_backend_rejects_detected_identity_mismatch(void) {
    static const char relative[] = "document/identity-mismatch.png";
    TEST_ASSERT(baca_test_write(relative, pixel_png, sizeof(pixel_png)));
    char *path = baca_test_path(relative);
    TEST_ASSERT(path != NULL);
    BacaError error = {0};
    BacaString held_string = {0};
    TEST_ASSERT(baca_string_append(&held_string, path, &error));
    TEST_ASSERT(baca_string_append(&held_string, ".held", &error));
    char *held_path = baca_string_take(&held_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);

    struct stat detected_identity;
    TEST_ASSERT(stat(path, &detected_identity) == 0);
    TEST_ASSERT(rename(path, held_path) == 0);
    TEST_ASSERT(baca_write_file(path, pixel_png, sizeof(pixel_png), &error));

    BacaDocument document = {.path = baca_strdup(path, &error)};
    TEST_ASSERT(document.path != NULL);
    TEST_ASSERT(!baca_image_open(&document, document.path, &detected_identity, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "format detection") != NULL);
    TEST_ASSERT(document.backend == NULL && document.ops == NULL);
    baca_document_close(&document);
    free(held_path);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_large_export_streams_held_original(void) {
    static const char relative[] = "document/large-export.bmp";
    static const unsigned char start_marker[] = {'B', 'M', 'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
    static const unsigned char end_marker[] = {'e', 'n', 'd', '-', 'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
    TEST_ASSERT(baca_test_write(relative, NULL, 0U));
    char *path = baca_test_path(relative);
    TEST_ASSERT(path != NULL);
    const off_t size = (off_t)BACA_GRAPHICS_MAX_INPUT_BYTES + 1;
    int source = open(path, O_WRONLY | O_CLOEXEC);
    TEST_ASSERT(source >= 0);
    TEST_ASSERT(ftruncate(source, size) == 0);
    TEST_ASSERT(pwrite(source, start_marker, sizeof(start_marker), 0) == (ssize_t)sizeof(start_marker));
    TEST_ASSERT(pwrite(source, end_marker, sizeof(end_marker), size - (off_t)sizeof(end_marker)) ==
                (ssize_t)sizeof(end_marker));
    TEST_ASSERT(close(source) == 0);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    BacaString held_string = {0};
    TEST_ASSERT(baca_string_append(&held_string, path, &error));
    TEST_ASSERT(baca_string_append(&held_string, ".held", &error));
    char *held_path = baca_string_take(&held_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);
    TEST_ASSERT(rename(path, held_path) == 0);
    TEST_ASSERT(unlink(held_path) == 0);
    TEST_ASSERT(baca_write_file(path, pixel_png, sizeof(pixel_png), &error));

    char *directory = baca_make_temp_directory("mereader-tui-image-large-test-", &error);
    TEST_ASSERT_MSG(directory != NULL, "%s", error.message);
    char *destination = baca_path_join(directory, "large-export.bmp", &error);
    TEST_ASSERT(destination != NULL);
    TEST_ASSERT_MSG(baca_image_export_original(&document, destination, &error), "%s", error.message);
    baca_document_close(&document);

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
    TEST_ASSERT(baca_remove_tree(directory, &error));
    free(destination);
    free(directory);
    free(held_path);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_export_cap_and_partial_cleanup(void) {
    static const char over_cap_relative[] = "document/export-over-cap.bmp";
    TEST_ASSERT(baca_test_write(over_cap_relative, NULL, 0U));
    char *over_cap_path = baca_test_path(over_cap_relative);
    TEST_ASSERT(over_cap_path != NULL);
    int source = open(over_cap_path, O_WRONLY | O_CLOEXEC);
    TEST_ASSERT(source >= 0);
    TEST_ASSERT(ftruncate(source, (off_t)BACA_DOCUMENT_MAX_RETAINED_BYTES + 1) == 0);
    TEST_ASSERT(close(source) == 0);

    BacaError error = {0};
    BacaDocument over_cap = {0};
    TEST_ASSERT_MSG(baca_document_open(&over_cap, over_cap_path, &error), "%s", error.message);
    char *directory = baca_make_temp_directory("mereader-tui-image-cap-test-", &error);
    TEST_ASSERT_MSG(directory != NULL, "%s", error.message);
    char *destination = baca_path_join(directory, "over-cap.bmp", &error);
    TEST_ASSERT(destination != NULL);
    TEST_ASSERT(!baca_image_export_original(&over_cap, destination, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "256 MiB") != NULL);
    errno = 0;
    TEST_ASSERT(lstat(destination, &(struct stat){0}) != 0 && errno == ENOENT);
    baca_document_close(&over_cap);
    TEST_ASSERT(baca_remove_tree(directory, &error));
    free(destination);
    free(directory);
    free(over_cap_path);

    static const char partial_relative[] = "document/export-partial.bmp";
    TEST_ASSERT(baca_test_write(partial_relative, NULL, 0U));
    char *partial_path = baca_test_path(partial_relative);
    TEST_ASSERT(partial_path != NULL);
    source = open(partial_path, O_WRONLY | O_CLOEXEC);
    TEST_ASSERT(source >= 0);
    TEST_ASSERT(ftruncate(source, 1024 * 1024) == 0);
    TEST_ASSERT(close(source) == 0);
    BacaDocument partial = {0};
    TEST_ASSERT_MSG(baca_document_open(&partial, partial_path, &error), "%s", error.message);
    directory = baca_make_temp_directory("mereader-tui-image-partial-test-", &error);
    TEST_ASSERT_MSG(directory != NULL, "%s", error.message);
    destination = baca_path_join(directory, "partial.bmp", &error);
    TEST_ASSERT(destination != NULL);

    const pid_t child = fork();
    TEST_ASSERT(child >= 0);
    if (child == 0) {
        const struct rlimit limit = {.rlim_cur = 4096U, .rlim_max = 4096U};
        BacaError child_error = {0};
        const bool ready = signal(SIGXFSZ, SIG_IGN) != SIG_ERR &&
                           setrlimit(RLIMIT_FSIZE, &limit) == 0;
        const bool exported = ready && baca_image_export_original(&partial, destination, &child_error);
        errno = 0;
        const bool removed = lstat(destination, &(struct stat){0}) != 0 && errno == ENOENT;
        _exit(!exported && child_error.code == BACA_ERROR_IO && removed ? 0 : 1);
    }
    int status = 0;
    TEST_ASSERT(waitpid(child, &status, 0) == child);
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    errno = 0;
    TEST_ASSERT(lstat(destination, &(struct stat){0}) != 0 && errno == ENOENT);
    baca_document_close(&partial);
    TEST_ASSERT(baca_remove_tree(directory, &error));
    free(destination);
    free(directory);
    free(partial_path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_corrupt_extensions_use_placeholder(void) {
    static const char corrupt[] = "not an image";
    static const char *const extensions[] = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "svg"};
    for (size_t index = 0U; index < BACA_ARRAY_LEN(extensions); ++index) {
        char relative[64] = {0};
        const int length = snprintf(relative, sizeof(relative), "document/corrupt.%s", extensions[index]);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(relative));
        TEST_ASSERT(baca_test_write(relative, corrupt, sizeof(corrupt) - 1U));
        char *path = baca_test_path(relative);
        TEST_ASSERT(path != NULL);

        BacaDocument document = {0};
        BacaError error = {0};
        TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
        TEST_ASSERT_INT(document.format, BACA_FORMAT_IMAGE);
        TEST_ASSERT(!document.blocks[0].value.image.broken);
        baca_document_probe_images(&document, true);
        TEST_ASSERT(document.blocks[0].value.image.broken);
        TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 0);
        TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 0);

        BacaResource resource = {0};
        TEST_ASSERT_MSG(baca_document_load_resource(&document, path, &resource, &error), "%s", error.message);
        TEST_ASSERT_SIZE(resource.length, sizeof(corrupt) - 1U);
        TEST_ASSERT(resource.mime_type == NULL);
        TEST_ASSERT(memcmp(resource.data, corrupt, sizeof(corrupt) - 1U) == 0);
        baca_resource_free(&resource);
        baca_document_close(&document);
        free(path);
    }
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_png_magic_without_image_extension(void) {
    static const char relative[] = "document/png-magic.unknown";
    TEST_ASSERT(baca_test_write(relative, pixel_png, sizeof(pixel_png)));
    char *path = baca_test_path(relative);
    TEST_ASSERT(path != NULL);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_IMAGE);
    TEST_ASSERT_STR(document.blocks[0].value.image.uri, path);
    baca_document_probe_images(&document, true);
    TEST_ASSERT(!document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 1);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_valid_jpeg_decode_and_probe(void) {
    BacaDocument decoder = {0};
    BacaResource jpeg = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_load_resource(&decoder, pixel_jpeg_data_uri, &jpeg, &error), "%s",
                    error.message);
    int width = 0;
    int height = 0;
    TEST_ASSERT_MSG(baca_graphics_probe_resource(&jpeg, &width, &height, &error), "%s", error.message);
    TEST_ASSERT_INT(width, 2);
    TEST_ASSERT_INT(height, 3);
    const StandaloneFormatFixture fixture = {
        .name = "jpeg",
        .extension = "jpg",
        .loader = "jpeg",
        .data = jpeg.data,
        .length = jpeg.length,
    };
    const BacaTestResult result = test_valid_standalone_format(&fixture);
    baca_resource_free(&jpeg);
    return result;
}

static BacaTestResult test_standalone_valid_static_gif_decode_and_probe(void) {
    const StandaloneFormatFixture fixture = {
        .name = "gif",
        .extension = "gif",
        .loader = "gif",
        .data = pixel_gif,
        .length = sizeof(pixel_gif),
    };
    return test_valid_standalone_format(&fixture);
}

static BacaTestResult test_standalone_valid_webp_magic_decode_and_probe(void) {
    const StandaloneFormatFixture fixture = {
        .name = "webp",
        .extension = "webp",
        .loader = "webp",
        .data = pixel_webp,
        .length = sizeof(pixel_webp),
        .optional_loader = true,
    };
    return test_valid_standalone_format(&fixture);
}

static BacaTestResult test_standalone_valid_bmp_magic_decode_and_probe(void) {
    const StandaloneFormatFixture fixture = {
        .name = "bmp",
        .extension = "bmp",
        .loader = "bmp",
        .data = pixel_bmp,
        .length = sizeof(pixel_bmp),
    };
    return test_valid_standalone_format(&fixture);
}

static BacaTestResult test_standalone_valid_svg_magic_decode_and_probe(void) {
    const StandaloneFormatFixture fixture = {
        .name = "svg",
        .extension = "svg",
        .loader = "svg",
        .data = pixel_svg,
        .length = sizeof(pixel_svg) - 1U,
        .optional_loader = true,
    };
    return test_valid_standalone_format(&fixture);
}

static BacaTestResult test_standalone_avif_is_unsupported(void) {
    static const unsigned char avif[] = {
        0x00U, 0x00U, 0x00U, 0x18U, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f',
        0x00U, 0x00U, 0x00U, 0x00U, 'a', 'v', 'i', 'f', 'm', 'i', 'f', '1',
    };
    static const char relative[] = "document/unsupported.avif";
    TEST_ASSERT(baca_test_write(relative, avif, sizeof(avif)));
    char *path = baca_test_path(relative);
    TEST_ASSERT(path != NULL);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
    TEST_ASSERT(document.path == NULL && document.format == BACA_FORMAT_UNKNOWN);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_standalone_oversized_input_uses_placeholder(void) {
    static const char relative[] = "document/oversized.bmp";
    TEST_ASSERT(baca_test_write(relative, NULL, 0U));
    char *path = baca_test_path(relative);
    TEST_ASSERT(path != NULL);
    int descriptor = open(path, O_WRONLY | O_CLOEXEC);
    TEST_ASSERT(descriptor >= 0);
    const off_t oversized = (off_t)BACA_GRAPHICS_MAX_INPUT_BYTES + 1;
    TEST_ASSERT(ftruncate(descriptor, oversized) == 0);
    TEST_ASSERT(close(descriptor) == 0);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_IMAGE);

    BacaString held_path_string = {0};
    TEST_ASSERT(baca_string_append(&held_path_string, path, &error));
    TEST_ASSERT(baca_string_append(&held_path_string, ".held", &error));
    char *held_path = baca_string_take(&held_path_string);
    TEST_ASSERT(held_path != NULL);
    (void)unlink(held_path);
    TEST_ASSERT(rename(path, held_path) == 0);
    TEST_ASSERT(baca_write_file(path, pixel_png, sizeof(pixel_png), &error));

    baca_document_probe_images(&document, true);
    TEST_ASSERT(document.blocks[0].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 0);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_height, 0);

    BacaResource resource = {0};
    TEST_ASSERT(!baca_document_load_resource(&document, path, &resource, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "size limit") != NULL);
    TEST_ASSERT(resource.data == NULL && resource.length == 0U && resource.mime_type == NULL);
    baca_document_close(&document);
    free(held_path);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_fifo_and_special_files_are_rejected_without_blocking(void) {
    char *fifo = baca_test_path("document/nonblocking.fifo");
    TEST_ASSERT(fifo != NULL);
    char *directory = baca_path_dirname(fifo, NULL);
    TEST_ASSERT(directory != NULL);
    TEST_ASSERT(baca_mkdirs(directory, NULL));
    free(directory);
    (void)unlink(fifo);
    TEST_ASSERT(mkfifo(fifo, 0600) == 0);

    DocumentOpenResult result = {0};
    TEST_ASSERT_MSG(document_open_returns_without_blocking(fifo, &result),
                    "opening a FIFO blocked instead of rejecting it");
    TEST_ASSERT(!result.opened);
    TEST_ASSERT(result.code != BACA_ERROR_NONE);

    result = (DocumentOpenResult){0};
    TEST_ASSERT_MSG(document_open_returns_without_blocking("/dev/null", &result),
                    "opening a special file did not return");
    TEST_ASSERT(!result.opened);
    TEST_ASSERT(result.code != BACA_ERROR_NONE);
    free(fifo);
    return BACA_TEST_PASS;
}

static BacaTestResult test_image_probe_dedupe_placeholder_and_aggregate_limit(void) {
    char *path = NULL;
    TEST_ASSERT(create_numbered_probe_archive("document/probe-same.zip", "same", 1U, pixel_png,
                                              sizeof(pixel_png), &path));
    ProbeBackend backend = {.resource_size = sizeof(pixel_png)};
    BacaDocument document = {
        .path = path,
        .format = BACA_FORMAT_EPUB,
        .backend = &backend,
        .ops = &probe_document_ops,
    };
    BacaError error = {0};
    TEST_ASSERT(append_probe_image(&document, "same-0.png", &error));
    TEST_ASSERT(append_probe_image(&document, "same-0.png", &error));
    baca_document_probe_images(&document, false);
    TEST_ASSERT_SIZE(backend.loads, 0U);
    TEST_ASSERT(document.blocks[0].value.image.broken && document.blocks[1].value.image.broken);

    baca_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(backend.loads, 1U);
    TEST_ASSERT(!document.blocks[0].value.image.broken && !document.blocks[1].value.image.broken);
    TEST_ASSERT_INT(document.blocks[0].value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(document.blocks[1].value.image.intrinsic_height, 1);
    baca_document_close(&document);

    path = NULL;
    TEST_ASSERT(create_numbered_probe_archive("document/probe-count.zip", "image",
                                              BACA_GRAPHICS_MAX_PROBES + 5U, "x", 1U, &path));
    backend = (ProbeBackend){.resource_size = 1U, .corrupt = true};
    document = (BacaDocument){
        .path = path,
        .format = BACA_FORMAT_EPUB,
        .backend = &backend,
        .ops = &probe_document_ops,
    };
    for (size_t index = 0U; index < BACA_GRAPHICS_MAX_PROBES + 5U; ++index) {
        char uri[64] = {0};
        const int length = snprintf(uri, sizeof(uri), "image-%zu.png", index);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(uri));
        TEST_ASSERT(append_probe_image(&document, uri, &error));
    }
    baca_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(backend.loads, BACA_GRAPHICS_MAX_PROBES);
    TEST_ASSERT(document.blocks[0].value.image.broken);
    TEST_ASSERT(document.blocks[document.block_count - 1U].value.image.broken);
    baca_document_close(&document);

    path = NULL;
    TEST_ASSERT(create_numbered_probe_archive("document/probe-budget.zip", "budget", 6U, "x", 1U,
                                              &path));
    char *oversized_path = baca_strdup(path, &error);
    TEST_ASSERT(oversized_path != NULL);

    backend = (ProbeBackend){.resource_size = BACA_GRAPHICS_MAX_INPUT_BYTES, .corrupt = true};
    document = (BacaDocument){
        .path = path,
        .format = BACA_FORMAT_EPUB,
        .backend = &backend,
        .ops = &probe_document_ops,
    };
    for (size_t index = 0U; index < 5U; ++index) {
        char uri[64] = {0};
        const int length = snprintf(uri, sizeof(uri), "budget-%zu.png", index);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(uri));
        TEST_ASSERT(append_probe_image(&document, uri, &error));
    }
    baca_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(backend.loads, BACA_GRAPHICS_MAX_PROBE_BYTES / BACA_GRAPHICS_MAX_INPUT_BYTES);
    baca_document_close(&document);

    backend = (ProbeBackend){
        .resource_size = 1U,
        .override_uri = "budget-5.png",
        .override_size = BACA_GRAPHICS_MAX_INPUT_BYTES + 1U,
        .corrupt = true,
    };
    document = (BacaDocument){
        .path = oversized_path,
        .format = BACA_FORMAT_EPUB,
        .backend = &backend,
        .ops = &probe_document_ops,
    };
    TEST_ASSERT(append_probe_image(&document, "budget-5.png", &error));
    baca_document_probe_images(&document, true);
    TEST_ASSERT_SIZE(backend.loads, 0U);
    TEST_ASSERT(document.blocks[0].value.image.broken);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_traversal_errors(void) {
    char *path = baca_test_path("document/archive-traversal.epub");
    TEST_ASSERT(path != NULL);
    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    TEST_ASSERT(archive != NULL);
    TEST_ASSERT(zip_add_bytes(archive, "mimetype", "application/epub+zip", 20U, true, NULL));
    TEST_ASSERT(zip_add_bytes(archive, "../escape", "x", 1U, false, NULL));
    TEST_ASSERT(close_archive(archive));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "unsafe EPUB archive path") != NULL);
    free(path);

    static const char unsafe_opf[] =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>x</dc:title></metadata>"
        "<manifest><item id=\"bad\" href=\"../../escape.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine><itemref idref=\"bad\"/></spine></package>";
    path = NULL;
    TEST_ASSERT(create_epub("document/reference-traversal.epub", unsafe_opf, NULL, 0U, &path));
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "escapes the document root") != NULL);
    free(path);
    return BACA_TEST_PASS;
}

static bool patch_member_size(const char *path, const char *member_name, uint32_t size) {
    BacaError error = {0};
    BacaBuffer archive = {0};
    if (!baca_read_file(path, &archive, &error)) {
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
    bool written = patched && baca_write_file(path, archive.data, archive.length, &error);
    baca_buffer_free(&archive);
    return written;
}

static BacaTestResult test_oversized_member_error(void) {
    static const FixtureMember extra[] = {
        {.name = "OEBPS/huge.bin", .data = "x", .length = 1U},
    };
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/oversized.epub",
                                   "<item id=\"huge\" href=\"huge.bin\" media-type=\"application/octet-stream\"/>",
                                   "", extra, BACA_ARRAY_LEN(extra), &path));
    TEST_ASSERT(patch_member_size(path, "OEBPS/huge.bin", 64U * 1024U * 1024U + 1U));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "size limit") != NULL);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_encryption_document_error(void) {
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
    TEST_ASSERT(build_minimal_epub("document/encrypted-metadata.epub", "", "", extra, BACA_ARRAY_LEN(extra),
                                   &path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
    TEST_ASSERT(strstr(error.message, "encrypted EPUB content") != NULL);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_encrypted_zip_member_error(void) {
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
    if (zip_file_set_encryption(archive, (zip_uint64_t)index, ZIP_EM_AES_256, "test-password") != 0) {
        zip_discard(archive);
        free(path);
        return baca_test_skip("libzip cannot create AES-encrypted fixtures");
    }
    TEST_ASSERT(close_archive(archive));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
    TEST_ASSERT(strstr(error.message, "encrypted EPUB members") != NULL);
    free(path);
    return BACA_TEST_PASS;
}

static char *configured_mobitool_path(const char *configured) {
    if (configured == NULL || configured[0] == '\0' || strchr(configured, '/') == NULL) {
        return NULL;
    }
    BacaError error = {0};
    return baca_realpath(configured, &error);
}

static BacaTestResult test_optional_real_mobi(void) {
    const char *configured_tool = getenv("MEREADER_TUI_TEST_MOBITOOL");
    const char *configured_sample = getenv("MEREADER_TUI_TEST_MOBI_SAMPLE");
    if (configured_tool == NULL || configured_tool[0] == '\0' || configured_sample == NULL ||
        configured_sample[0] == '\0') {
        return baca_test_skip(
            "set MEREADER_TUI_TEST_MOBITOOL and MEREADER_TUI_TEST_MOBI_SAMPLE for real conversion");
    }
    char *tool = configured_mobitool_path(configured_tool);
    BacaError error = {0};
    char *sample = baca_realpath(configured_sample, &error);
    if (tool == NULL || access(tool, X_OK) != 0 || sample == NULL) {
        free(tool);
        free(sample);
        return baca_test_fail_at(__FILE__, __LINE__, "configured MOBI tool/sample is not accessible");
    }
    TEST_ASSERT(baca_test_mkdir("mobi-bin"));
    char *shim = baca_test_path("mobi-bin/mobitool");
    char *bin = baca_test_path("mobi-bin");
    TEST_ASSERT(shim != NULL && bin != NULL);
    TEST_ASSERT(symlink(tool, shim) == 0);
    const char *old_path_value = getenv("PATH");
    char *old_path = baca_strdup(old_path_value == NULL ? "" : old_path_value, &error);
    TEST_ASSERT(old_path != NULL);
    BacaString path_value = {0};
    TEST_ASSERT(baca_string_append(&path_value, bin, &error));
    TEST_ASSERT(baca_string_append_char(&path_value, ':', &error));
    TEST_ASSERT(baca_string_append(&path_value, old_path, &error));
    TEST_ASSERT(setenv("PATH", path_value.data, 1) == 0);

    size_t before = baca_test_count_directories("tmp", "mereader-tui-mobi-");
    TEST_ASSERT(before != SIZE_MAX);
    BacaDocument document = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, sample, &error), "%s", error.message);
    size_t during = baca_test_count_directories("tmp", "mereader-tui-mobi-");
    TEST_ASSERT_SIZE(during, before + 1U);
    TEST_ASSERT(document.format == BACA_FORMAT_MOBI || document.format == BACA_FORMAT_AZW);
    TEST_ASSERT(document.block_count > 0U && document.section_count > 0U);
    TEST_ASSERT(document.metadata.title != NULL || document.metadata.creator != NULL);
    baca_document_close(&document);
    TEST_ASSERT_SIZE(baca_test_count_directories("tmp", "mereader-tui-mobi-"), before);
    TEST_ASSERT(setenv("PATH", old_path, 1) == 0);
    baca_string_free(&path_value);
    free(old_path);
    free(shim);
    free(bin);
    free(tool);
    free(sample);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_document_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
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
        {.name = "optional_real_mobi", .function = test_optional_real_mobi},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
