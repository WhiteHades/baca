#include "test_support.h"

#include "mereader-tui/document.h"

#include <stdlib.h>

static const char fb2_fixture[] =
    "<?xml version='1.0' encoding='UTF-8'?>"
    "<FictionBook xmlns='http://www.gribuser.ru/xml/fictionbook/2.0' "
    "xmlns:l='http://www.w3.org/1999/xlink'>"
    "<description><title-info><genre>science</genre>"
    "<author><first-name>Ada</first-name><last-name>Lovelace</last-name></author>"
    "<author><nickname>Analyst</nickname></author><book-title>Native FB2</book-title>"
    "<annotation><p>A short <emphasis>annotation</emphasis>.</p></annotation>"
    "<date value='1843'>Eighteen forty-three</date><coverpage><image l:href='#cover.png'/></coverpage>"
    "<lang>en</lang></title-info>"
    "<document-info><author><first-name>Editor</first-name><last-name>One</last-name></author>"
    "<program-used>mereader-tui Fixture</program-used><date value='2026-07-19'>Today</date>"
    "<src-url>https://example.invalid/source</src-url><id>native-fb2-id</id></document-info>"
    "<publish-info><publisher>Example Press</publisher></publish-info></description>"
    "<body><title><p>Native FB2</p></title><section id='chapter-1'><title><p>Chapter One</p></title>"
    "<p><strong>Bold</strong> and <emphasis>italic</emphasis> "
    "<a l:href='#note-1'>note</a>.</p><image l:href='#cover.png'/>"
    "<section><title><p>Nested</p></title><p>Nested text.</p></section></section></body>"
    "<body name='notes'><section id='note-1'><title><p>Note One</p></title><p>Footnote text.</p></section></body>"
    "<binary id='cover.png' content-type='image/png'>"
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
    "</binary></FictionBook>";

static const MereaderTuiBlock *find_text_block(const MereaderTuiDocument *document, const char *needle) {
    for (size_t index = 0U; index < document->block_count; ++index) {
        const MereaderTuiBlock *block = &document->blocks[index];
        if (block->kind == MEREADER_TUI_BLOCK_TEXT && block->value.text.text != NULL &&
            strstr(block->value.text.text, needle) != NULL) {
            return block;
        }
    }
    return NULL;
}

static MereaderTuiTestResult test_metadata_toc_styles_links_bodies_and_image(void) {
    TEST_ASSERT(mereader_tui_test_write_text("fb2/native.fb2", fb2_fixture));
    char *path = mereader_tui_test_path("fb2/native.fb2");
    TEST_ASSERT(path != NULL);
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_FB2);
    TEST_ASSERT_STR(document.metadata.title, "Native FB2");
    TEST_ASSERT_STR(document.metadata.author, "Ada Lovelace, Analyst");
    TEST_ASSERT_STR(document.metadata.creator, "Editor One");
    TEST_ASSERT_STR(document.metadata.description, "A short annotation.");
    TEST_ASSERT_STR(document.metadata.publisher, "Example Press");
    TEST_ASSERT_STR(document.metadata.producer, "mereader-tui Fixture");
    TEST_ASSERT_STR(document.metadata.date, "1843");
    TEST_ASSERT_STR(document.metadata.creation_date, "2026-07-19");
    TEST_ASSERT_STR(document.metadata.language, "en");
    TEST_ASSERT_STR(document.metadata.format, "FictionBook 2");
    TEST_ASSERT_STR(document.metadata.identifier, "native-fb2-id");
    TEST_ASSERT_STR(document.metadata.source, "https://example.invalid/source");
    TEST_ASSERT_SIZE(document.section_count, 1U);
    TEST_ASSERT_SIZE(document.toc_count, 3U);
    TEST_ASSERT_STR(document.toc[0].label, "Chapter One");
    TEST_ASSERT_SIZE(document.toc[0].depth, 0U);
    TEST_ASSERT_STR(document.toc[1].label, "Nested");
    TEST_ASSERT_SIZE(document.toc[1].depth, 1U);
    TEST_ASSERT_STR(document.toc[2].label, "Note One");

    const MereaderTuiBlock *paragraph = find_text_block(&document, "Bold and italic note.");
    TEST_ASSERT(paragraph != NULL);
    TEST_ASSERT_SIZE(paragraph->value.text.span_count, 3U);
    TEST_ASSERT_INT(paragraph->value.text.spans[0].style, MEREADER_TUI_STYLE_BOLD);
    TEST_ASSERT_INT(paragraph->value.text.spans[1].style, MEREADER_TUI_STYLE_ITALIC);
    TEST_ASSERT_STR(paragraph->value.text.spans[2].link, "fb2/document#note-1");
    TEST_ASSERT(find_text_block(&document, "Footnote text.") != NULL);

    const MereaderTuiBlock *image = NULL;
    for (size_t index = 0U; index < document.block_count; ++index) {
        if (document.blocks[index].kind == MEREADER_TUI_BLOCK_IMAGE) {
            image = &document.blocks[index];
            break;
        }
    }
    TEST_ASSERT(image != NULL);
    TEST_ASSERT_STR(image->value.image.uri, "fb2://binary/0.png");
    MereaderTuiResource resource = {0};
    TEST_ASSERT_MSG(mereader_tui_document_load_resource(&document, image->value.image.uri, &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, 68U);
    TEST_ASSERT_INT(resource.data[0], 0x89);
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    mereader_tui_resource_free(&resource);
    mereader_tui_document_probe_images(&document, true);
    TEST_ASSERT(!image->value.image.broken);
    TEST_ASSERT_INT(image->value.image.intrinsic_width, 1);
    TEST_ASSERT_INT(image->value.image.intrinsic_height, 1);

    char *held = mereader_tui_test_path("fb2/native-held.fb2");
    TEST_ASSERT(held != NULL);
    TEST_ASSERT(rename(path, held) == 0);
    TEST_ASSERT(mereader_tui_test_write_text("fb2/native.fb2", "replacement"));
    TEST_ASSERT(mereader_tui_document_load_resource(&document, image->value.image.uri, &resource, &error));
    TEST_ASSERT_INT(resource.data[0], 0x89);
    mereader_tui_resource_free(&resource);

    mereader_tui_document_close(&document);
    free(held);
    free(path);

    TEST_ASSERT(mereader_tui_test_write_text("fb2/by-magic.data", fb2_fixture));
    path = mereader_tui_test_path("fb2/by-magic.data");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, MEREADER_TUI_FORMAT_FB2);
    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_malformed_dtd_and_binary_errors(void) {
    static const char *const fixtures[] = {
        "<not-fiction-book/>",
        "<!DOCTYPE FictionBook [<!ENTITY x 'unsafe'>]><FictionBook><body><p>&x;</p></body></FictionBook>",
        "<FictionBook><description/><binary id='x.png' content-type='image/png'>%%%"
        "</binary><body><image href='#x.png'/></body></FictionBook>",
        "<FictionBook><description/><body/><binary id='x.png' content-type='image/png'>AA==</binary>"
        "<binary id='x.png' content-type='image/png'>AA==</binary></FictionBook>",
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(fixtures); ++index) {
        char relative[64] = {0};
        TEST_ASSERT(snprintf(relative, sizeof(relative), "fb2/invalid-%zu.fb2", index) > 0);
        TEST_ASSERT(mereader_tui_test_write_text(relative, fixtures[index]));
        char *path = mereader_tui_test_path(relative);
        TEST_ASSERT(path != NULL);
        MereaderTuiDocument document = {0};
        MereaderTuiError error = {0};
        TEST_ASSERT(!mereader_tui_document_open(&document, path, &error));
        TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
        free(path);
    }
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_minimal_book_without_images(void) {
    static const char minimal[] = "<FictionBook><body><section><title><p>Only Chapter</p></title>"
                                  "<p>Text only.</p></section></body></FictionBook>";
    TEST_ASSERT(mereader_tui_test_write_text("fb2/minimal.fb2", minimal));
    char *path = mereader_tui_test_path("fb2/minimal.fb2");
    TEST_ASSERT(path != NULL);
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_STR(document.metadata.title, "minimal.fb2");
    TEST_ASSERT_SIZE(document.toc_count, 1U);
    TEST_ASSERT(find_text_block(&document, "Text only.") != NULL);
    mereader_tui_document_close(&document);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_fb2_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "metadata_toc_styles_links_bodies_and_image",
         .function = test_metadata_toc_styles_links_bodies_and_image},
        {.name = "malformed_dtd_and_binary_errors", .function = test_malformed_dtd_and_binary_errors},
        {.name = "minimal_book_without_images", .function = test_minimal_book_without_images},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
