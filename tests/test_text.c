#include "test_support.h"

#include "baca/document.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_TEXT_MAX_BYTES (64U * 1024U * 1024U)

static bool text_pty_write(int descriptor, const char *value, size_t length) {
    size_t offset = 0U;
    while (offset < length) {
        const ssize_t written = write(descriptor, value + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool text_pty_drain(int descriptor, BacaString *output) {
    char buffer[4096] = {0};
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

static bool text_pty_wait_for(int descriptor, BacaString *output, size_t start, const char *needle) {
    for (unsigned attempt = 0U; attempt < 500U; ++attempt) {
        struct pollfd input = {.fd = descriptor, .events = POLLIN};
        const int ready = poll(&input, 1U, 20);
        if ((ready < 0 && errno != EINTR) || !text_pty_drain(descriptor, output)) {
            return false;
        }
        if (output->data != NULL && start <= output->length && strstr(output->data + start, needle) != NULL) {
            return true;
        }
    }
    return false;
}

static bool text_pty_wait_for_exit(pid_t child, int descriptor, BacaString *output, int *status) {
    for (unsigned attempt = 0U; attempt < 500U; ++attempt) {
        (void)text_pty_drain(descriptor, output);
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

static bool run_vim_navigation_pty(BacaString *output, unsigned *stage) {
    *stage = 0U;
    static const char markers[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcd";
    BacaString fixture = {0};
    BacaError error = {0};
    for (size_t line = 0U; line < sizeof(markers) - 1U; ++line) {
        char value[32] = {0};
        const int length =
            snprintf(value, sizeof(value), "%c%c%c%c%c%c%c%c\n", markers[line], markers[line], markers[line],
                     markers[line], markers[line], markers[line], markers[line], markers[line]);
        if (length <= 0 || (size_t)length >= sizeof(value) ||
            !baca_string_append_n(&fixture, value, (size_t)length, &error)) {
            baca_string_free(&fixture);
            return false;
        }
    }
    const bool prepared =
        baca_test_write_text("text/vim/reader.txt", fixture.data) &&
        baca_test_write_text("text/vim/config/baca/config.ini", "[General]\nMaxTextWidth=40\nPageScrollDuration=0\n"
                                                                "ImageMode=placeholder\n") &&
        baca_test_mkdir("text/vim/cache");
    baca_string_free(&fixture);
    char *path = baca_test_path("text/vim/reader.txt");
    char *config = baca_test_path("text/vim/config");
    char *cache = baca_test_path("text/vim/cache");
    if (!prepared || path == NULL || config == NULL || cache == NULL) {
        free(cache);
        free(config);
        free(path);
        return false;
    }

    const struct winsize size = {
        .ws_row = 5U,
        .ws_col = 40U,
        .ws_xpixel = 400U,
        .ws_ypixel = 100U,
    };
    int master = -1;
    (void)fflush(NULL);
    const pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child < 0) {
        free(cache);
        free(config);
        free(path);
        return false;
    }
    if (child == 0) {
        (void)setenv("TERM", "xterm-256color", 1);
        (void)setenv("XDG_CONFIG_HOME", config, 1);
        (void)setenv("XDG_CACHE_HOME", cache, 1);
        (void)unsetenv("TERM_PROGRAM");
        (void)unsetenv("KITTY_WINDOW_ID");
        (void)execl("./build/baca", "baca", path, (char *)NULL);
        _exit(127);
    }

    bool ok = fcntl(master, F_SETFL, O_NONBLOCK) == 0 && text_pty_wait_for(master, output, 0U, "00000000");
    if (ok) {
        *stage = 1U;
    }
    size_t start = output->length;
    ok = ok && text_pty_write(master, "12j", 3U) && text_pty_wait_for(master, output, start, "CCCCCCCC");
    if (ok) {
        *stage = 2U;
    }
    start = output->length;
    ok = ok && text_pty_write(master, "G", 1U) && text_pty_wait_for(master, output, start, "dddddddd");
    if (ok) {
        *stage = 3U;
    }
    start = output->length;
    ok = ok && text_pty_write(master, "\017", 1U) && text_pty_wait_for(master, output, start, "CCCCCCCC");
    if (ok) {
        *stage = 4U;
    }
    start = output->length;
    static const char ctrl_i[] = "\033[105;5u";
    ok = ok && text_pty_write(master, ctrl_i, sizeof(ctrl_i) - 1U) &&
         text_pty_wait_for(master, output, start, "dddddddd");
    if (ok) {
        *stage = 5U;
    }
    start = output->length;
    ok = ok && text_pty_write(master, "5gg", 3U) && text_pty_wait_for(master, output, start, "44444444");
    if (ok) {
        *stage = 6U;
    }
    start = output->length;
    ok = ok && text_pty_write(master, "10G", 3U) && text_pty_wait_for(master, output, start, "99999999") &&
         text_pty_write(master, "2\006", 2U);
    if (ok) {
        *stage = 7U;
    }
    start = output->length;
    ok = ok && text_pty_wait_for(master, output, start, "HHHHHHHH") && text_pty_write(master, "2\002", 2U);
    if (ok) {
        *stage = 8U;
    }
    start = output->length;
    static const char search[] = "/(AAAAAAAA|MMMMMMMM|ZZZZZZZZ)\n";
    ok = ok && text_pty_wait_for(master, output, start, "99999999") &&
         text_pty_write(master, search, sizeof(search) - 1U);
    if (ok) {
        *stage = 9U;
    }
    start = output->length;
    ok = ok && text_pty_write(master, "2n", 2U) && text_pty_wait_for(master, output, start, "ZZZZZZZZ") &&
         text_pty_write(master, "\nq", 2U);
    if (ok) {
        *stage = 10U;
    }

    int status = 0;
    const bool completed = ok && text_pty_wait_for_exit(child, master, output, &status);
    if (!completed) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
    }
    (void)text_pty_drain(master, output);
    (void)close(master);
    free(cache);
    free(config);
    free(path);
    return completed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static BacaTestResult test_utf8_markdown_and_empty_documents(void) {
    static const unsigned char text[] = {
        0xefU, 0xbbU, 0xbfU, 'F', 'i', 'r', 's', 't',  '\r', '\n', 'S', 'e', 'c', 'o', 'n',
        'd',   '\r',  'T',   'h', 'i', 'r', 'd', '\n', '\n', 'F',  'o', 'u', 'r', 't', 'h',
    };
    TEST_ASSERT(baca_test_write("text/sample.txt", text, sizeof(text)));
    char *path = baca_test_path("text/sample.txt");
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_TEXT);
    TEST_ASSERT_STR(document.metadata.title, "sample.txt");
    TEST_ASSERT_STR(document.metadata.format, "Plain Text");
    TEST_ASSERT_SIZE(document.block_count, 1U);
    TEST_ASSERT_SIZE(document.section_count, 1U);
    TEST_ASSERT_STR(document.blocks[0].value.text.text, "First\nSecond\nThird\n\nFourth");
    TEST_ASSERT(!document.blocks[0].value.text.preformatted);
    baca_document_close(&document);
    free(path);

    TEST_ASSERT(baca_test_write_text("text/readme.md", "# Heading\n\n**literal markdown**\n"));
    path = baca_test_path("text/readme.md");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_STR(document.metadata.format, "Markdown");
    TEST_ASSERT_STR(document.blocks[0].value.text.text, "# Heading\n\n**literal markdown**\n");
    baca_document_close(&document);
    free(path);

    TEST_ASSERT(baca_test_write("text/empty.txt", NULL, 0U));
    path = baca_test_path("text/empty.txt");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.block_count, 1U);
    TEST_ASSERT_STR(document.blocks[0].value.text.text, "");
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_utf16_byte_orders(void) {
    static const unsigned char little_endian[] = {
        0xffU, 0xfeU, 0x48U, 0x00U, 0xe9U, 0x00U, 0x0dU, 0x00U, 0x0aU, 0x00U, 0x16U, 0x4eU, 0x4cU, 0x75U,
    };
    static const unsigned char big_endian[] = {
        0xfeU, 0xffU, 0x00U, 0x48U, 0x00U, 0xe9U, 0x00U, 0x0aU, 0x4eU, 0x16U, 0x75U, 0x4cU,
    };
    static const char expected[] = "H\xc3\xa9\n\xe4\xb8\x96\xe7\x95\x8c";
    const struct {
        const char *name;
        const unsigned char *data;
        size_t length;
    } fixtures[] = {
        {.name = "text/little.txt", .data = little_endian, .length = sizeof(little_endian)},
        {.name = "text/big.txt", .data = big_endian, .length = sizeof(big_endian)},
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(fixtures); ++index) {
        TEST_ASSERT(baca_test_write(fixtures[index].name, fixtures[index].data, fixtures[index].length));
        char *path = baca_test_path(fixtures[index].name);
        TEST_ASSERT(path != NULL);
        BacaDocument document = {0};
        BacaError error = {0};
        TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
        TEST_ASSERT_STR(document.blocks[0].value.text.text, expected);
        baca_document_close(&document);
        free(path);
    }
    return BACA_TEST_PASS;
}

static BacaTestResult test_invalid_binary_and_oversized_text(void) {
    static const unsigned char invalid_utf8[] = {0xc3U, 0x28U};
    static const unsigned char embedded_nul[] = {'a', 0x00U, 'b'};
    static const unsigned char odd_utf16[] = {0xffU, 0xfeU, 0x41U};
    const struct {
        const char *name;
        const unsigned char *data;
        size_t length;
    } fixtures[] = {
        {.name = "text/invalid.txt", .data = invalid_utf8, .length = sizeof(invalid_utf8)},
        {.name = "text/nul.txt", .data = embedded_nul, .length = sizeof(embedded_nul)},
        {.name = "text/odd.txt", .data = odd_utf16, .length = sizeof(odd_utf16)},
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(fixtures); ++index) {
        TEST_ASSERT(baca_test_write(fixtures[index].name, fixtures[index].data, fixtures[index].length));
        char *path = baca_test_path(fixtures[index].name);
        TEST_ASSERT(path != NULL);
        BacaDocument document = {0};
        BacaError error = {0};
        TEST_ASSERT(!baca_document_open(&document, path, &error));
        TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
        free(path);
    }

    char *path = baca_test_path("text/oversized.txt");
    TEST_ASSERT(path != NULL);
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    TEST_ASSERT(descriptor >= 0);
    TEST_ASSERT(ftruncate(descriptor, (off_t)TEST_TEXT_MAX_BYTES + 1) == 0);
    TEST_ASSERT(close(descriptor) == 0);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_vim_counts_and_jump_history(void) {
    BacaString output = {0};
    unsigned stage = 0U;
    const bool ran = run_vim_navigation_pty(&output, &stage);
    const char *captured = output.data == NULL ? "(empty)" : output.data;
    const char *tail = output.data == NULL || output.length <= 800U ? captured : output.data + output.length - 800U;
    const BacaTestResult result =
        ran ? BACA_TEST_PASS
            : baca_test_fail_at(__FILE__, __LINE__,
                                "Vim navigation PTY failed at stage %u after %zu bytes; tail: %.800s", stage,
                                output.length, tail);
    baca_string_free(&output);
    return result;
}

const BacaTestCase *baca_text_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "utf8_markdown_and_empty_documents", .function = test_utf8_markdown_and_empty_documents},
        {.name = "utf16_byte_orders", .function = test_utf16_byte_orders},
        {.name = "invalid_binary_and_oversized_text", .function = test_invalid_binary_and_oversized_text},
        {.name = "vim_counts_and_jump_history", .function = test_vim_counts_and_jump_history},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
