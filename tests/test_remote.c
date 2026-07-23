#include "test_support.h"

#include "baca/remote.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static bool remote_write_all(int descriptor, const char *data, size_t length) {
    size_t offset = 0U;
    while (offset < length) {
        const ssize_t written = write(descriptor, data + offset, length - offset);
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

static void remote_server_respond(int client, const char *request) {
    static const char body[] = "remote text\n";
    if (strstr(request, "\r\nUser-Agent: mereader-tui/0.1.1\r\n") == NULL) {
        static const char response[] =
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)remote_write_all(client, response, sizeof(response) - 1U);
    } else if (strstr(request, "GET /plain ") != NULL) {
        static const char header[] =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 12\r\nConnection: close\r\n\r\n";
        (void)remote_write_all(client, header, sizeof(header) - 1U);
        (void)remote_write_all(client, body, sizeof(body) - 1U);
    } else if (strstr(request, "GET /redirect ") != NULL) {
        static const char response[] =
            "HTTP/1.1 302 Found\r\nLocation: /plain\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)remote_write_all(client, response, sizeof(response) - 1U);
    } else if (strstr(request, "GET /large ") != NULL) {
        char response[256] = {0};
        const int length = snprintf(response, sizeof(response),
                                    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                                    "Content-Length: %" PRIuMAX "\r\nConnection: close\r\n\r\n",
                                    (uintmax_t)BACA_REMOTE_MAX_BYTES + 1U);
        if (length > 0 && (size_t)length < sizeof(response)) {
            (void)remote_write_all(client, response, (size_t)length);
        }
    } else {
        static const char response[] =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)remote_write_all(client, response, sizeof(response) - 1U);
    }
}

static void remote_server_loop(int listener) {
    (void)signal(SIGPIPE, SIG_IGN);
    (void)alarm(30U);
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(0);
        }
        char request[4096] = {0};
        size_t length = 0U;
        while (length + 1U < sizeof(request)) {
            const ssize_t count = read(client, request + length, sizeof(request) - length - 1U);
            if (count > 0) {
                length += (size_t)count;
                request[length] = '\0';
                if (strstr(request, "\r\n\r\n") != NULL) {
                    break;
                }
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        remote_server_respond(client, request);
        (void)close(client);
    }
}

static pid_t remote_server_start(uint16_t *port) {
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        return -1;
    }
    const int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(0U),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 8) != 0) {
        (void)close(listener);
        return -1;
    }
    socklen_t address_length = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_length) != 0) {
        (void)close(listener);
        return -1;
    }
    *port = ntohs(address.sin_port);

    const pid_t child = fork();
    if (child == 0) {
        remote_server_loop(listener);
        _exit(0);
    }
    (void)close(listener);
    return child;
}

static void remote_server_stop(pid_t child) {
    if (child <= 0) {
        return;
    }
    (void)kill(child, SIGTERM);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
}

static bool remote_url(char output[256], uint16_t port, const char *path) {
    const int length = snprintf(output, 256U, "http://127.0.0.1:%u%s", (unsigned)port, path);
    return length > 0 && (size_t)length < 256U;
}

static BacaTestResult test_fetch_redirect_limits_and_offline_cache(void) {
    uint16_t port = 0U;
    const pid_t server = remote_server_start(&port);
    TEST_ASSERT(server > 0);

    char plain_url[256] = {0};
    char redirect_url[256] = {0};
    char large_url[256] = {0};
    char missing_url[256] = {0};
    TEST_ASSERT(remote_url(plain_url, port, "/plain#chapter"));
    TEST_ASSERT(remote_url(redirect_url, port, "/redirect"));
    TEST_ASSERT(remote_url(large_url, port, "/large"));
    TEST_ASSERT(remote_url(missing_url, port, "/missing"));

    BacaError error = {0};
    BacaRemoteFile file = {0};
    TEST_ASSERT_MSG(baca_remote_fetch(plain_url, &file, &error), "%s", error.message);
    TEST_ASSERT(strstr(file.url, "#") == NULL);
    TEST_ASSERT_STR(baca_path_extension(file.path), ".txt");
    BacaBuffer contents = {0};
    TEST_ASSERT_MSG(baca_read_file(file.path, &contents, &error), "%s", error.message);
    TEST_ASSERT_SIZE(contents.length, 12U);
    TEST_ASSERT(memcmp(contents.data, "remote text\n", 12U) == 0);
    baca_buffer_free(&contents);
    struct stat status;
    TEST_ASSERT(stat(file.path, &status) == 0 && S_ISREG(status.st_mode));
    TEST_ASSERT((status.st_mode & 0777U) == 0600U);
    char *cached_path = baca_strdup(file.path, &error);
    TEST_ASSERT_MSG(cached_path != NULL, "%s", error.message);
    baca_remote_file_free(&file);

    TEST_ASSERT_MSG(baca_remote_fetch(redirect_url, &file, &error), "%s", error.message);
    TEST_ASSERT_STR(baca_path_extension(file.path), ".txt");
    baca_remote_file_free(&file);

    TEST_ASSERT(!baca_remote_fetch(large_url, &file, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    baca_error_clear(&error);
    TEST_ASSERT(!baca_remote_fetch(missing_url, &file, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_EXTERNAL);
    baca_error_clear(&error);

    remote_server_stop(server);
    TEST_ASSERT_MSG(baca_remote_fetch(plain_url, &file, &error), "%s", error.message);
    TEST_ASSERT_STR(file.path, cached_path);
    baca_remote_file_free(&file);
    free(cached_path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_rejects_credentials_and_non_http_urls(void) {
    BacaRemoteFile file = {0};
    BacaError error = {0};
    TEST_ASSERT(baca_remote_is_url("HTTP://example.test/book.epub"));
    TEST_ASSERT(baca_remote_is_url("https://example.test/book.epub"));
    TEST_ASSERT(!baca_remote_is_url("ftp://example.test/book.epub"));
    TEST_ASSERT(!baca_remote_is_url("/books/http://example.epub"));
    TEST_ASSERT(!baca_remote_fetch("ftp://example.test/book.epub", &file, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
    baca_error_clear(&error);
    TEST_ASSERT(!baca_remote_fetch("https://user:secret@example.test/book.epub", &file, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_error_clear(&error);
    TEST_ASSERT(!baca_remote_fetch("https://[invalid", &file, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_remote_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "fetch_redirect_limits_and_offline_cache",
         .function = test_fetch_redirect_limits_and_offline_cache},
        {.name = "rejects_credentials_and_non_http_urls",
         .function = test_rejects_credentials_and_non_http_urls},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
