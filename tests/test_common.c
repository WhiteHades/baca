#include "test_support.h"

#include "baca/document_backend.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static BacaTestResult test_checked_strings_and_buffers(void) {
    BacaError error = {0};
    BacaString string = {0};
    TEST_ASSERT(baca_string_append(&string, "alpha", &error));
    TEST_ASSERT(baca_string_append_n(&string, string.data + 1, 3U, &error));
    TEST_ASSERT(baca_string_append_char(&string, '!', &error));
    TEST_ASSERT_STR(string.data, "alphalph!");
    TEST_ASSERT_SIZE(string.length, 9U);
    TEST_ASSERT(string.data[string.length] == '\0');
    char *taken = baca_string_take(&string);
    TEST_ASSERT_STR(taken, "alphalph!");
    TEST_ASSERT(string.data == NULL && string.length == 0U && string.capacity == 0U);
    free(taken);

    BacaBuffer buffer = {0};
    static const unsigned char bytes[] = {0x00U, 0x7fU, 0xffU};
    TEST_ASSERT(baca_buffer_append(&buffer, bytes, sizeof(bytes), &error));
    TEST_ASSERT(baca_buffer_append(&buffer, buffer.data, buffer.length, &error));
    TEST_ASSERT_SIZE(buffer.length, 6U);
    TEST_ASSERT(memcmp(buffer.data, bytes, sizeof(bytes)) == 0);
    TEST_ASSERT(memcmp(buffer.data + sizeof(bytes), bytes, sizeof(bytes)) == 0);
    TEST_ASSERT(buffer.data[buffer.length] == '\0');
    baca_buffer_free(&buffer);

    TEST_ASSERT(!baca_string_append(NULL, "x", &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_error_clear(&error);
    TEST_ASSERT(!baca_buffer_append(&buffer, NULL, 1U, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_error_clear(&error);
    TEST_ASSERT(baca_reallocarray(NULL, SIZE_MAX, 2U, &error) == NULL);
    TEST_ASSERT_ERROR(error, BACA_ERROR_MEMORY);

    size_t capacity = 1U;
    TEST_ASSERT(baca_array_reserve(NULL, &capacity, sizeof(int), 1U, &error) == NULL);
    TEST_ASSERT_ERROR(error, BACA_ERROR_INTERNAL);
    return BACA_TEST_PASS;
}

static BacaTestResult test_files_and_paths(void) {
    static const unsigned char payload[] = {'a', '\0', 'b', '\n'};
    TEST_ASSERT(baca_test_write("common/nested/data.bin", payload, sizeof(payload)));
    char *path = baca_test_path("common/nested/data.bin");
    TEST_ASSERT(path != NULL);
    BacaError error = {0};
    BacaBuffer read = {0};
    TEST_ASSERT(baca_read_file(path, &read, &error));
    TEST_ASSERT_SIZE(read.length, sizeof(payload));
    TEST_ASSERT(memcmp(read.data, payload, sizeof(payload)) == 0);
    TEST_ASSERT(read.data[read.length] == '\0');
    TEST_ASSERT(baca_file_exists(path));

    char *directory = baca_path_dirname(path, &error);
    char *basename = baca_path_basename(path, &error);
    char *stem = baca_path_stem(path, &error);
    TEST_ASSERT(directory != NULL && basename != NULL && stem != NULL);
    TEST_ASSERT(baca_directory_exists(directory));
    TEST_ASSERT_STR(basename, "data.bin");
    TEST_ASSERT_STR(stem, "data");
    TEST_ASSERT_STR(baca_path_extension(path), ".bin");
    free(directory);
    free(basename);
    free(stem);
    baca_buffer_free(&read);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_utf8_width(void) {
    static const char text[] = "A界e\xcc\x81🙂";
    TEST_ASSERT_SIZE(baca_utf8_width(text, strlen(text)), 6U);
    int columns = -1;
    size_t offset = baca_utf8_next(text, strlen(text), 1U, &columns);
    TEST_ASSERT_SIZE(offset, 4U);
    TEST_ASSERT_INT(columns, 2);
    offset = baca_utf8_next(text, strlen(text), 5U, &columns);
    TEST_ASSERT_SIZE(offset, 7U);
    TEST_ASSERT_INT(columns, 0);
    static const char controls[] = "\x01\x7f";
    TEST_ASSERT_SIZE(baca_utf8_width(controls, sizeof(controls) - 1U), 0U);
    static const char malformed[] = "\xf0\x28\x8c\x28";
    TEST_ASSERT_SIZE(baca_utf8_width(malformed, sizeof(malformed) - 1U), 4U);
    TEST_ASSERT_SIZE(baca_utf8_width(NULL, 20U), 0U);
    return BACA_TEST_PASS;
}

static BacaTestResult test_uri_components_and_traversal(void) {
    BacaError error = {0};
    char *decoded = baca_uri_decode("part%20one%23x%3Fy", &error);
    TEST_ASSERT_STR(decoded, "part one#x?y");
    free(decoded);

    char *resolved = baca_document_resolve_uri("OPS/Text/chapter.xhtml?old=1#old",
                                               "../Images/a%23b%3Fc.png?size=1%3F2#spot%23one", false,
                                               &error);
    TEST_ASSERT_STR(resolved, "OPS/Images/a%23b%3Fc.png?size=1?2#spot%23one");
    char *member = baca_document_uri_path(resolved, &error);
    TEST_ASSERT_STR(member, "OPS/Images/a#b?c.png");
    free(member);
    free(resolved);

    resolved = baca_document_resolve_uri("OPS/Text/one.xhtml", "./nested/../two.xhtml#here", false, &error);
    TEST_ASSERT_STR(resolved, "OPS/Text/two.xhtml#here");
    free(resolved);
    resolved = baca_document_resolve_uri("OPS/Text/one.xhtml", "../../../escape.xhtml", false, &error);
    TEST_ASSERT(resolved == NULL);
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    baca_error_clear(&error);

    char *target = baca_document_fragment_target("OPS/a%23b%3F.xhtml?query#old", "id?#value", &error);
    TEST_ASSERT_STR(target, "OPS/a%23b%3F.xhtml#id?%23value");
    free(target);
    TEST_ASSERT(baca_is_external_uri("https://example.test/book"));
    TEST_ASSERT(baca_is_external_uri("mailto:reader@example.test"));
    TEST_ASSERT(!baca_is_external_uri("Text/chapter.xhtml"));
    TEST_ASSERT(baca_document_resolve_uri("OPS/a.xhtml", "https://example.test", false, &error) == NULL);
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    return BACA_TEST_PASS;
}

static BacaTestResult test_xdg_isolation(void) {
    BacaError error = {0};
    char *expected_config = baca_test_path("xdg-config/mereader-tui/config.ini");
    char *expected_cache = baca_test_path("xdg-cache/mereader-tui/history/mereader-tui.db");
    char *config = baca_xdg_config_path("config.ini", &error);
    char *cache = baca_xdg_cache_path("history/mereader-tui.db", &error);
    TEST_ASSERT(expected_config != NULL && expected_cache != NULL && config != NULL && cache != NULL);
    TEST_ASSERT_STR(config, expected_config);
    TEST_ASSERT_STR(cache, expected_cache);
    free(expected_config);
    free(expected_cache);
    free(config);
    free(cache);

    TEST_ASSERT(baca_xdg_cache_path("../escape", &error) == NULL);
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_error_clear(&error);
    TEST_ASSERT(baca_xdg_config_path("absolute/../escape", &error) == NULL);
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);

    char *temporary = baca_make_temp_directory("common-temp-", &error);
    TEST_ASSERT(temporary != NULL);
    char *temporary_root = baca_test_path("tmp/");
    TEST_ASSERT(temporary_root != NULL);
    TEST_ASSERT(strncmp(temporary, temporary_root, strlen(temporary_root)) == 0);
    TEST_ASSERT(baca_directory_exists(temporary));
    TEST_ASSERT(baca_remove_tree(temporary, &error));
    free(temporary_root);
    free(temporary);
    return BACA_TEST_PASS;
}

static BacaTestResult test_symlink_safe_tree_removal(void) {
    TEST_ASSERT(baca_test_write_text("remove/outside/keep.txt", "keep"));
    TEST_ASSERT(baca_test_write_text("remove/tree/inside.txt", "inside"));
    char *tree = baca_test_path("remove/tree");
    char *link = baca_test_path("remove/tree/outside-link");
    char *outside_file = baca_test_path("remove/outside/keep.txt");
    TEST_ASSERT(tree != NULL && link != NULL && outside_file != NULL);
    TEST_ASSERT(symlink("../outside", link) == 0);

    BacaError error = {0};
    TEST_ASSERT(baca_remove_tree(tree, &error));
    TEST_ASSERT(!baca_directory_exists(tree));
    TEST_ASSERT(baca_file_exists(outside_file));
    TEST_ASSERT(baca_remove_tree(tree, &error));
    TEST_ASSERT(!baca_remove_tree("/", &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    free(tree);
    free(link);
    free(outside_file);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_common_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "checked_strings_and_buffers", .function = test_checked_strings_and_buffers},
        {.name = "files_and_paths", .function = test_files_and_paths},
        {.name = "utf8_width", .function = test_utf8_width},
        {.name = "uri_components_and_traversal", .function = test_uri_components_and_traversal},
        {.name = "xdg_isolation", .function = test_xdg_isolation},
        {.name = "symlink_safe_tree_removal", .function = test_symlink_safe_tree_removal},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
