#include "test_support.h"

#include "mereader-tui/remote.h"

#include <arpa/inet.h>
#include <dirent.h>
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
#include <time.h>
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
                                    (uintmax_t)MEREADER_TUI_REMOTE_MAX_BYTES + 1U);
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

static bool remote_secure_url(char output[256], uint16_t port, const char *path) {
    const int length = snprintf(output, 256U, "https://127.0.0.1:%u%s", (unsigned)port, path);
    return length > 0 && (size_t)length < 256U;
}

static void remote_pause(void) {
    struct timespec pause = {.tv_nsec = 10000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
    }
}

static pid_t remote_tls_server_start(uint16_t *http_port, uint16_t *tls_port, char **port_file) {
    if (!mereader_tui_test_mkdir("remote-tls")) {
        return -1;
    }
    *port_file = mereader_tui_test_path("remote-tls/ports");
    if (*port_file == NULL) {
        return -1;
    }
    (void)unlink(*port_file);
    const pid_t child = fork();
    if (child == 0) {
        (void)execlp("python3", "python3", "tests/remote_tls_server.py", *port_file,
                     "tests/fixtures/remote-test-cert.pem", "tests/fixtures/remote-test-key.pem",
                     (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        free(*port_file);
        *port_file = NULL;
        return -1;
    }

    for (unsigned attempt = 0U; attempt < 300U; ++attempt) {
        MereaderTuiError error = {0};
        MereaderTuiBuffer ports = {0};
        if (mereader_tui_read_file(*port_file, &ports, &error) &&
            sscanf((const char *)ports.data, "%hu %hu", http_port, tls_port) == 2 &&
            *http_port != 0U && *tls_port != 0U) {
            mereader_tui_buffer_free(&ports);
            return child;
        }
        mereader_tui_buffer_free(&ports);
        int status = 0;
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            free(*port_file);
            *port_file = NULL;
            return -1;
        }
        remote_pause();
    }
    remote_server_stop(child);
    free(*port_file);
    *port_file = NULL;
    return -1;
}

static size_t remote_directory_entries(const char *path) {
    DIR *directory = opendir(path);
    if (directory == NULL) {
        return errno == ENOENT ? 0U : SIZE_MAX;
    }
    size_t count = 0U;
    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    return closedir(directory) == 0 ? count : SIZE_MAX;
}

static bool remote_rejected_without_cache(const char *url, const char *cache_directory,
                                          MereaderTuiError *error) {
    const size_t before = remote_directory_entries(cache_directory);
    MereaderTuiRemoteFile file = {0};
    const bool rejected = before != SIZE_MAX && !mereader_tui_remote_fetch(url, &file, error) &&
                          file.url == NULL && file.path == NULL &&
                          remote_directory_entries(cache_directory) == before;
    mereader_tui_remote_file_free(&file);
    return rejected;
}

static MereaderTuiTestResult test_fetch_redirect_limits_and_offline_cache(void) {
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

    MereaderTuiError error = {0};
    MereaderTuiRemoteFile file = {0};
    TEST_ASSERT_MSG(mereader_tui_remote_fetch(plain_url, &file, &error), "%s", error.message);
    TEST_ASSERT(strstr(file.url, "#") == NULL);
    TEST_ASSERT_STR(mereader_tui_path_extension(file.path), ".txt");
    MereaderTuiBuffer contents = {0};
    TEST_ASSERT_MSG(mereader_tui_read_file(file.path, &contents, &error), "%s", error.message);
    TEST_ASSERT_SIZE(contents.length, 12U);
    TEST_ASSERT(memcmp(contents.data, "remote text\n", 12U) == 0);
    mereader_tui_buffer_free(&contents);
    struct stat status;
    TEST_ASSERT(stat(file.path, &status) == 0 && S_ISREG(status.st_mode));
    TEST_ASSERT((status.st_mode & 0777U) == 0600U);
    char *cached_path = mereader_tui_strdup(file.path, &error);
    TEST_ASSERT_MSG(cached_path != NULL, "%s", error.message);
    mereader_tui_remote_file_free(&file);

    TEST_ASSERT_MSG(mereader_tui_remote_fetch(redirect_url, &file, &error), "%s", error.message);
    TEST_ASSERT_STR(mereader_tui_path_extension(file.path), ".txt");
    mereader_tui_remote_file_free(&file);

    TEST_ASSERT(!mereader_tui_remote_fetch(large_url, &file, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_fetch(missing_url, &file, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_EXTERNAL);
    mereader_tui_error_clear(&error);

    remote_server_stop(server);
    TEST_ASSERT_MSG(mereader_tui_remote_fetch(plain_url, &file, &error), "%s", error.message);
    TEST_ASSERT_STR(file.path, cached_path);
    mereader_tui_remote_file_free(&file);
    free(cached_path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_url_policy_rejects_credentials_and_unapproved_schemes(void) {
    MereaderTuiRemoteFile file = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(mereader_tui_remote_is_url("HTTP://example.test/book.epub"));
    TEST_ASSERT(mereader_tui_remote_is_url("https://example.test/book.epub"));
    TEST_ASSERT(!mereader_tui_remote_is_url("ftp://example.test/book.epub"));
    TEST_ASSERT(!mereader_tui_remote_is_url("/books/http://example.epub"));
    TEST_ASSERT(mereader_tui_remote_validate_url("HTTP://example.test/book.epub#chapter", &error));
    TEST_ASSERT(mereader_tui_remote_validate_url("https://example.test/book.epub", &error));
    TEST_ASSERT(!mereader_tui_remote_validate_url("file:///etc/passwd", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_validate_url("mailto:reader@example.test", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_validate_url("custom-handler:payload", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_validate_url("//example.test/book.epub", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_validate_url("https://user:secret@example.test/book.epub", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_validate_url("https://[invalid", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_fetch("ftp://example.test/book.epub", &file, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_fetch("https://user:secret@example.test/book.epub", &file, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_remote_fetch("https://[invalid", &file, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_tls_redirects_failures_and_partial_cleanup(void) {
    uint16_t http_port = 0U;
    uint16_t tls_port = 0U;
    char *port_file = NULL;
    const pid_t server = remote_tls_server_start(&http_port, &tls_port, &port_file);
    TEST_ASSERT(server > 0);

    char plain[256] = {0};
    char redirect[256] = {0};
    char downgrade[256] = {0};
    char credentials[256] = {0};
    char loop[256] = {0};
    char truncated[256] = {0};
    char interrupted[256] = {0};
    TEST_ASSERT(remote_secure_url(plain, tls_port, "/plain"));
    TEST_ASSERT(remote_secure_url(redirect, tls_port, "/redirect"));
    TEST_ASSERT(remote_secure_url(downgrade, tls_port, "/downgrade"));
    TEST_ASSERT(remote_secure_url(credentials, tls_port, "/credentials"));
    TEST_ASSERT(remote_secure_url(loop, tls_port, "/loop"));
    TEST_ASSERT(remote_secure_url(truncated, tls_port, "/truncated"));
    TEST_ASSERT(remote_secure_url(interrupted, tls_port, "/interrupted"));

    MereaderTuiError error = {0};
    char *cache_directory = mereader_tui_xdg_cache_path("downloads", &error);
    TEST_ASSERT_MSG(cache_directory != NULL, "%s", error.message);
    TEST_ASSERT(remote_rejected_without_cache(plain, cache_directory, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_EXTERNAL);
    mereader_tui_error_clear(&error);

    TEST_ASSERT(setenv("MEREADER_TUI_TEST_CA_BUNDLE", "tests/fixtures/remote-test-cert.pem", 1) == 0);
    MereaderTuiRemoteFile file = {0};
    TEST_ASSERT_MSG(mereader_tui_remote_fetch(plain, &file, &error), "%s", error.message);
    TEST_ASSERT_STR(mereader_tui_path_extension(file.path), ".txt");
    MereaderTuiBuffer contents = {0};
    TEST_ASSERT(mereader_tui_read_file(file.path, &contents, &error));
    TEST_ASSERT_SIZE(contents.length, sizeof("secure remote text\n") - 1U);
    TEST_ASSERT(memcmp(contents.data, "secure remote text\n", contents.length) == 0);
    mereader_tui_buffer_free(&contents);
    mereader_tui_remote_file_free(&file);

    TEST_ASSERT_MSG(mereader_tui_remote_fetch(redirect, &file, &error), "%s", error.message);
    mereader_tui_remote_file_free(&file);
    struct stat directory_status;
    TEST_ASSERT(stat(cache_directory, &directory_status) == 0 && S_ISDIR(directory_status.st_mode));
    TEST_ASSERT((directory_status.st_mode & 0777U) == 0700U);

    TEST_ASSERT(remote_rejected_without_cache(downgrade, cache_directory, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_UNSUPPORTED);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(remote_rejected_without_cache(credentials, cache_directory, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(remote_rejected_without_cache(loop, cache_directory, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_EXTERNAL);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(remote_rejected_without_cache(truncated, cache_directory, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_EXTERNAL);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(remote_rejected_without_cache(interrupted, cache_directory, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_EXTERNAL);

    TEST_ASSERT(unsetenv("MEREADER_TUI_TEST_CA_BUNDLE") == 0);
    remote_server_stop(server);
    (void)unlink(port_file);
    free(port_file);
    free(cache_directory);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_remote_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "fetch_redirect_limits_and_offline_cache",
         .function = test_fetch_redirect_limits_and_offline_cache},
        {.name = "url_policy_rejects_credentials_and_unapproved_schemes",
         .function = test_url_policy_rejects_credentials_and_unapproved_schemes},
        {.name = "tls_redirects_failures_and_partial_cleanup",
         .function = test_tls_redirects_failures_and_partial_cleanup},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
