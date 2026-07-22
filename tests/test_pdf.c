#include "test_support.h"

#include "baca/app.h"
#include "baca/document.h"
#include "baca/document_backend.h"
#include "baca/graphics.h"
#include "baca/layout.h"

#include <cairo-pdf.h>
#include <cairo.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <glib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zip.h>

typedef struct PdfCellCapture {
    size_t count;
    int first_row;
    int last_row;
} PdfCellCapture;

typedef struct PdfPtyResult {
    BacaString output;
    BacaString opened;
    int status;
    bool fixed_rendered;
    bool help_opened;
    bool help_shows_configured_toggle;
    bool toggled_to_text;
    bool search_reached_page;
    bool resize_preserved_page;
    bool internal_link_reached_page;
    bool external_link_opened;
    bool internal_toc_reached_page;
    bool external_toc_opened;
    bool page_opened;
    bool completed;
} PdfPtyResult;

enum {
    PDF_OUTLINE_NODE_LIMIT = 10000,
    PDF_LINK_TARGET_BYTE_LIMIT = 8 * 1024 * 1024,
    PDF_STRESS_TARGET_BYTES = 64 * 1024,
    PDF_STRESS_TARGET_COUNT = PDF_LINK_TARGET_BYTE_LIMIT / PDF_STRESS_TARGET_BYTES + 1,
};

static const char PDF_ROTATED[] =
    "%PDF-1.7\n"
    "%TEST\n"
    "1 0 obj\n"
    "<< /Pages 3 0 R /Type /Catalog >>\n"
    "endobj\n"
    "2 0 obj\n"
    "<< /CreationDate (D:20260719051045+06'00) /Producer "
    "(cairo 1.18.4 \\(https://cairographics.org\\)) >>\n"
    "endobj\n"
    "3 0 obj\n"
    "<< /Count 1 /Kids [ 4 0 R ] /Type /Pages >>\n"
    "endobj\n"
    "4 0 obj\n"
    "<< /Contents 5 0 R /Group << /CS /DeviceRGB /I true /S /Transparency /Type /Group >> "
    "/MediaBox [ 0 0 200 100 ] /Parent 3 0 R /Resources 6 0 R /Rotate 90 /StructParents 0 /Type /Page >>\n"
    "endobj\n"
    "5 0 obj\n"
    "<< /Length 22 >>\n"
    "stream\n"
    "1 0 0 -1 0 100 cm\n"
    "q\n"
    "Q\n"
    "endstream\n"
    "endobj\n"
    "6 0 obj\n"
    "<< >>\n"
    "endobj\n"
    "xref\n"
    "0 7\n"
    "0000000000 65535 f \n"
    "0000000015 00000 n \n"
    "0000000064 00000 n \n"
    "0000000179 00000 n \n"
    "0000000238 00000 n \n"
    "0000000438 00000 n \n"
    "0000000509 00000 n \n"
    "trailer << /Info 2 0 R /Root 1 0 R /Size 7 "
    "/ID [<F7D9AE4D4AD2B8B816408CD4F6492860><F7D9AE4D4AD2B8B816408CD4F6492860>] >>\n"
    "startxref\n"
    "530\n"
    "%%EOF\n";

static const char PDF_ZERO_PAGE[] =
    "%PDF-1.3\n"
    "%TEST\n"
    "1 0 obj\n"
    "<< /Pages 2 0 R /Type /Catalog >>\n"
    "endobj\n"
    "2 0 obj\n"
    "<< /Count 0 /Kids [ ] /Type /Pages >>\n"
    "endobj\n"
    "xref\n"
    "0 3\n"
    "0000000000 65535 f \n"
    "0000000015 00000 n \n"
    "0000000064 00000 n \n"
    "trailer << /Root 1 0 R /Size 3 "
    "/ID [<c7c823668a4ee9fc0eb73128629609ce><c7c823668a4ee9fc0eb73128629609ce>] >>\n"
    "startxref\n"
    "117\n"
    "%%EOF\n";

static const char PDF_ENCRYPTED[] =
    "%PDF-1.7\n"
    "%TEST\n"
    "1 0 obj\n"
    "<< /Extensions << /ADBE << /BaseVersion /1.7 /ExtensionLevel 8 >> >> /Pages 2 0 R /Type /Catalog >>\n"
    "endobj\n"
    "2 0 obj\n"
    "<< /Count 0 /Kids [ ] /Type /Pages >>\n"
    "endobj\n"
    "3 0 obj\n"
    "<< /CF << /StdCF << /AuthEvent /DocOpen /CFM /AESV3 /Length 32 >> >> /Filter /Standard /Length 256 "
    "/O <48f37916c9aa5d6ec9d31eed138ef752bde12951b800866ff266ef911790639d49cdfc2dd077d0159e239db15a5c749f> "
    "/OE <17611742c58b46da7e4afda93349ed886e214d0feb39459bb01540fdf022b143> /P -4 "
    "/Perms <442882724b0e129ffdb66b6179d13b34> /R 6 /StmF /StdCF /StrF /StdCF "
    "/U <ca47111af744ab18089f83ac3a0a410a2c7143f5ce042745d6ea748d15ce970230ea5273a42ad10ea48912391578b4fb> "
    "/UE <fe3efc806443e86a0bb873f1535bb0e5ecb3a6c0f8b1fe9bd43a6a9cbf7eb4ac> /V 5 >>\n"
    "endobj\n"
    "xref\n"
    "0 4\n"
    "0000000000 65535 f \n"
    "0000000015 00000 n \n"
    "0000000130 00000 n \n"
    "0000000183 00000 n \n"
    "trailer << /Root 1 0 R /Size 4 "
    "/ID [<c7c823668a4ee9fc0eb73128629609ce><c273ccfdf80e2974884aa822de040271>] /Encrypt 3 0 R >>\n"
    "startxref\n"
    "730\n"
    "%%EOF\n";

static bool pdf_capture_cell(void *user_data, int row, int column, uint32_t foreground,
                             uint32_t background) {
    (void)column;
    (void)foreground;
    (void)background;
    PdfCellCapture *capture = user_data;
    if (capture->count == 0U) {
        capture->first_row = row;
    }
    capture->last_row = row;
    ++capture->count;
    return true;
}

static bool pdf_write_all(int descriptor, const void *data, size_t length) {
    const unsigned char *cursor = data;
    unsigned stalled = 0U;
    while (length > 0U) {
        const ssize_t written = write(descriptor, cursor, length);
        if (written > 0) {
            cursor += (size_t)written;
            length -= (size_t)written;
            stalled = 0U;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if ((written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) || written == 0) {
            if (++stalled > 150U) {
                return false;
            }
            struct pollfd output = {.fd = descriptor, .events = POLLOUT};
            int ready = 0;
            do {
                ready = poll(&output, 1U, 100);
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0 || (output.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool pdf_test_opener(void *user_data, const char *target, const char *preferred, BacaError *error) {
    (void)preferred;
    const int descriptor = *(const int *)user_data;
    const size_t length = strlen(target);
    if (!pdf_write_all(descriptor, target, length) || !pdf_write_all(descriptor, "\n", 1U)) {
        baca_error_set(error, BACA_ERROR_IO, "could not capture PDF opener target");
        return false;
    }
    return true;
}

int baca_pdf_pty_child(void) {
    const char *path = getenv("BACA_PDF_PTY_PATH");
    const char *descriptor_value = getenv("BACA_PDF_PTY_OPEN_FD");
    if (path == NULL || descriptor_value == NULL) {
        return 121;
    }
    char *end = NULL;
    const long parsed = strtol(descriptor_value, &end, 10);
    if (end == descriptor_value || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        return 122;
    }
    int descriptor = (int)parsed;
    BacaApp app = {0};
    BacaError error = {0};
    if (!baca_app_init(&app, path, true, &error)) {
        (void)fprintf(stderr, "PDF PTY init: %s\n", error.message);
        return 120;
    }
    const char *initial_progress = getenv("BACA_PDF_PTY_INITIAL_PROGRESS");
    if (initial_progress != NULL) {
        errno = 0;
        char *progress_end = NULL;
        const double parsed_progress = strtod(initial_progress, &progress_end);
        if (errno != 0 || progress_end == initial_progress || *progress_end != '\0' ||
            !isfinite(parsed_progress) || parsed_progress < 0.0 || parsed_progress > 1.0) {
            (void)baca_app_free(&app, NULL);
            return 123;
        }
        app.saved_progress = parsed_progress;
    }
    app.external_opener = pdf_test_opener;
    app.external_opener_data = &descriptor;
    const int run_result = baca_app_run(&app, &error);
    BacaError free_error = {0};
    if (!baca_app_free(&app, &free_error)) {
        (void)fprintf(stderr, "PDF PTY save: %s\n", free_error.message);
        (void)close(descriptor);
        return 119;
    }
    (void)close(descriptor);
    return run_result;
}

static bool pdf_drain_fd(int descriptor, BacaString *output) {
    char buffer[8192] = {0};
    BacaError error = {0};
    for (;;) {
        const ssize_t length = read(descriptor, buffer, sizeof(buffer));
        if (length > 0) {
            if (!baca_string_append_n(output, buffer, (size_t)length, &error)) {
                return false;
            }
        } else if (length < 0 && errno == EINTR) {
            continue;
        } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)) {
            return true;
        } else {
            return length == 0;
        }
    }
}

static bool pdf_wait_for(int descriptor, BacaString *output, size_t start, const char *needle) {
    for (unsigned attempt = 0U; attempt < 750U; ++attempt) {
        struct pollfd poll_descriptor = {.fd = descriptor, .events = POLLIN};
        const int ready = poll(&poll_descriptor, 1U, 20);
        if (ready < 0 && errno != EINTR) {
            return false;
        }
        if (!pdf_drain_fd(descriptor, output)) {
            return false;
        }
        if (output->data != NULL && start <= output->length && strstr(output->data + start, needle) != NULL) {
            return true;
        }
    }
    return false;
}

static bool pdf_drain_until_idle(int descriptor, BacaString *output) {
    for (unsigned attempt = 0U; attempt < 50U; ++attempt) {
        struct pollfd poll_descriptor = {.fd = descriptor, .events = POLLIN};
        const int ready = poll(&poll_descriptor, 1U, 100);
        if (ready < 0 && errno != EINTR) {
            return false;
        }
        if (!pdf_drain_fd(descriptor, output)) {
            return false;
        }
        if (ready == 0) {
            return true;
        }
    }
    return false;
}

static bool pdf_send_mouse_click(int descriptor, int x, int y) {
    char sequence[96] = {0};
    const int length = snprintf(sequence, sizeof(sequence), "\033[<0;%d;%dM\033[<0;%d;%dm", x + 1, y + 1,
                                x + 1, y + 1);
    return length > 0 && (size_t)length < sizeof(sequence) &&
           pdf_write_all(descriptor, sequence, (size_t)length);
}

static void pdf_draw_text(cairo_t *context, double x, double y, const char *text) {
    cairo_select_font_face(context, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 18.0);
    cairo_move_to(context, x, y);
    cairo_show_text(context, text);
}

static bool pdf_zip_add(zip_t *archive, const char *name, const char *text, bool stored) {
    zip_source_t *source = zip_source_buffer(archive, text, strlen(text), 0);
    if (source == NULL) {
        return false;
    }
    const zip_int64_t index = zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
    if (index < 0) {
        zip_source_free(source);
        return false;
    }
    return !stored || zip_set_file_compression(archive, (zip_uint64_t)index, ZIP_CM_STORE, 0U) == 0;
}

static char *create_key_conflict_epub_fixture(const char *relative) {
    static const char mimetype[] = "application/epub+zip";
    static const char container[] =
        "<?xml version=\"1.0\"?>"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
        "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    static const char opf[] =
        "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" "
        "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>Key Conflict</dc:title><dc:identifier id=\"id\">key-conflict</dc:identifier>"
        "</metadata><manifest><item id=\"chapter\" href=\"chapter.xhtml\" "
        "media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"chapter\"/>"
        "</spine></package>";
    BacaString chapter = {0};
    BacaError error = {0};
    if (!baca_string_append(&chapter,
                            "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>NON_PDF_START</p>",
                            &error)) {
        return NULL;
    }
    for (int line = 0; line < 40; ++line) {
        if (!baca_string_append(&chapter, "<p>filler line</p>", &error)) {
            baca_string_free(&chapter);
            return NULL;
        }
    }
    if (!baca_string_append(&chapter, "<p>CONFLICT_TARGET</p></body></html>", &error)) {
        baca_string_free(&chapter);
        return NULL;
    }

    char *path = baca_test_path(relative);
    char *directory = path == NULL ? NULL : baca_path_dirname(path, &error);
    if (path == NULL || directory == NULL || !baca_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        baca_string_free(&chapter);
        return NULL;
    }
    free(directory);
    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    if (archive == NULL) {
        free(path);
        baca_string_free(&chapter);
        return NULL;
    }
    const bool added = pdf_zip_add(archive, "mimetype", mimetype, true) &&
                       pdf_zip_add(archive, "META-INF/container.xml", container, false) &&
                       pdf_zip_add(archive, "OEBPS/content.opf", opf, false) &&
                       pdf_zip_add(archive, "OEBPS/chapter.xhtml", chapter.data, false);
    if (!added) {
        zip_discard(archive);
        baca_string_free(&chapter);
        free(path);
        return NULL;
    }
    if (zip_close(archive) != 0) {
        zip_discard(archive);
        baca_string_free(&chapter);
        free(path);
        return NULL;
    }
    baca_string_free(&chapter);
    return path;
}

static char *create_raw_page_pdf_fixture(const char *relative, const char *width, const char *height) {
    BacaString pdf = {0};
    BacaError error = {0};
    size_t offsets[5] = {0};
    if (!baca_string_append(&pdf, "%PDF-1.7\n%TEST\n", &error)) {
        return NULL;
    }
    offsets[1] = pdf.length;
    if (!baca_string_append(&pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n", &error)) {
        goto fail;
    }
    offsets[2] = pdf.length;
    if (!baca_string_append(&pdf, "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n", &error)) {
        goto fail;
    }
    offsets[3] = pdf.length;
    char page[256] = {0};
    const int page_length = snprintf(page, sizeof(page),
                                     "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %s %s] "
                                     "/Resources << >> /Contents 4 0 R >>\nendobj\n",
                                     width, height);
    if (page_length <= 0 || (size_t)page_length >= sizeof(page) ||
        !baca_string_append_n(&pdf, page, (size_t)page_length, &error)) {
        goto fail;
    }
    offsets[4] = pdf.length;
    if (!baca_string_append(&pdf, "4 0 obj\n<< /Length 0 >>\nstream\n\nendstream\nendobj\n", &error)) {
        goto fail;
    }
    const size_t xref = pdf.length;
    char trailer[512] = {0};
    const int trailer_length = snprintf(
        trailer, sizeof(trailer),
        "xref\n0 5\n0000000000 65535 f \n%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
        "%010zu 00000 n \ntrailer\n<< /Size 5 /Root 1 0 R "
        "/ID [<0123456789abcdef0123456789abcdef><0123456789abcdef0123456789abcdef>] >>\n"
        "startxref\n%zu\n%%%%EOF\n",
        offsets[1], offsets[2], offsets[3], offsets[4], xref);
    if (trailer_length <= 0 || (size_t)trailer_length >= sizeof(trailer) ||
        !baca_string_append_n(&pdf, trailer, (size_t)trailer_length, &error) ||
        !baca_test_write(relative, pdf.data, pdf.length)) {
        goto fail;
    }
    baca_string_free(&pdf);
    return baca_test_path(relative);

fail:
    baca_string_free(&pdf);
    return NULL;
}

static char *create_tall_link_pdf_fixture(const char *relative) {
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return NULL;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return NULL;
    }
    free(directory);

    cairo_surface_t *surface = cairo_pdf_surface_create(path, 100.0, 8333.0);
    cairo_t *context = cairo_create(surface);
    cairo_tag_begin(context, CAIRO_TAG_LINK,
                    "rect=[0 0 100 500] uri='https://example.test/invisible-hotspot'");
    cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    cairo_rectangle(context, 0.0, 0.0, 100.0, 500.0);
    cairo_fill(context);
    cairo_tag_end(context, CAIRO_TAG_LINK);
    cairo_show_page(context);
    cairo_destroy(context);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        free(path);
        return NULL;
    }
    return path;
}

static char *create_pdf_actions_fixture(const char *relative) {
    static const char malformed_annotation[] =
        "<< /Type /Annot /Subtype /Link /Rect [10 10 110 30] "
        "/A << /S /URI /URI (https://valid.test/\377tail) >> >>";
    static const struct {
        const char *value;
        size_t length;
    } objects[] = {
        {NULL, 0U},
        {"<< /Type /Catalog /Pages 2 0 R /Outlines 7 0 R >>",
         sizeof("<< /Type /Catalog /Pages 2 0 R /Outlines 7 0 R >>") - 1U},
        {"<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>",
         sizeof("<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>") - 1U},
        {"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 1200] /Resources << >> "
         "/Contents 5 0 R /Annots [13 0 R] >>",
         sizeof("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 1200] /Resources << >> "
                "/Contents 5 0 R /Annots [13 0 R] >>") - 1U},
        {"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Rotate 90 "
         "/Resources << >> /Contents 6 0 R >>",
         sizeof("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Rotate 90 "
                "/Resources << >> /Contents 6 0 R >>") - 1U},
        {"<< /Length 0 >>\nstream\n\nendstream", sizeof("<< /Length 0 >>\nstream\n\nendstream") - 1U},
        {"<< /Length 0 >>\nstream\n\nendstream", sizeof("<< /Length 0 >>\nstream\n\nendstream") - 1U},
        {"<< /Type /Outlines /First 8 0 R /Last 12 0 R /Count 5 >>",
         sizeof("<< /Type /Outlines /First 8 0 R /Last 12 0 R /Count 5 >>") - 1U},
        {"<< /Title (XYZ) /Parent 7 0 R /Next 9 0 R /Dest [3 0 R /XYZ 0 960 null] >>",
         sizeof("<< /Title (XYZ) /Parent 7 0 R /Next 9 0 R /Dest [3 0 R /XYZ 0 960 null] >>") - 1U},
        {"<< /Title (FitH) /Parent 7 0 R /Prev 8 0 R /Next 10 0 R /Dest [3 0 R /FitH 900] >>",
         sizeof("<< /Title (FitH) /Parent 7 0 R /Prev 8 0 R /Next 10 0 R /Dest [3 0 R /FitH 900] >>") - 1U},
        {"<< /Title (FitBH) /Parent 7 0 R /Prev 9 0 R /Next 11 0 R /Dest [3 0 R /FitBH 600] >>",
         sizeof("<< /Title (FitBH) /Parent 7 0 R /Prev 9 0 R /Next 11 0 R /Dest [3 0 R /FitBH 600] >>") - 1U},
        {"<< /Title (FitR) /Parent 7 0 R /Prev 10 0 R /Next 12 0 R "
         "/Dest [3 0 R /FitR 0 240 100 480] >>",
         sizeof("<< /Title (FitR) /Parent 7 0 R /Prev 10 0 R /Next 12 0 R "
                "/Dest [3 0 R /FitR 0 240 100 480] >>") - 1U},
        {"<< /Title (Rotated) /Parent 7 0 R /Prev 11 0 R /Dest [4 0 R /XYZ 0 50 null] >>",
         sizeof("<< /Title (Rotated) /Parent 7 0 R /Prev 11 0 R /Dest [4 0 R /XYZ 0 50 null] >>") - 1U},
        {malformed_annotation, sizeof(malformed_annotation) - 1U},
    };

    BacaString pdf = {0};
    BacaError error = {0};
    size_t offsets[BACA_ARRAY_LEN(objects)] = {0};
    if (!baca_string_append(&pdf, "%PDF-1.7\n%TEST\n", &error)) {
        return NULL;
    }
    for (size_t object = 1U; object < BACA_ARRAY_LEN(objects); ++object) {
        offsets[object] = pdf.length;
        char header[32] = {0};
        const int header_length = snprintf(header, sizeof(header), "%zu 0 obj\n", object);
        if (header_length <= 0 || (size_t)header_length >= sizeof(header) ||
            !baca_string_append_n(&pdf, header, (size_t)header_length, &error) ||
            !baca_string_append_n(&pdf, objects[object].value, objects[object].length, &error) ||
            !baca_string_append(&pdf, "\nendobj\n", &error)) {
            baca_string_free(&pdf);
            return NULL;
        }
    }

    const size_t xref = pdf.length;
    if (!baca_string_append(&pdf, "xref\n0 14\n0000000000 65535 f \n", &error)) {
        baca_string_free(&pdf);
        return NULL;
    }
    for (size_t object = 1U; object < BACA_ARRAY_LEN(objects); ++object) {
        char entry[32] = {0};
        const int entry_length = snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offsets[object]);
        if (entry_length <= 0 || (size_t)entry_length >= sizeof(entry) ||
            !baca_string_append_n(&pdf, entry, (size_t)entry_length, &error)) {
            baca_string_free(&pdf);
            return NULL;
        }
    }
    char trailer[256] = {0};
    const int trailer_length = snprintf(
        trailer, sizeof(trailer),
        "trailer\n<< /Size 14 /Root 1 0 R "
        "/ID [<0123456789abcdef0123456789abcdef><0123456789abcdef0123456789abcdef>] >>\n"
        "startxref\n%zu\n%%%%EOF\n",
        xref);
    const bool written = trailer_length > 0 && (size_t)trailer_length < sizeof(trailer) &&
                         baca_string_append_n(&pdf, trailer, (size_t)trailer_length, &error) &&
                         baca_test_write(relative, pdf.data, pdf.length);
    baca_string_free(&pdf);
    return written ? baca_test_path(relative) : NULL;
}

static char *create_pdf_fixture(const char *relative, bool huge_page, bool varied_pages) {
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return NULL;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return NULL;
    }
    free(directory);

    const double first_width = huge_page ? 1000001.0 : 612.0;
    const double first_height = huge_page ? 20.0 : 792.0;
    cairo_surface_t *surface = cairo_pdf_surface_create(path, first_width, first_height);
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_TITLE, "Baca PDF Fixture");
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_AUTHOR, "Fixture Author");
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_SUBJECT, "Fixture subject");
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_CREATOR, "Baca Cairo Tests");
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_CREATE_DATE, "2026-07-19T12:00:00Z");
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_MOD_DATE, "2026-07-19T13:00:00Z");
    cairo_t *context = cairo_create(surface);
    cairo_set_source_rgb(context, 0.1, 0.1, 0.1);
    if (huge_page) {
        pdf_draw_text(context, 10.0, 15.0, "huge page");
        cairo_show_page(context);
    } else {
        pdf_draw_text(context, 50.0, 45.0, "First page alpha Unicode: caf\xc3\xa9 \xce\xa9");
        cairo_tag_begin(context, CAIRO_TAG_LINK,
                        "rect=[50 70 220 24] uri='https://example.test/pdf-link'");
        pdf_draw_text(context, 50.0, 88.0, "External PDF link");
        cairo_tag_end(context, CAIRO_TAG_LINK);
        cairo_tag_begin(context, CAIRO_TAG_LINK, "rect=[50 110 220 24] page=2");
        pdf_draw_text(context, 50.0, 128.0, "Go to landscape page");
        cairo_tag_end(context, CAIRO_TAG_LINK);
        cairo_show_page(context);

        if (varied_pages) {
            cairo_pdf_surface_set_size(surface, 792.0, 612.0);
        }
        pdf_draw_text(context, 60.0, 70.0, "Second page needle landscape");
        cairo_show_page(context);

        if (varied_pages) {
            cairo_pdf_surface_set_size(surface, 400.0, 400.0);
        }
        cairo_set_source_rgb(context, 0.2, 0.4, 0.8);
        cairo_rectangle(context, 40.0, 40.0, 320.0, 320.0);
        cairo_fill(context);
        cairo_show_page(context);

        const int chapter = cairo_pdf_surface_add_outline(
            surface, CAIRO_PDF_OUTLINE_ROOT, "First chapter", "page=1",
            CAIRO_PDF_OUTLINE_FLAG_OPEN);
        if (chapter > 0) {
            (void)cairo_pdf_surface_add_outline(surface, chapter, "Landscape child", "page=2", 0);
        }
        (void)cairo_pdf_surface_add_outline(surface, CAIRO_PDF_OUTLINE_ROOT, "External reference",
                                            "uri='https://example.test/pdf-outline'", 0);
    }
    cairo_destroy(context);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        free(path);
        return NULL;
    }
    return path;
}

static char *create_many_page_pdf_fixture(const char *relative) {
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return NULL;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return NULL;
    }
    free(directory);

    cairo_surface_t *surface = cairo_pdf_surface_create(path, 20.0, 20.0);
    cairo_t *context = cairo_create(surface);
    for (int page = 0; page < 10001; ++page) {
        cairo_show_page(context);
    }
    cairo_destroy(context);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        free(path);
        return NULL;
    }
    return path;
}

static char *create_many_outline_pdf_fixture(const char *relative) {
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return NULL;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return NULL;
    }
    free(directory);

    cairo_surface_t *surface = cairo_pdf_surface_create(path, 20.0, 20.0);
    cairo_t *context = cairo_create(surface);
    cairo_show_page(context);
    for (int node = 0; node <= PDF_OUTLINE_NODE_LIMIT; ++node) {
        if (cairo_pdf_surface_add_outline(surface, CAIRO_PDF_OUTLINE_ROOT, "", "page=1", 0) <= 0) {
            cairo_destroy(context);
            cairo_surface_destroy(surface);
            free(path);
            return NULL;
        }
    }
    cairo_destroy(context);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        free(path);
        return NULL;
    }
    return path;
}

static char *create_link_budget_pdf_fixture(const char *relative) {
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return NULL;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return NULL;
    }
    free(directory);

    static const char prefix[] = "https://budget.test/";
    char *target = baca_reallocarray(NULL, PDF_STRESS_TARGET_BYTES, sizeof(*target), &error);
    char *attributes = baca_reallocarray(NULL, PDF_STRESS_TARGET_BYTES + 7U, sizeof(*attributes), &error);
    if (target == NULL || attributes == NULL) {
        free(attributes);
        free(target);
        free(path);
        return NULL;
    }
    memcpy(target, prefix, sizeof(prefix) - 1U);
    memset(target + sizeof(prefix) - 1U, 'a', PDF_STRESS_TARGET_BYTES - sizeof(prefix));
    target[PDF_STRESS_TARGET_BYTES - 1U] = '\0';
    const int attributes_length = snprintf(attributes, PDF_STRESS_TARGET_BYTES + 7U, "uri='%s'", target);
    free(target);
    if (attributes_length <= 0 || (size_t)attributes_length >= PDF_STRESS_TARGET_BYTES + 7U) {
        free(attributes);
        free(path);
        return NULL;
    }

    cairo_surface_t *surface = cairo_pdf_surface_create(path, 20.0, 20.0);
    cairo_t *context = cairo_create(surface);
    cairo_show_page(context);
    bool added = true;
    for (int node = 0; node < PDF_STRESS_TARGET_COUNT; ++node) {
        if (cairo_pdf_surface_add_outline(surface, CAIRO_PDF_OUTLINE_ROOT, "Budget target", attributes, 0) <= 0) {
            added = false;
            break;
        }
    }
    free(attributes);
    cairo_destroy(context);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (!added || status != CAIRO_STATUS_SUCCESS) {
        free(path);
        return NULL;
    }
    return path;
}

static char *create_tall_destination_pdf_fixture(const char *relative) {
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return NULL;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return NULL;
    }
    free(directory);

    cairo_surface_t *surface = cairo_pdf_surface_create(path, 300.0, 1200.0);
    cairo_t *context = cairo_create(surface);
    cairo_set_source_rgb(context, 0.1, 0.1, 0.1);
    for (int line = 0; line < 40; ++line) {
        char text[96] = {0};
        if (line == 9) {
            (void)snprintf(text, sizeof(text), "UPPER_CURRENT_MATCH");
        } else if (line == 30) {
            (void)snprintf(text, sizeof(text), "LOWER_CURRENT_MATCH");
        } else {
            (void)snprintf(text, sizeof(text), "search filler line %02d", line);
        }
        pdf_draw_text(context, 20.0, 25.0 + (double)line * 28.0, text);
    }
    cairo_show_page(context);
    const int upper = cairo_pdf_surface_add_outline(surface, CAIRO_PDF_OUTLINE_ROOT, "Upper destination",
                                                    "page=1 pos=[0 240]", 0);
    const int lower = cairo_pdf_surface_add_outline(surface, CAIRO_PDF_OUTLINE_ROOT, "Lower destination",
                                                    "page=1 pos=[0 960]", 0);
    cairo_destroy(context);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (upper <= 0 || lower <= 0 || status != CAIRO_STATUS_SUCCESS) {
        free(path);
        return NULL;
    }
    return path;
}

static bool run_pdf_pty(PdfPtyResult *result) {
    char *path = create_pdf_fixture("pdf/pty/reader.pdf", false, false);
    if (path == NULL ||
        !baca_test_write_text("pdf/pty/config/baca/config.ini",
                              "[General]\nImageMode=kitty\nMaxTextWidth=80\nPageScrollDuration=0\n"
                              "[Keymaps]\nTogglePdfView=ctrl+x\nOpenHelp=z\n")) {
        free(path);
        return false;
    }
    char *config_root = baca_test_path("pdf/pty/config");
    if (config_root == NULL) {
        free(path);
        return false;
    }
    int opener_pipe[2] = {-1, -1};
    if (pipe(opener_pipe) != 0) {
        free(config_root);
        free(path);
        return false;
    }

    struct winsize size = {
        .ws_row = 12U,
        .ws_col = 40U,
        .ws_xpixel = 400U,
        .ws_ypixel = 240U,
    };
    int master = -1;
    (void)fflush(NULL);
    const pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child < 0) {
        (void)close(opener_pipe[0]);
        (void)close(opener_pipe[1]);
        free(config_root);
        free(path);
        return false;
    }
    if (child == 0) {
        (void)close(opener_pipe[0]);
        (void)setenv("TERM", "xterm-256color", 1);
        (void)setenv("COLORTERM", "truecolor", 1);
        (void)setenv("TERM_PROGRAM", "kitty", 1);
        (void)setenv("KITTY_WINDOW_ID", "1", 1);
        (void)unsetenv("TMUX");
        (void)unsetenv("STY");
        (void)setenv("XDG_CONFIG_HOME", config_root, 1);
        char descriptor[32] = {0};
        (void)snprintf(descriptor, sizeof(descriptor), "%d", opener_pipe[1]);
        (void)setenv("BACA_PDF_PTY_CHILD", "1", 1);
        (void)setenv("BACA_PDF_PTY_PATH", path, 1);
        (void)setenv("BACA_PDF_PTY_OPEN_FD", descriptor, 1);
        (void)execl("./build/tests/test_baca", "test_baca", (char *)NULL);
        _exit(127);
    }

    (void)close(opener_pipe[1]);
    bool ok = fcntl(master, F_SETFL, O_NONBLOCK) == 0 &&
              fcntl(opener_pipe[0], F_SETFL, O_NONBLOCK) == 0;
    if (!ok) {
        goto cleanup;
    }

    result->fixed_rendered = pdf_wait_for(master, &result->output, 0U, "a=t,f=100");
    size_t start = result->output.length;
    ok = result->fixed_rendered && pdf_write_all(master, "z", 1U) &&
          pdf_wait_for(master, &result->output, start, "Help");
    result->help_opened = ok;
    start = result->output.length;
    ok = ok && pdf_write_all(master, "jjj", 3U) &&
          pdf_wait_for(master, &result->output, start, "ctrl+x");
    result->help_shows_configured_toggle =
        ok && strstr(result->output.data + start, "ctrl+x") != NULL;
    start = result->output.length;
    ok = result->help_shows_configured_toggle && pdf_write_all(master, "q", 1U) &&
         pdf_write_all(master, "\030", 1U);
    result->toggled_to_text = ok && pdf_wait_for(master, &result->output, start, "First page alpha");

    start = result->output.length;
    ok = result->toggled_to_text && pdf_write_all(master, "\030", 1U) &&
         pdf_wait_for(master, &result->output, start, "a=p,");
    start = result->output.length;
    ok = ok && pdf_write_all(master, "/needle\n", 8U) &&
         pdf_wait_for(master, &result->output, start, "a=t,f=100");
    start = result->output.length;
    ok = ok && pdf_write_all(master, "\030", 1U);
    result->search_reached_page = ok && pdf_wait_for(master, &result->output, start, "Second page");

    start = result->output.length;
    ok = result->search_reached_page && pdf_write_all(master, "\n", 1U) &&
         pdf_write_all(master, "\030", 1U) &&
         pdf_wait_for(master, &result->output, start, "c=36,");
    struct winsize resized = {
        .ws_row = 14U,
        .ws_col = 50U,
        .ws_xpixel = 500U,
        .ws_ypixel = 280U,
    };
    start = result->output.length;
    ok = ok && ioctl(master, TIOCSWINSZ, &resized) == 0 &&
         pdf_wait_for(master, &result->output, start, "c=46,");
    start = result->output.length;
    ok = ok && pdf_write_all(master, "\030", 1U);
    result->resize_preserved_page = ok && pdf_wait_for(master, &result->output, start, "Second page");

    ok = result->resize_preserved_page && pdf_write_all(master, "\030", 1U);
    start = result->output.length;
    ok = ok && ioctl(master, TIOCSWINSZ, &size) == 0 &&
         pdf_wait_for(master, &result->output, start, "c=36,") &&
         pdf_write_all(master, "gg", 2U);
    start = result->output.length;
    ok = ok && pdf_wait_for(master, &result->output, start, "a=p,");

    const size_t internal_opened = result->opened.length;
    start = result->output.length;
    ok = ok && pdf_send_mouse_click(master, 6, 3) &&
         pdf_wait_for(master, &result->output, start, "a=p,") &&
         pdf_drain_until_idle(master, &result->output) &&
         pdf_drain_until_idle(opener_pipe[0], &result->opened) &&
         result->opened.length == internal_opened;
    start = result->output.length;
    ok = ok && pdf_write_all(master, "\030", 1U);
    result->internal_link_reached_page =
        ok && pdf_wait_for(master, &result->output, start, "Second page");

    start = result->output.length;
    ok = result->internal_link_reached_page && pdf_write_all(master, "\030", 1U) &&
         pdf_wait_for(master, &result->output, start, "c=36,");
    start = result->output.length;
    ok = ok && pdf_write_all(master, "gg", 2U) &&
         pdf_wait_for(master, &result->output, start, "a=p,") &&
         pdf_drain_until_idle(master, &result->output);
    const size_t external_output = result->output.length;
    start = result->opened.length;
    ok = ok && pdf_send_mouse_click(master, 6, 2);
    result->external_link_opened =
        ok && pdf_wait_for(opener_pipe[0], &result->opened, start,
                           "https://example.test/pdf-link") &&
        pdf_wait_for(master, &result->output, external_output, "a=p,") &&
        pdf_drain_until_idle(master, &result->output);

    const size_t internal_toc_opened = result->opened.length;
    start = result->output.length;
    ok = result->external_link_opened && pdf_write_all(master, "\tj\n", 3U) &&
         pdf_wait_for(master, &result->output, start, "a=p,");
    start = result->output.length;
    ok = ok && pdf_write_all(master, "\030", 1U);
    result->internal_toc_reached_page =
        ok && pdf_wait_for(master, &result->output, start, "Second page") &&
        pdf_drain_until_idle(opener_pipe[0], &result->opened) &&
        result->opened.length == internal_toc_opened;

    start = result->output.length;
    ok = result->internal_toc_reached_page && pdf_write_all(master, "\030", 1U) &&
         pdf_wait_for(master, &result->output, start, "a=p,") &&
         pdf_write_all(master, "\tG\n", 3U);
    start = result->opened.length;
    result->external_toc_opened =
        ok && pdf_wait_for(opener_pipe[0], &result->opened, start,
                           "https://example.test/pdf-outline");

    start = result->output.length;
    ok = result->external_toc_opened && pdf_write_all(master, "gg\030", 3U) &&
         pdf_wait_for(master, &result->output, start, "First page alpha");
    start = result->output.length;
    ok = ok && pdf_write_all(master, "\030", 1U) &&
         pdf_wait_for(master, &result->output, start, "a=p,");
    start = result->opened.length;
    ok = ok && pdf_send_mouse_click(master, 30, 10);
    result->page_opened = ok && pdf_wait_for(opener_pipe[0], &result->opened, start, path);

    ok = result->page_opened && kill(child, SIGTERM) == 0;
    for (unsigned attempt = 0U; attempt < 750U && ok; ++attempt) {
        (void)pdf_drain_fd(master, &result->output);
        const pid_t waited = waitpid(child, &result->status, WNOHANG);
        if (waited == child) {
            result->completed = true;
            break;
        }
        if (waited < 0) {
            ok = false;
            break;
        }
        struct timespec pause = {.tv_nsec = 20000000L};
        (void)nanosleep(&pause, NULL);
    }

cleanup:
    if (!result->completed) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &result->status, 0);
    }
    (void)pdf_drain_fd(master, &result->output);
    (void)pdf_drain_fd(opener_pipe[0], &result->opened);
    (void)close(master);
    (void)close(opener_pipe[0]);
    free(config_root);
    free(path);
    return ok;
}

static bool pdf_saved_progress(const char *cache_root, const char *path, double *progress) {
    BacaError error = {0};
    char *database_path = baca_path_join(cache_root, "baca/baca.db", &error);
    if (database_path == NULL) {
        return false;
    }
    BacaDatabase database = {0};
    BacaHistory history = {0};
    bool found = false;
    if (baca_database_open(&database, database_path, &error) &&
        baca_database_migrate(&database, &error) &&
        baca_database_history(&database, false, &history, &error)) {
        for (size_t index = 0U; index < history.length; ++index) {
            if (history.items[index].filepath != NULL && strcmp(history.items[index].filepath, path) == 0) {
                *progress = history.items[index].reading_progress;
                found = true;
                break;
            }
        }
    }
    baca_history_free(&history);
    baca_database_close(&database);
    free(database_path);
    return found;
}

static bool run_pdf_progress_pty(const char *name, const char *path, const struct winsize *size,
                                 bool restored_complete, bool press_end, double *progress,
                                 BacaString *output) {
    char config_relative[192] = {0};
    const int config_length = snprintf(config_relative, sizeof(config_relative),
                                       "pdf/progress/%s/config/baca/config.ini", name);
    if (config_length <= 0 || (size_t)config_length >= sizeof(config_relative) ||
        !baca_test_write_text(config_relative,
                              "[General]\nImageMode=kitty\nMaxTextWidth=80\nPageScrollDuration=0\n")) {
        return false;
    }
    char root_relative[160] = {0};
    const int root_length = snprintf(root_relative, sizeof(root_relative), "pdf/progress/%s", name);
    if (root_length <= 0 || (size_t)root_length >= sizeof(root_relative)) {
        return false;
    }
    char config_root_relative[192] = {0};
    char cache_root_relative[192] = {0};
    const int config_root_length = snprintf(config_root_relative, sizeof(config_root_relative), "%s/config",
                                            root_relative);
    const int cache_root_length = snprintf(cache_root_relative, sizeof(cache_root_relative), "%s/cache",
                                           root_relative);
    if (config_root_length <= 0 || (size_t)config_root_length >= sizeof(config_root_relative) ||
        cache_root_length <= 0 || (size_t)cache_root_length >= sizeof(cache_root_relative) ||
        !baca_test_mkdir(cache_root_relative)) {
        return false;
    }
    char *config_root = baca_test_path(config_root_relative);
    char *cache_root = baca_test_path(cache_root_relative);
    if (config_root == NULL || cache_root == NULL) {
        free(cache_root);
        free(config_root);
        return false;
    }

    int opener_pipe[2] = {-1, -1};
    if (pipe(opener_pipe) != 0) {
        free(cache_root);
        free(config_root);
        return false;
    }
    int master = -1;
    (void)fflush(NULL);
    const pid_t child = forkpty(&master, NULL, NULL, size);
    if (child < 0) {
        (void)close(opener_pipe[0]);
        (void)close(opener_pipe[1]);
        free(cache_root);
        free(config_root);
        return false;
    }
    if (child == 0) {
        (void)close(opener_pipe[0]);
        (void)setenv("TERM", "xterm-256color", 1);
        (void)setenv("COLORTERM", "truecolor", 1);
        (void)setenv("TERM_PROGRAM", "kitty", 1);
        (void)setenv("KITTY_WINDOW_ID", "1", 1);
        (void)unsetenv("TMUX");
        (void)unsetenv("STY");
        (void)setenv("XDG_CONFIG_HOME", config_root, 1);
        (void)setenv("XDG_CACHE_HOME", cache_root, 1);
        if (restored_complete) {
            (void)setenv("BACA_PDF_PTY_INITIAL_PROGRESS", "1", 1);
        } else {
            (void)unsetenv("BACA_PDF_PTY_INITIAL_PROGRESS");
        }
        char descriptor[32] = {0};
        (void)snprintf(descriptor, sizeof(descriptor), "%d", opener_pipe[1]);
        (void)setenv("BACA_PDF_PTY_CHILD", "1", 1);
        (void)setenv("BACA_PDF_PTY_PATH", path, 1);
        (void)setenv("BACA_PDF_PTY_OPEN_FD", descriptor, 1);
        (void)execl("./build/tests/test_baca", "test_baca", (char *)NULL);
        _exit(127);
    }

    (void)close(opener_pipe[1]);
    bool ok = fcntl(master, F_SETFL, O_NONBLOCK) == 0 &&
              pdf_wait_for(master, output, 0U, "a=t,f=100");
    if (ok && press_end) {
        const size_t start = output->length;
        ok = pdf_write_all(master, "G", 1U) && pdf_wait_for(master, output, start, "a=p,");
    }
    ok = ok && pdf_write_all(master, "q", 1U);
    int status = 0;
    bool completed = false;
    for (unsigned attempt = 0U; attempt < 750U && ok; ++attempt) {
        (void)pdf_drain_fd(master, output);
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            completed = true;
            break;
        }
        if (waited < 0) {
            ok = false;
            break;
        }
        struct timespec pause = {.tv_nsec = 20000000L};
        (void)nanosleep(&pause, NULL);
    }
    if (!completed) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
    }
    (void)pdf_drain_fd(master, output);
    (void)close(master);
    (void)close(opener_pipe[0]);
    ok = ok && completed && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
         pdf_saved_progress(cache_root, path, progress);
    free(cache_root);
    free(config_root);
    return ok;
}

static bool run_non_pdf_key_conflict_pty(BacaString *output) {
    char *path = create_key_conflict_epub_fixture("pdf/key-conflict/reader.epub");
    if (path == NULL ||
        !baca_test_write_text("pdf/key-conflict/config/baca/config.ini",
                              "[General]\nImageMode=placeholder\nMaxTextWidth=80\nPageScrollDuration=0\n"
                              "[Keymaps]\nConfirm=v\nTogglePdfView=v\n") ||
        !baca_test_mkdir("pdf/key-conflict/cache")) {
        free(path);
        return false;
    }
    char *config_root = baca_test_path("pdf/key-conflict/config");
    char *cache_root = baca_test_path("pdf/key-conflict/cache");
    if (config_root == NULL || cache_root == NULL) {
        free(cache_root);
        free(config_root);
        free(path);
        return false;
    }
    const struct winsize size = {
        .ws_row = 12U,
        .ws_col = 40U,
        .ws_xpixel = 400U,
        .ws_ypixel = 240U,
    };
    int master = -1;
    (void)fflush(NULL);
    const pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child < 0) {
        free(cache_root);
        free(config_root);
        free(path);
        return false;
    }
    if (child == 0) {
        (void)setenv("TERM", "xterm-256color", 1);
        (void)setenv("XDG_CONFIG_HOME", config_root, 1);
        (void)setenv("XDG_CACHE_HOME", cache_root, 1);
        (void)execl("./build/baca", "baca", path, (char *)NULL);
        _exit(127);
    }

    bool ok = fcntl(master, F_SETFL, O_NONBLOCK) == 0 &&
              pdf_wait_for(master, output, 0U, "NON_PDF_START");
    const size_t start = output->length;
    static const char search[] = "/CONFLICT_TARGET";
    ok = ok && pdf_write_all(master, search, sizeof(search) - 1U) &&
         pdf_write_all(master, "v", 1U) &&
         pdf_wait_for(master, output, start, "CONFLICT_TARGET") &&
         pdf_write_all(master, "qq", 2U);
    int status = 0;
    bool completed = false;
    for (unsigned attempt = 0U; attempt < 750U && ok; ++attempt) {
        (void)pdf_drain_fd(master, output);
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            completed = true;
            break;
        }
        if (waited < 0) {
            ok = false;
            break;
        }
        struct timespec pause = {.tv_nsec = 20000000L};
        (void)nanosleep(&pause, NULL);
    }
    if (!completed) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
    }
    (void)pdf_drain_fd(master, output);
    (void)close(master);
    free(cache_root);
    free(config_root);
    free(path);
    return ok && completed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool pdf_wait_for_child_exit(pid_t child, int master, BacaString *output, int *status) {
    for (unsigned attempt = 0U; attempt < 750U; ++attempt) {
        (void)pdf_drain_fd(master, output);
        const pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child) {
            return true;
        }
        if (waited < 0) {
            return false;
        }
        const struct timespec pause = {.tv_nsec = 20000000L};
        (void)nanosleep(&pause, NULL);
    }
    return false;
}

static bool run_pdf_current_page_search_pty(BacaString *output, unsigned *stage) {
    *stage = 0U;
    char *path = create_tall_destination_pdf_fixture("pdf/search-pty/reader.pdf");
    if (path == NULL ||
        !baca_test_write_text("pdf/search-pty/config/baca/config.ini",
                               "[General]\nImageMode=kitty\nMaxTextWidth=80\nPageScrollDuration=0\n"
                               "[Keymaps]\nTogglePdfView=ctrl+x\nSearchBackward=u\n") ||
        !baca_test_mkdir("pdf/search-pty/cache")) {
        free(path);
        return false;
    }
    char *config_root = baca_test_path("pdf/search-pty/config");
    char *cache_root = baca_test_path("pdf/search-pty/cache");
    if (config_root == NULL || cache_root == NULL) {
        free(cache_root);
        free(config_root);
        free(path);
        return false;
    }

    int opener_pipe[2] = {-1, -1};
    if (pipe(opener_pipe) != 0) {
        free(cache_root);
        free(config_root);
        free(path);
        return false;
    }
    const struct winsize size = {
        .ws_row = 12U,
        .ws_col = 40U,
        .ws_xpixel = 400U,
        .ws_ypixel = 240U,
    };
    int master = -1;
    (void)fflush(NULL);
    const pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child < 0) {
        (void)close(opener_pipe[0]);
        (void)close(opener_pipe[1]);
        free(cache_root);
        free(config_root);
        free(path);
        return false;
    }
    if (child == 0) {
        (void)close(opener_pipe[0]);
        (void)setenv("TERM", "xterm-256color", 1);
        (void)setenv("COLORTERM", "truecolor", 1);
        (void)setenv("TERM_PROGRAM", "kitty", 1);
        (void)setenv("KITTY_WINDOW_ID", "1", 1);
        (void)unsetenv("TMUX");
        (void)unsetenv("STY");
        (void)setenv("XDG_CONFIG_HOME", config_root, 1);
        (void)setenv("XDG_CACHE_HOME", cache_root, 1);
        (void)setenv("BACA_PDF_PTY_INITIAL_PROGRESS", "0.5", 1);
        char descriptor[32] = {0};
        (void)snprintf(descriptor, sizeof(descriptor), "%d", opener_pipe[1]);
        (void)setenv("BACA_PDF_PTY_CHILD", "1", 1);
        (void)setenv("BACA_PDF_PTY_PATH", path, 1);
        (void)setenv("BACA_PDF_PTY_OPEN_FD", descriptor, 1);
        (void)execl("./build/tests/test_baca", "test_baca", (char *)NULL);
        _exit(127);
    }

    (void)close(opener_pipe[1]);
    bool ok = fcntl(master, F_SETFL, O_NONBLOCK) == 0 &&
              pdf_wait_for(master, output, 0U, "a=t,f=100");
    if (ok) {
        *stage = 1U;
    }
    size_t start = output->length;
    static const char forward[] = "/CURRENT_MATCH\n\030";
    ok = ok && pdf_write_all(master, forward, sizeof(forward) - 1U) &&
         pdf_wait_for(master, output, start, "search filler line 31");
    if (ok) {
        *stage = 2U;
    }
    start = output->length;
    ok = ok && pdf_write_all(master, "q\030", 2U) &&
         pdf_wait_for(master, output, start, "a=p,");
    if (ok) {
        *stage = 3U;
    }
    start = output->length;
    static const char backward[] = "uCURRENT_MATCH\n\030";
    ok = ok && pdf_write_all(master, backward, sizeof(backward) - 1U) &&
         pdf_wait_for(master, output, start, "search filler line 10") &&
         pdf_write_all(master, "qq", 2U);
    if (ok) {
        *stage = 4U;
    }

    int status = 0;
    const bool completed = ok && pdf_wait_for_child_exit(child, master, output, &status);
    if (completed) {
        *stage = 5U;
    }
    if (!completed) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
    }
    (void)pdf_drain_fd(master, output);
    (void)close(master);
    (void)close(opener_pipe[0]);
    free(cache_root);
    free(config_root);
    free(path);
    return ok && completed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool run_pdf_resize_retry_pty(BacaString *output, unsigned *stage) {
    *stage = 0U;
    char *path = create_tall_link_pdf_fixture("pdf/resize-retry/reader.pdf");
    if (path == NULL ||
        !baca_test_write_text("pdf/resize-retry/config/baca/config.ini",
                              "[General]\nImageMode=kitty\nMaxTextWidth=80\nPageScrollDuration=0\n") ||
        !baca_test_mkdir("pdf/resize-retry/cache")) {
        free(path);
        return false;
    }
    char *config_root = baca_test_path("pdf/resize-retry/config");
    char *cache_root = baca_test_path("pdf/resize-retry/cache");
    if (config_root == NULL || cache_root == NULL) {
        free(cache_root);
        free(config_root);
        free(path);
        return false;
    }

    int opener_pipe[2] = {-1, -1};
    if (pipe(opener_pipe) != 0) {
        free(cache_root);
        free(config_root);
        free(path);
        return false;
    }
    const struct winsize oversized = {
        .ws_row = 12U,
        .ws_col = 40U,
        .ws_xpixel = 400U,
        .ws_ypixel = 360U,
    };
    int master = -1;
    (void)fflush(NULL);
    const pid_t child = forkpty(&master, NULL, NULL, &oversized);
    if (child < 0) {
        (void)close(opener_pipe[0]);
        (void)close(opener_pipe[1]);
        free(cache_root);
        free(config_root);
        free(path);
        return false;
    }
    if (child == 0) {
        (void)close(opener_pipe[0]);
        (void)setenv("TERM", "xterm-256color", 1);
        (void)setenv("COLORTERM", "truecolor", 1);
        (void)setenv("TERM_PROGRAM", "kitty", 1);
        (void)setenv("KITTY_WINDOW_ID", "1", 1);
        (void)unsetenv("TMUX");
        (void)unsetenv("STY");
        (void)setenv("XDG_CONFIG_HOME", config_root, 1);
        (void)setenv("XDG_CACHE_HOME", cache_root, 1);
        (void)unsetenv("BACA_PDF_PTY_INITIAL_PROGRESS");
        char descriptor[32] = {0};
        (void)snprintf(descriptor, sizeof(descriptor), "%d", opener_pipe[1]);
        (void)setenv("BACA_PDF_PTY_CHILD", "1", 1);
        (void)setenv("BACA_PDF_PTY_PATH", path, 1);
        (void)setenv("BACA_PDF_PTY_OPEN_FD", descriptor, 1);
        (void)execl("./build/tests/test_baca", "test_baca", (char *)NULL);
        _exit(127);
    }

    (void)close(opener_pipe[1]);
    BacaString opened = {0};
    bool ok = fcntl(master, F_SETFL, O_NONBLOCK) == 0 &&
              fcntl(opener_pipe[0], F_SETFL, O_NONBLOCK) == 0 &&
              pdf_wait_for(master, output, 0U, "target image exceeds");
    if (ok) {
        *stage = 1U;
    }
    ok = ok && pdf_write_all(master, "q", 1U) &&
         output->data != NULL && strstr(output->data, "IMAGE") != NULL &&
         pdf_send_mouse_click(master, 19, 5);
    if (ok) {
        *stage = 2U;
    }
    ok = ok && pdf_wait_for(opener_pipe[0], &opened, 0U, "\n");
    if (ok) {
        *stage = 3U;
    }
    ok = ok && strstr(opened.data, path) != NULL &&
         strstr(opened.data, "https://example.test/invisible-hotspot") == NULL;
    if (ok) {
        *stage = 4U;
    }
    const struct winsize smaller = {
        .ws_row = 12U,
        .ws_col = 20U,
        .ws_xpixel = 200U,
        .ws_ypixel = 240U,
    };
    size_t start = output->length;
    ok = ok && ioctl(master, TIOCSWINSZ, &smaller) == 0 &&
         pdf_wait_for(master, output, start, "a=t,f=100") &&
         pdf_wait_for(master, output, start, "a=p,");
    if (ok) {
        *stage = 5U;
    }
    start = output->length;
    ok = ok && ioctl(master, TIOCSWINSZ, &oversized) == 0 &&
         pdf_wait_for(master, output, start, "target image exceeds");
    if (ok) {
        *stage = 6U;
    }
    start = output->length;
    ok = ok && pdf_write_all(master, "q", 1U) &&
         pdf_drain_until_idle(master, output) &&
         (output->data == NULL || strstr(output->data + start, "target image exceeds") == NULL) &&
         pdf_write_all(master, "q", 1U);

    int status = 0;
    const bool completed = ok && pdf_wait_for_child_exit(child, master, output, &status);
    if (completed) {
        *stage = 7U;
    }
    if (!completed) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
    }
    (void)pdf_drain_fd(master, output);
    (void)close(master);
    (void)close(opener_pipe[0]);
    baca_string_free(&opened);
    free(cache_root);
    free(config_root);
    free(path);
    return ok && completed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static const BacaImageLink *find_image_link(const BacaImageBlock *image, const char *target) {
    for (size_t index = 0U; index < image->link_count; ++index) {
        if (image->links[index].target != NULL && strcmp(image->links[index].target, target) == 0) {
            return &image->links[index];
        }
    }
    return NULL;
}

static bool pdf_target_y(const char *target, double *y) {
    const char *fragment = strstr(target, "#y=");
    if (fragment == NULL) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const double parsed = strtod(fragment + 3U, &end);
    if (errno != 0 || end == fragment + 3U || *end != '\0' || !isfinite(parsed)) {
        return false;
    }
    *y = parsed;
    return true;
}

static BacaTestResult test_destination_types_rotation_and_malformed_uri(void) {
    char *path = create_pdf_actions_fixture("pdf/actions.pdf");
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.toc_count, 5U);
    static const char *const labels[] = {"XYZ", "FitH", "FitBH", "FitR", "Rotated"};
    static const double expected_y[] = {0.2, 0.25, 0.5, 0.6};
    for (size_t index = 0U; index < BACA_ARRAY_LEN(labels); ++index) {
        TEST_ASSERT_STR(document.toc[index].label, labels[index]);
    }
    for (size_t index = 0U; index < BACA_ARRAY_LEN(expected_y); ++index) {
        double y = -1.0;
        TEST_ASSERT(pdf_target_y(document.toc[index].target, &y));
        TEST_ASSERT_DOUBLE(y, expected_y[index], 1e-12);
    }
    TEST_ASSERT_STR(document.toc[4].target, "pdf://page/1");

    const BacaImageBlock *image =
        &document.blocks[document.sections[0].first_block].value.image;
    TEST_ASSERT_SIZE(image->link_count, 1U);
    const char *uri = image->links[0].target;
    TEST_ASSERT(uri != NULL && g_utf8_validate(uri, -1, NULL));
    TEST_ASSERT(strncmp(uri, "https://valid.test/", 19U) == 0);
    TEST_ASSERT(strstr(uri, "tail") != NULL);

    BacaLayout fixed = {0};
    TEST_ASSERT_MSG(baca_layout_build_presentation(&fixed, &document, 20, BACA_JUSTIFY_LEFT,
                                                   BACA_PRESENTATION_FIXED, 1, 1, NULL, &error),
                    "%s", error.message);
    BacaSearchMatch *matches = NULL;
    size_t match_count = 0U;
    TEST_ASSERT_MSG(baca_layout_search(&fixed, "tail", &matches, &match_count, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(match_count, 1U);
    free(matches);
    matches = NULL;
    match_count = 0U;
    TEST_ASSERT_MSG(baca_layout_search(&fixed, "not-present", &matches, &match_count, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(match_count, 0U);
    free(matches);

    baca_layout_free(&fixed);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_ten_thousand_page_indexes_are_direct(void) {
    enum { PAGE_COUNT = 10000 };
    BacaDocument document = {
        .format = BACA_FORMAT_PDF,
        .default_presentation = BACA_PRESENTATION_FIXED,
    };
    BacaError error = {0};
    for (size_t page = 0U; page < PAGE_COUNT; ++page) {
        char target[64] = {0};
        const int length = snprintf(target, sizeof(target), "pdf://page/%zu", page);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(target));
        const size_t first_block = document.block_count;
        BacaBlock image = {
            .kind = BACA_BLOCK_IMAGE,
            .presentation = BACA_PRESENTATION_FIXED,
            .section_id = baca_strdup(target, &error),
            .value.image = {
                .uri = baca_strdup(target, &error),
                .anchor = baca_strdup(target, &error),
                .page_index = (int)page,
                .intrinsic_width = 1,
                .intrinsic_height = 1,
            },
        };
        TEST_ASSERT(image.section_id != NULL && image.value.image.uri != NULL &&
                    image.value.image.anchor != NULL);
        TEST_ASSERT(baca_document_add_image_block(&document, &image, &error));
        BacaBlock text = {
            .kind = BACA_BLOCK_TEXT,
            .presentation = BACA_PRESENTATION_REFLOW,
            .section_id = baca_strdup(target, &error),
            .value.text.text = baca_strdup("needle", &error),
        };
        TEST_ASSERT(text.section_id != NULL && text.value.text.text != NULL);
        TEST_ASSERT(baca_document_add_text_block(&document, &text, &error));
        BacaSection section = {
            .id = baca_strdup(target, &error),
            .first_block = first_block,
            .block_count = 2U,
            .linear = true,
        };
        TEST_ASSERT(section.id != NULL && baca_document_add_section(&document, &section, &error));
    }

    BacaLayout layout = {0};
    TEST_ASSERT_MSG(baca_layout_build_presentation(&layout, &document, 1, BACA_JUSTIFY_LEFT,
                                                   BACA_PRESENTATION_FIXED, 1, 1, NULL, &error),
                    "%s", error.message);
    TEST_ASSERT_SIZE(layout.section_first_line[PAGE_COUNT - 1U], (PAGE_COUNT - 1U) * 2U);
    TEST_ASSERT_SIZE(layout.block_section[(PAGE_COUNT - 1U) * 2U + 1U], PAGE_COUNT - 1U);
    size_t comparisons = 0U;
    TEST_ASSERT_SIZE(baca_layout_section_for_line(
                         &layout, layout.section_first_line[PAGE_COUNT - 1U], &comparisons),
                     PAGE_COUNT - 1U);
    TEST_ASSERT(comparisons <= 15U);
    TEST_ASSERT_SIZE(baca_layout_target_line(&layout, "pdf://page/9999"),
                     layout.section_first_line[PAGE_COUNT - 1U]);

    BacaSearchMatch *matches = NULL;
    size_t match_count = 0U;
    TEST_ASSERT_MSG(baca_layout_search(&layout, "needle", &matches, &match_count, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(match_count, PAGE_COUNT);
    for (size_t page = 0U; page < PAGE_COUNT; ++page) {
        TEST_ASSERT_SIZE(matches[page].line, layout.section_first_line[page]);
        TEST_ASSERT_SIZE(matches[page].block_index, page * 2U + 1U);
    }

    free(matches);
    baca_layout_free(&layout);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_open_metadata_toc_text_links_and_cleanup(void) {
    char *path = create_pdf_fixture("pdf/fixture.data", false, true);
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_PDF);
    TEST_ASSERT_INT(document.default_presentation, BACA_PRESENTATION_FIXED);
    TEST_ASSERT_STR(document.metadata.title, "Baca PDF Fixture");
    TEST_ASSERT_STR(document.metadata.author, "Fixture Author");
    TEST_ASSERT_STR(document.metadata.description, "Fixture subject");
    TEST_ASSERT_STR(document.metadata.creator, "Baca Cairo Tests");
    TEST_ASSERT(document.metadata.producer != NULL && strstr(document.metadata.producer, "cairo") != NULL);
    TEST_ASSERT(document.metadata.creation_date != NULL &&
                strstr(document.metadata.creation_date, "2026-07-19T12:00:00") != NULL);
    TEST_ASSERT(document.metadata.modification_date != NULL &&
                strstr(document.metadata.modification_date, "2026-07-19T13:00:00") != NULL);
    TEST_ASSERT_SIZE(document.section_count, 3U);
    TEST_ASSERT_STR(document.sections[0].id, "pdf://page/0");
    TEST_ASSERT_STR(document.sections[1].id, "pdf://page/1");
    TEST_ASSERT_STR(document.sections[2].id, "pdf://page/2");
    TEST_ASSERT_SIZE(document.sections[0].first_block, 0U);
    TEST_ASSERT(document.sections[0].block_count >= 2U);

    const BacaBlock *first_image = &document.blocks[document.sections[0].first_block];
    const BacaBlock *first_text = &document.blocks[document.sections[0].first_block + 1U];
    TEST_ASSERT_INT(first_image->kind, BACA_BLOCK_IMAGE);
    TEST_ASSERT_INT(first_image->presentation, BACA_PRESENTATION_FIXED);
    TEST_ASSERT_INT(first_image->value.image.page_index, 0);
    TEST_ASSERT_INT(first_image->value.image.intrinsic_width, 612);
    TEST_ASSERT_INT(first_image->value.image.intrinsic_height, 792);
    TEST_ASSERT_INT(first_text->kind, BACA_BLOCK_TEXT);
    TEST_ASSERT_INT(first_text->presentation, BACA_PRESENTATION_REFLOW);
    TEST_ASSERT(strstr(first_text->value.text.text, "First page alpha") != NULL);
    TEST_ASSERT(strstr(first_text->value.text.text, "caf\xc3\xa9") != NULL);
    TEST_ASSERT(strstr(first_text->value.text.text, "\xce\xa9") != NULL);

    const BacaImageBlock *image = &first_image->value.image;
    TEST_ASSERT_SIZE(image->link_count, 2U);
    const BacaImageLink *external = find_image_link(image, "https://example.test/pdf-link");
    const BacaImageLink *internal = find_image_link(image, "pdf://page/1");
    TEST_ASSERT(external != NULL && internal != NULL);
    TEST_ASSERT(external->x >= 0.0 && external->x < 1.0 && external->y >= 0.0 && external->y < 1.0);
    TEST_ASSERT(external->width > 0.0 && external->height > 0.0);
    TEST_ASSERT(internal->y > external->y);

    TEST_ASSERT_SIZE(document.toc_count, 3U);
    TEST_ASSERT_STR(document.toc[0].label, "First chapter");
    TEST_ASSERT_STR(document.toc[0].target, "pdf://page/0");
    TEST_ASSERT_INT((int)document.toc[0].depth, 0);
    TEST_ASSERT_STR(document.toc[1].label, "Landscape child");
    TEST_ASSERT_STR(document.toc[1].target, "pdf://page/1");
    TEST_ASSERT_INT((int)document.toc[1].depth, 1);
    TEST_ASSERT_SIZE(document.toc[1].section_index, 1U);
    TEST_ASSERT_STR(document.toc[2].label, "External reference");
    TEST_ASSERT_STR(document.toc[2].target, "https://example.test/pdf-outline");
    TEST_ASSERT_SIZE(document.toc[2].section_index, SIZE_MAX);

    const BacaBlock *second_image = &document.blocks[document.sections[1].first_block];
    TEST_ASSERT_INT(second_image->value.image.intrinsic_width, 792);
    TEST_ASSERT_INT(second_image->value.image.intrinsic_height, 612);
    const BacaBlock *scanned_text = &document.blocks[document.sections[2].first_block + 1U];
    TEST_ASSERT_STR(scanned_text->value.text.text, "");

    baca_document_close(&document);
    TEST_ASSERT(document.path == NULL && document.backend == NULL && document.blocks == NULL);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_fixed_reflow_search_and_link_lines(void) {
    char *path = create_pdf_fixture("pdf/layout.pdf", false, true);
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    BacaLayout fixed = {0};
    BacaLayout reflow = {0};
    TEST_ASSERT_MSG(baca_layout_build_presentation(&fixed, &document, 40, BACA_JUSTIFY_LEFT,
                                                   BACA_PRESENTATION_FIXED, 8, 16, NULL, &error),
                    "%s", error.message);
    TEST_ASSERT(fixed.line_count > 3U);
    TEST_ASSERT_INT(fixed.lines[fixed.block_first_line[document.sections[0].first_block]].kind,
                    BACA_LAYOUT_IMAGE);
    for (size_t line = 0U; line < fixed.line_count; ++line) {
        TEST_ASSERT(fixed.lines[line].kind != BACA_LAYOUT_TEXT);
    }

    BacaSearchMatch *matches = NULL;
    size_t match_count = 0U;
    TEST_ASSERT_MSG(baca_layout_search(&fixed, "needle", &matches, &match_count, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(match_count, 1U);
    const size_t second_fixed_line = baca_layout_target_line(&fixed, "pdf://page/1");
    const size_t third_fixed_line = baca_layout_target_line(&fixed, "pdf://page/2");
    TEST_ASSERT(matches[0].line > second_fixed_line && matches[0].line < third_fixed_line);
    TEST_ASSERT_SIZE(matches[0].block_index, document.sections[1].first_block + 1U);
    free(matches);

    TEST_ASSERT_MSG(baca_layout_build_presentation(&reflow, &document, 24, BACA_JUSTIFY_LEFT,
                                                   BACA_PRESENTATION_REFLOW, 1, 2, NULL, &error),
                    "%s", error.message);
    TEST_ASSERT(reflow.line_count > 3U);
    for (size_t line = 0U; line < reflow.line_count; ++line) {
        TEST_ASSERT(reflow.lines[line].kind != BACA_LAYOUT_IMAGE);
    }
    matches = NULL;
    match_count = 0U;
    TEST_ASSERT(baca_layout_search(&reflow, "needle", &matches, &match_count, &error));
    TEST_ASSERT_SIZE(match_count, 1U);
    TEST_ASSERT(matches[0].line >= baca_layout_target_line(&reflow, "pdf://page/1"));
    free(matches);

    matches = NULL;
    match_count = 0U;
    TEST_ASSERT(baca_layout_search(&reflow, "scan-secret", &matches, &match_count, &error));
    TEST_ASSERT_SIZE(match_count, 0U);
    free(matches);

    bool found_external_line = false;
    for (size_t block_index = document.sections[0].first_block;
         block_index < document.sections[0].first_block + document.sections[0].block_count; ++block_index) {
        const BacaBlock *block = &document.blocks[block_index];
        if (block->kind == BACA_BLOCK_TEXT && block->value.text.text != NULL &&
            strstr(block->value.text.text, "https://example.test/pdf-link") != NULL) {
            TEST_ASSERT(block->value.text.span_count >= 1U);
            bool found_span = false;
            for (size_t span = 0U; span < block->value.text.span_count; ++span) {
                if (block->value.text.spans[span].link != NULL &&
                    strcmp(block->value.text.spans[span].link, "https://example.test/pdf-link") == 0) {
                    found_span = true;
                    break;
                }
            }
            TEST_ASSERT(found_span);
            found_external_line = true;
        }
    }
    TEST_ASSERT(found_external_line);
    baca_layout_free(&reflow);
    baca_layout_free(&fixed);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_destination_coordinates_and_hidden_search_mapping(void) {
    char *path = create_tall_destination_pdf_fixture("pdf/destinations-and-search.pdf");
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.toc_count, 2U);
    TEST_ASSERT(strncmp(document.toc[0].target, "pdf://page/0#y=", 15U) == 0);
    TEST_ASSERT(strncmp(document.toc[1].target, "pdf://page/0#y=", 15U) == 0);
    TEST_ASSERT(strcmp(document.toc[0].target, document.toc[1].target) != 0);
    TEST_ASSERT_SIZE(document.toc[0].section_index, 0U);
    TEST_ASSERT_SIZE(document.toc[1].section_index, 0U);

    BacaLayout fixed = {0};
    TEST_ASSERT_MSG(baca_layout_build_presentation(&fixed, &document, 40, BACA_JUSTIFY_LEFT,
                                                   BACA_PRESENTATION_FIXED, 8, 16, NULL, &error),
                    "%s", error.message);
    const size_t page_start = baca_layout_target_line(&fixed, "pdf://page/0");
    const size_t upper_target = baca_layout_target_line(&fixed, document.toc[0].target);
    const size_t lower_target = baca_layout_target_line(&fixed, document.toc[1].target);
    TEST_ASSERT(page_start != SIZE_MAX && upper_target != SIZE_MAX && lower_target != SIZE_MAX);
    TEST_ASSERT(upper_target > page_start && lower_target > upper_target + 20U);
    TEST_ASSERT_SIZE(baca_layout_target_line(&fixed, "pdf://page/0#y=invalid"), page_start);
    const size_t page_span = fixed.line_count - page_start - 1U;
    TEST_ASSERT_DOUBLE((double)(upper_target - page_start) / (double)page_span, 0.2, 0.03);
    TEST_ASSERT_DOUBLE((double)(lower_target - page_start) / (double)page_span, 0.8, 0.03);

    BacaSearchMatch *matches = NULL;
    size_t match_count = 0U;
    TEST_ASSERT_MSG(baca_layout_search(&fixed, "CURRENT_MATCH", &matches, &match_count, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(match_count, 2U);
    TEST_ASSERT(matches[0].line > page_start && matches[1].line > matches[0].line + 20U);
    const size_t middle = matches[0].line + (matches[1].line - matches[0].line) / 2U;
    size_t forward = SIZE_MAX;
    for (size_t index = 0U; index < match_count; ++index) {
        if (matches[index].line >= middle) {
            forward = index;
            break;
        }
    }
    size_t backward = SIZE_MAX;
    for (size_t index = match_count; index > 0U; --index) {
        if (matches[index - 1U].line <= middle) {
            backward = index - 1U;
            break;
        }
    }
    TEST_ASSERT_SIZE(forward, 1U);
    TEST_ASSERT_SIZE(backward, 0U);

    free(matches);
    baca_layout_free(&fixed);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_lazy_render_dimensions_cache_theme_and_clipping(void) {
    char *path = create_pdf_fixture("pdf/render.pdf", false, true);
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);

    BacaResource direct = {0};
    TEST_ASSERT_MSG(baca_document_render_page(&document, 0, 123, 157, 0x112233U, &direct, &error), "%s",
                    error.message);
    TEST_ASSERT_STR(direct.mime_type, "image/png");
    int width = 0;
    int height = 0;
    TEST_ASSERT(baca_graphics_probe_resource(&direct, &width, &height, &error));
    TEST_ASSERT_INT(width, 123);
    TEST_ASSERT_INT(height, 157);
    baca_resource_free(&direct);

    BacaGraphicsContext *graphics = baca_graphics_create(
        BACA_GRAPHICS_DEFAULT_CACHE_BYTES, BACA_GRAPHICS_MULTIPLEXER_NONE, 0xffffffU, &error);
    TEST_ASSERT(graphics != NULL);
    TEST_ASSERT(baca_graphics_set_cell_pixels(graphics, 8, 16, &error));
    BacaGraphicsSurface first = {0};
    BacaGraphicsSurface repeated = {0};
    TEST_ASSERT_MSG(baca_graphics_prepare(graphics, &document, document.sections[0].first_block, 20, 25,
                                          &first, &error),
                    "%s", error.message);
    TEST_ASSERT_INT(first.width, 160);
    TEST_ASSERT_INT(first.height, 400);
    TEST_ASSERT_INT(first.columns, 20);
    TEST_ASSERT_INT(first.rows, 25);
    TEST_ASSERT_SIZE(baca_graphics_cache_stats(graphics).entries, 1U);
    TEST_ASSERT(baca_graphics_prepare(graphics, &document, document.sections[0].first_block, 20, 25,
                                      &repeated, &error));
    TEST_ASSERT_INT((int)repeated.image_id, (int)first.image_id);
    TEST_ASSERT_SIZE(baca_graphics_cache_stats(graphics).entries, 1U);

    const BacaGraphicsPlacement placement = {
        .row = -5,
        .column = 2,
        .viewport_rows = 8,
        .viewport_columns = 30,
    };
    PdfCellCapture capture = {0};
    TEST_ASSERT(baca_graphics_render_cells(&first, &placement, pdf_capture_cell, &capture, &error));
    TEST_ASSERT_SIZE(capture.count, 8U * 20U);
    TEST_ASSERT_INT(capture.first_row, 0);
    TEST_ASSERT_INT(capture.last_row, 7);

    baca_graphics_surface_release(&repeated);
    baca_graphics_surface_release(&first);
    TEST_ASSERT(baca_graphics_set_background(graphics, 0x000000U, &error));
    TEST_ASSERT_SIZE(baca_graphics_cache_stats(graphics).entries, 0U);
    TEST_ASSERT(baca_graphics_set_cell_pixels(graphics, 10, 20, &error));
    TEST_ASSERT(baca_graphics_prepare(graphics, &document, document.sections[0].first_block, 36, 24,
                                      &first, &error));
    TEST_ASSERT_INT(first.width, 360);
    TEST_ASSERT_INT(first.height, 480);
    TEST_ASSERT(first.pixels[0] == 255U && first.pixels[1] == 255U && first.pixels[2] == 255U);
    baca_graphics_surface_release(&first);
    baca_graphics_free(graphics);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_large_vector_page_renders_to_bounded_output(void) {
    char *path = create_raw_page_pdf_fixture("pdf/vector-large.pdf", "9000", "9000");
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    const size_t block_index = document.sections[0].first_block;
    TEST_ASSERT_INT(document.blocks[block_index].value.image.intrinsic_width, 9000);
    TEST_ASSERT_INT(document.blocks[block_index].value.image.intrinsic_height, 9000);

    BacaGraphicsContext *graphics = baca_graphics_create(
        BACA_GRAPHICS_DEFAULT_CACHE_BYTES, BACA_GRAPHICS_MULTIPLEXER_NONE, 0xffffffU, &error);
    TEST_ASSERT(graphics != NULL);
    TEST_ASSERT(baca_graphics_set_cell_pixels(graphics, 1, 1, &error));
    BacaGraphicsSurface surface = {0};
    TEST_ASSERT_MSG(baca_graphics_prepare(graphics, &document, block_index, 8, 8, &surface, &error), "%s",
                    error.message);
    TEST_ASSERT_INT(surface.width, 8);
    TEST_ASSERT_INT(surface.height, 8);
    baca_graphics_surface_release(&surface);
    baca_graphics_free(graphics);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_corrupt_empty_huge_and_output_validation(void) {
    TEST_ASSERT(baca_test_write_text("pdf/empty.pdf", ""));
    TEST_ASSERT(baca_test_write_text("pdf/corrupt.pdf", "%PDF-1.7\nnot a document\n"));
    char *empty = baca_test_path("pdf/empty.pdf");
    char *corrupt = baca_test_path("pdf/corrupt.pdf");
    char *huge = create_pdf_fixture("pdf/huge.pdf", true, false);
    TEST_ASSERT(empty != NULL && corrupt != NULL && huge != NULL);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, empty, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(document.path == NULL && document.backend == NULL);
    TEST_ASSERT(!baca_document_open(&document, corrupt, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(document.path == NULL && document.backend == NULL);
    TEST_ASSERT(!baca_document_open(&document, huge, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "dimensions") != NULL);

    char *valid = create_pdf_fixture("pdf/output.pdf", false, true);
    TEST_ASSERT(valid != NULL);
    TEST_ASSERT(baca_document_open(&document, valid, &error));
    BacaResource occupied = {
        .data = (unsigned char *)1,
    };
    TEST_ASSERT(!baca_document_render_page(&document, 0, 10, 10, 0U, &occupied, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    occupied = (BacaResource){0};
    TEST_ASSERT(!baca_document_render_page(&document, 99, 10, 10, 0U, &occupied, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    TEST_ASSERT(!baca_document_render_page(&document, 0, 32768, 32768, 0U, &occupied, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_document_close(&document);

    free(valid);
    free(huge);
    free(corrupt);
    free(empty);
    return BACA_TEST_PASS;
}

static BacaTestResult test_rotation_password_and_page_count_guards(void) {
    char *too_many = create_many_page_pdf_fixture("pdf/guards/too-many.pdf");
    TEST_ASSERT(too_many != NULL);
    TEST_ASSERT(baca_test_write("pdf/guards/rotated.pdf", PDF_ROTATED, sizeof(PDF_ROTATED) - 1U));
    TEST_ASSERT(baca_test_write("pdf/guards/zero.pdf", PDF_ZERO_PAGE, sizeof(PDF_ZERO_PAGE) - 1U));
    TEST_ASSERT(baca_test_write("pdf/guards/encrypted.pdf", PDF_ENCRYPTED, sizeof(PDF_ENCRYPTED) - 1U));
    char *rotated = baca_test_path("pdf/guards/rotated.pdf");
    char *zero = baca_test_path("pdf/guards/zero.pdf");
    char *encrypted = baca_test_path("pdf/guards/encrypted.pdf");
    TEST_ASSERT(rotated != NULL && zero != NULL && encrypted != NULL);

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, rotated, &error), "%s", error.message);
    TEST_ASSERT_STR(document.metadata.identifier, "f7d9ae4d4ad2b8b816408cd4f6492860");
    TEST_ASSERT_SIZE(strlen(document.metadata.identifier), 32U);
    const BacaImageBlock *image = &document.blocks[document.sections[0].first_block].value.image;
    TEST_ASSERT_INT(image->intrinsic_width, 100);
    TEST_ASSERT_INT(image->intrinsic_height, 200);
    BacaResource rendered = {0};
    TEST_ASSERT(baca_document_render_page(&document, 0, 37, 73, 0xffffffU, &rendered, &error));
    int width = 0;
    int height = 0;
    TEST_ASSERT(baca_graphics_probe_resource(&rendered, &width, &height, &error));
    TEST_ASSERT_INT(width, 37);
    TEST_ASSERT_INT(height, 73);
    baca_resource_free(&rendered);
    baca_document_close(&document);

    TEST_ASSERT(!baca_document_open(&document, encrypted, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
    TEST_ASSERT(strstr(error.message, "password required") != NULL);
    TEST_ASSERT(document.path == NULL && document.backend == NULL);
    TEST_ASSERT(!baca_document_open(&document, zero, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "no pages") != NULL);
    TEST_ASSERT(document.path == NULL && document.backend == NULL);
    TEST_ASSERT(!baca_document_open(&document, too_many, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "limit of 10000") != NULL);
    TEST_ASSERT(document.path == NULL && document.backend == NULL);

    free(encrypted);
    free(zero);
    free(rotated);
    free(too_many);
    return BACA_TEST_PASS;
}

static BacaTestResult test_outline_total_node_limit_counts_untitled_nodes(void) {
    char *path = create_many_outline_pdf_fixture("pdf/guards/too-many-outline-nodes.pdf");
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "outline") != NULL && strstr(error.message, "node") != NULL);
    TEST_ASSERT(document.path == NULL && document.backend == NULL);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_aggregate_link_target_budget_precedes_retention(void) {
    char *path = create_link_budget_pdf_fixture("pdf/guards/link-target-budget.pdf");
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_pdf_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "link target") != NULL && strstr(error.message, "size") != NULL);
    TEST_ASSERT_SIZE(document.toc_count, PDF_LINK_TARGET_BYTE_LIMIT / PDF_STRESS_TARGET_BYTES);
    TEST_ASSERT(document.retained_bytes < (size_t)PDF_LINK_TARGET_BYTE_LIMIT * 2U);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_tui_one_page_fit_preserves_restored_complete_progress(void) {
    char *path = create_raw_page_pdf_fixture("pdf/progress/one-page-fit.pdf", "612", "100");
    TEST_ASSERT(path != NULL);
    const struct winsize size = {
        .ws_row = 30U,
        .ws_col = 60U,
        .ws_xpixel = 600U,
        .ws_ypixel = 600U,
    };
    double progress = -1.0;
    BacaString output = {0};
    const bool ran = run_pdf_progress_pty("one-page-fit", path, &size, true, false, &progress, &output);
    const char *captured = output.data == NULL ? "(empty)" : output.data;
    BacaTestResult result = BACA_TEST_PASS;
    if (!ran) {
        result = baca_test_fail_at(__FILE__, __LINE__,
                                   "one-page PDF progress PTY failed after %zu bytes: %.800s",
                                   output.length, captured);
    } else if (progress != 1.0) {
        result = baca_test_fail_at(__FILE__, __LINE__,
                                   "one-page restored progress was %.17g, expected exactly 1", progress);
    }
    baca_string_free(&output);
    free(path);
    return result;
}

static BacaTestResult test_tui_multi_page_end_persists_complete_progress(void) {
    char *path = create_pdf_fixture("pdf/progress/multi-page-end.pdf", false, false);
    TEST_ASSERT(path != NULL);
    const struct winsize size = {
        .ws_row = 12U,
        .ws_col = 40U,
        .ws_xpixel = 400U,
        .ws_ypixel = 240U,
    };
    double progress = -1.0;
    BacaString output = {0};
    const bool ran = run_pdf_progress_pty("multi-page-end", path, &size, false, true, &progress, &output);
    const char *captured = output.data == NULL ? "(empty)" : output.data;
    BacaTestResult result = BACA_TEST_PASS;
    if (!ran) {
        result = baca_test_fail_at(__FILE__, __LINE__,
                                   "multi-page PDF End progress PTY failed after %zu bytes: %.800s",
                                   output.length, captured);
    } else if (progress != 1.0) {
        result = baca_test_fail_at(__FILE__, __LINE__,
                                   "multi-page End progress was %.17g, expected exactly 1", progress);
    }
    baca_string_free(&output);
    free(path);
    return result;
}

static BacaTestResult test_non_pdf_v_conflict_uses_configured_action(void) {
    BacaString output = {0};
    const bool ran = run_non_pdf_key_conflict_pty(&output);
    const char *captured = output.data == NULL ? "(empty)" : output.data;
    BacaTestResult result = ran ? BACA_TEST_PASS
                                : baca_test_fail_at(__FILE__, __LINE__,
                                                    "non-PDF v conflict PTY failed after %zu bytes: %.800s",
                                                    output.length, captured);
    baca_string_free(&output);
    return result;
}

static BacaTestResult test_tui_current_page_forward_and_backward_search(void) {
    BacaString output = {0};
    unsigned stage = 0U;
    const bool ran = run_pdf_current_page_search_pty(&output, &stage);
    const char *captured = output.data == NULL ? "(empty)" : output.data;
    const char *tail = output.data == NULL || output.length <= 800U ? captured : output.data + output.length - 800U;
    const BacaTestResult result = ran ? BACA_TEST_PASS
                                      : baca_test_fail_at(__FILE__, __LINE__,
                                                          "current-page PDF search PTY failed at stage %u after %zu bytes; tail: %.800s",
                                                          stage, output.length, tail);
    baca_string_free(&output);
    return result;
}

static BacaTestResult test_tui_failed_render_fallback_opens_document_and_retries_after_resize(void) {
    BacaString output = {0};
    unsigned stage = 0U;
    const bool ran = run_pdf_resize_retry_pty(&output, &stage);
    const char *captured = output.data == NULL ? "(empty)" : output.data;
    const char *tail = output.data == NULL || output.length <= 800U ? captured : output.data + output.length - 800U;
    const BacaTestResult result = ran ? BACA_TEST_PASS
                                      : baca_test_fail_at(__FILE__, __LINE__,
                                                          "PDF fallback click/resize retry PTY failed at stage %u after %zu bytes; tail: %.800s",
                                                          stage, output.length, tail);
    baca_string_free(&output);
    return result;
}

static BacaTestResult test_tui_fixed_toggle_search_links_resize_and_signal(void) {
    PdfPtyResult result = {0};
    const bool ran = run_pdf_pty(&result);
    const char *output = result.output.data == NULL ? "(empty)" : result.output.data;
    const char *opened = result.opened.data == NULL ? "(empty)" : result.opened.data;
    BacaTestResult test_result = BACA_TEST_PASS;
    if (!ran) {
        test_result = baca_test_fail_at(__FILE__, __LINE__,
                                        "PDF PTY workflow stopped early after %zu bytes (fixed=%d help-open=%d help=%d text=%d search=%d "
                                        "resize=%d internal=%d external=%d toc-internal=%d toc-external=%d page=%d); "
                                        "opened: %s; output: %.800s",
                                        result.output.length, result.fixed_rendered,
                                        result.help_opened,
                                        result.help_shows_configured_toggle, result.toggled_to_text,
                                        result.search_reached_page, result.resize_preserved_page,
                                        result.internal_link_reached_page, result.external_link_opened,
                                        result.internal_toc_reached_page, result.external_toc_opened,
                                        result.page_opened, opened, output);
    } else if (!result.fixed_rendered) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "fixed PDF page did not render: %.800s", output);
    } else if (!result.help_shows_configured_toggle) {
        test_result = baca_test_fail_at(__FILE__, __LINE__,
                                        "PDF help did not show the configured toggle mapping: %.800s", output);
    } else if (!result.toggled_to_text) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "configured PDF toggle did not reveal text: %.800s",
                                        output);
    } else if (!result.search_reached_page) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "fixed-view search did not reach page 2: %.800s", output);
    } else if (!result.resize_preserved_page) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "PDF resize did not preserve page 2: %.800s", output);
    } else if (!result.internal_link_reached_page) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "PDF internal link did not reach page 2: %.800s", output);
    } else if (!result.external_link_opened) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "PDF external link was not captured: %s", opened);
    } else if (!result.internal_toc_reached_page) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "PDF internal TOC target did not reach page 2: %.800s",
                                        output);
    } else if (!result.external_toc_opened) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "PDF external TOC target was not captured: %s", opened);
    } else if (!result.page_opened) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "unmapped PDF page click was not captured: %s", opened);
    } else if (!result.completed || !WIFEXITED(result.status) || WEXITSTATUS(result.status) != 128 + SIGTERM) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "PDF PTY child did not exit after SIGTERM");
    } else if (strstr(output, "d=I,i=") == NULL) {
        test_result = baca_test_fail_at(__FILE__, __LINE__, "PDF Kitty image was not deleted");
    }
    baca_string_free(&result.opened);
    baca_string_free(&result.output);
    return test_result;
}

const BacaTestCase *baca_pdf_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "open_metadata_toc_text_links_and_cleanup",
         .function = test_open_metadata_toc_text_links_and_cleanup},
        {.name = "fixed_reflow_search_and_link_lines",
         .function = test_fixed_reflow_search_and_link_lines},
        {.name = "destination_coordinates_and_hidden_search_mapping",
         .function = test_destination_coordinates_and_hidden_search_mapping},
        {.name = "destination_types_rotation_and_malformed_uri",
         .function = test_destination_types_rotation_and_malformed_uri},
        {.name = "ten_thousand_page_indexes_are_direct",
         .function = test_ten_thousand_page_indexes_are_direct},
        {.name = "lazy_render_dimensions_cache_theme_and_clipping",
         .function = test_lazy_render_dimensions_cache_theme_and_clipping},
        {.name = "large_vector_page_renders_to_bounded_output",
         .function = test_large_vector_page_renders_to_bounded_output},
        {.name = "corrupt_empty_huge_and_output_validation",
         .function = test_corrupt_empty_huge_and_output_validation},
        {.name = "rotation_password_and_page_count_guards",
         .function = test_rotation_password_and_page_count_guards},
        {.name = "outline_total_node_limit_counts_untitled_nodes",
         .function = test_outline_total_node_limit_counts_untitled_nodes},
        {.name = "aggregate_link_target_budget_precedes_retention",
         .function = test_aggregate_link_target_budget_precedes_retention},
        {.name = "tui_one_page_fit_preserves_restored_complete_progress",
         .function = test_tui_one_page_fit_preserves_restored_complete_progress},
        {.name = "tui_multi_page_end_persists_complete_progress",
         .function = test_tui_multi_page_end_persists_complete_progress},
        {.name = "non_pdf_v_conflict_uses_configured_action",
         .function = test_non_pdf_v_conflict_uses_configured_action},
        {.name = "tui_current_page_forward_and_backward_search",
         .function = test_tui_current_page_forward_and_backward_search},
        {.name = "tui_failed_render_fallback_opens_document_and_retries_after_resize",
         .function = test_tui_failed_render_fallback_opens_document_and_retries_after_resize},
        {.name = "tui_fixed_toggle_search_links_resize_and_signal",
         .function = test_tui_fixed_toggle_search_links_resize_and_signal},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
