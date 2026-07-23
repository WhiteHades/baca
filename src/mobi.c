#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mereader-tui/document_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MOBITOOL_OUTPUT_MAX (64U * 1024U)
#define MOBITOOL_EPUB_MAX (1024ULL * 1024ULL * 1024ULL)
#define MOBITOOL_TIMEOUT_MS 30000U
#define MOBITOOL_TERM_GRACE_MS 250U
#define MOBITOOL_POLL_SLICE_MS 100U

extern char **environ;

static void remove_temp_directory(const char *path) {
    if (path == NULL) {
        return;
    }
    MereaderTuiError ignored = {0};
    (void) mereader_tui_remove_tree(path, &ignored);
}

static bool make_private_temp(char **directory, MereaderTuiError *error) {
    char *path = mereader_tui_make_temp_directory(MEREADER_TUI_NAME "-mobi-", error);
    if (path == NULL) {
        return false;
    }
    struct stat stat;
    if (lstat(path, &stat) != 0 || !S_ISDIR(stat.st_mode) || stat.st_uid != geteuid()) {
        remove_temp_directory(path);
        free(path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not create a private MOBI extraction directory");
        return false;
    }
    if (chmod(path, S_IRWXU) != 0) {
        int saved_errno = errno;
        remove_temp_directory(path);
        free(path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not secure MOBI extraction directory: %s",
                       strerror(saved_errno));
        return false;
    }
    *directory = path;
    return true;
}

static bool add_spawn_action(int result, MereaderTuiError *error) {
    if (result == 0) {
        return true;
    }
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not configure mobitool process: %s", strerror(result));
    return false;
}

static bool move_descriptor_above_stdio(int *descriptor, MereaderTuiError *error) {
    if (*descriptor > STDERR_FILENO) {
        return true;
    }
    int moved = fcntl(*descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not secure a mobitool descriptor: %s", strerror(errno));
        return false;
    }
    close(*descriptor);
    *descriptor = moved;
    return true;
}

static bool monotonic_milliseconds(uint64_t *milliseconds, MereaderTuiError *error) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read the monotonic clock: %s", strerror(errno));
        return false;
    }
    if (now.tv_sec < 0 || now.tv_nsec < 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "monotonic clock returned a negative value");
        return false;
    }
    uint64_t seconds = (uint64_t) now.tv_sec;
    if (seconds > UINT64_MAX / 1000U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "monotonic clock value overflow");
        return false;
    }
    *milliseconds = seconds * 1000U + (uint64_t) now.tv_nsec / 1000000U;
    return true;
}

static unsigned mobitool_timeout_ms(void) {
    const char *configured = getenv("MEREADER_TUI_TEST_MOBITOOL_TIMEOUT_MS");
    if (configured == NULL || configured[0] == '\0') {
        return MOBITOOL_TIMEOUT_MS;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(configured, &end, 10);
    return errno == 0 && end != configured && *end == '\0' && parsed > 0U &&
                   parsed <= MOBITOOL_TIMEOUT_MS ?
               (unsigned)parsed :
               MOBITOOL_TIMEOUT_MS;
}

static bool capture_child_output(int descriptor, MereaderTuiString *output, bool *eof, MereaderTuiError *error) {
    char buffer[4096];
    while (true) {
        ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count == 0) {
            *eof = true;
            return true;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read mobitool diagnostics: %s", strerror(errno));
            return false;
        }
        if (output->length > MOBITOOL_OUTPUT_MAX ||
            (size_t) count > MOBITOOL_OUTPUT_MAX - output->length) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool exceeded the diagnostic output limit");
            return false;
        }
        if (!mereader_tui_string_append_n(output, buffer, (size_t) count, error)) {
            return false;
        }
    }
}

static bool mobitool_output_within_limit(const char *path, MereaderTuiError *error) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect mobitool output: %s", strerror(errno));
        return false;
    }
    if (!S_ISREG(status.st_mode)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool produced a non-regular EPUB output");
        return false;
    }
    if (status.st_size < 0 || (uintmax_t) status.st_size > MOBITOOL_EPUB_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool EPUB output exceeds the supported size limit");
        return false;
    }
    return true;
}

static void terminate_process_group(pid_t child, bool child_reaped, int *status) {
    (void) kill(-child, SIGTERM);
    uint64_t started = 0;
    MereaderTuiError ignored = {0};
    bool have_clock = monotonic_milliseconds(&started, &ignored);
    while (!child_reaped) {
        pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) {
            child_reaped = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            break;
        }
        uint64_t now = 0;
        if (!have_clock || !monotonic_milliseconds(&now, &ignored) ||
            now - started >= MOBITOOL_TERM_GRACE_MS) {
            break;
        }
        struct timespec pause = {.tv_nsec = 10000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        }
    }
    (void) kill(-child, SIGKILL);
    while (!child_reaped) {
        pid_t waited = waitpid(child, status, 0);
        if (waited == child || (waited < 0 && errno == ECHILD)) {
            child_reaped = true;
        } else if (waited < 0 && errno != EINTR) {
            break;
        }
    }
}

static bool run_mobitool(const char *path, const char *output_directory, const char *expected_output,
                         char **diagnostics, int *status, MereaderTuiError *error) {
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not create mobitool output pipe: %s", strerror(errno));
        return false;
    }
    if (!move_descriptor_above_stdio(&descriptors[0], error) ||
        !move_descriptor_above_stdio(&descriptors[1], error)) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    int read_flags = fcntl(descriptors[0], F_GETFL);
    if (read_flags < 0 || fcntl(descriptors[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not configure mobitool output pipe: %s", strerror(errno));
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    int null_descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_descriptor < 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not open /dev/null for mobitool: %s", strerror(errno));
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    if (!move_descriptor_above_stdio(&null_descriptor, error)) {
        close(null_descriptor);
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }

    posix_spawn_file_actions_t actions;
    int action_result = posix_spawn_file_actions_init(&actions);
    if (!add_spawn_action(action_result, error)) {
        close(null_descriptor);
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    bool actions_ready = add_spawn_action(posix_spawn_file_actions_adddup2(&actions, null_descriptor,
                                                                           STDIN_FILENO), error) &&
                          add_spawn_action(posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO),
                                           error) &&
                          add_spawn_action(posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO),
                                           error) &&
                          add_spawn_action(posix_spawn_file_actions_addclosefrom_np(&actions,
                                                                                    STDERR_FILENO + 1), error);
    if (!actions_ready) {
        posix_spawn_file_actions_destroy(&actions);
        close(null_descriptor);
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }

    posix_spawnattr_t attributes;
    int attribute_result = posix_spawnattr_init(&attributes);
    if (!add_spawn_action(attribute_result, error)) {
        posix_spawn_file_actions_destroy(&actions);
        close(null_descriptor);
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    short spawn_flags = (short) (POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    bool attributes_ready = add_spawn_action(posix_spawnattr_setpgroup(&attributes, 0), error) &&
                            add_spawn_action(posix_spawnattr_setsigmask(&attributes, &signal_mask), error) &&
                            add_spawn_action(posix_spawnattr_setflags(&attributes, spawn_flags), error);
    if (!attributes_ready) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(null_descriptor);
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }

    char *const arguments[] = {
        (char *) "mobitool",
        (char *) "-e",
        (char *) "-o",
        (char *) output_directory,
        (char *) path,
        NULL,
    };
    pid_t child = 0;
    int spawn_result = posix_spawnp(&child, "mobitool", &actions, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(null_descriptor);
    close(descriptors[1]);
    if (spawn_result != 0) {
        close(descriptors[0]);
        if (spawn_result == ENOENT) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                           "MOBI/AZW support requires mobitool from libmobi, but mobitool was not found in PATH");
        } else {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not start mobitool: %s", strerror(spawn_result));
        }
        return false;
    }

    MereaderTuiString captured = {0};
    int child_status = 0;
    bool child_reaped = false;
    bool pipe_eof = false;
    uint64_t started = 0;
    if (!monotonic_milliseconds(&started, error)) {
        close(descriptors[0]);
        terminate_process_group(child, child_reaped, &child_status);
        return false;
    }
    const unsigned timeout_ms = mobitool_timeout_ms();
    if (started > UINT64_MAX - timeout_ms) {
        close(descriptors[0]);
        terminate_process_group(child, child_reaped, &child_status);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "mobitool timeout value overflow");
        return false;
    }
    uint64_t deadline = started + timeout_ms;
    bool success = true;
    while (!child_reaped || !pipe_eof) {
        if (!pipe_eof && !capture_child_output(descriptors[0], &captured, &pipe_eof, error)) {
            success = false;
            break;
        }
        if (!mobitool_output_within_limit(expected_output, error)) {
            success = false;
            break;
        }
        if (!child_reaped) {
            pid_t waited = waitpid(child, &child_status, WNOHANG);
            if (waited == child) {
                child_reaped = true;
            } else if (waited < 0 && errno != EINTR) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not wait for mobitool: %s", strerror(errno));
                success = false;
                break;
            }
        }
        if (child_reaped && pipe_eof) {
            break;
        }

        uint64_t now = 0;
        if (!monotonic_milliseconds(&now, error)) {
            success = false;
            break;
        }
        if (now >= deadline) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool exceeded the %u ms timeout",
                           timeout_ms);
            success = false;
            break;
        }
        uint64_t remaining = deadline - now;
        int timeout = (int) (remaining < MOBITOOL_POLL_SLICE_MS ? remaining : MOBITOOL_POLL_SLICE_MS);
        struct pollfd poll_descriptor = {
            .fd = pipe_eof ? -1 : descriptors[0],
            .events = POLLIN | POLLHUP,
        };
        int polled;
        do {
            polled = poll(&poll_descriptor, 1, timeout);
        } while (polled < 0 && errno == EINTR);
        if (polled < 0 || (poll_descriptor.revents & POLLNVAL) != 0) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not poll mobitool diagnostics: %s",
                           polled < 0 ? strerror(errno) : "invalid descriptor");
            success = false;
            break;
        }
    }
    close(descriptors[0]);
    if (!success) {
        terminate_process_group(child, child_reaped, &child_status);
        mereader_tui_string_free(&captured);
        return false;
    }

    char *text = mereader_tui_string_take(&captured);
    if (text == NULL) {
        text = mereader_tui_strdup("", error);
        if (text == NULL) {
            return false;
        }
    }
    *diagnostics = text;
    *status = child_status;
    return true;
}

static char *mobitool_expected_output(const char *input_path, const char *output_directory,
                                      MereaderTuiError *error) {
    char *basename = mereader_tui_path_basename(input_path, error);
    if (basename == NULL) {
        return NULL;
    }
    char *extension = strrchr(basename, '.');
    if (extension != NULL) {
        *extension = '\0';
    }
    MereaderTuiString filename = {0};
    bool built = mereader_tui_string_append(&filename, basename, error) &&
                 mereader_tui_string_append(&filename, ".epub", error);
    free(basename);
    if (!built) {
        mereader_tui_string_free(&filename);
        return NULL;
    }
    char *name = mereader_tui_string_take(&filename);
    char *path = mereader_tui_path_join(output_directory, name, error);
    free(name);
    return path;
}

static bool expected_output_exists(const char *path, bool *exists, MereaderTuiError *error) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        if (errno == ENOENT) {
            *exists = false;
            return true;
        }
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect mobitool EPUB output: %s", strerror(errno));
        return false;
    }
    if (!S_ISREG(status.st_mode)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool produced a non-regular EPUB output");
        return false;
    }
    if (status.st_size < 0 || (uintmax_t) status.st_size > MOBITOOL_EPUB_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool EPUB output exceeds the supported size limit");
        return false;
    }
    *exists = true;
    return true;
}

static char *diagnostic_summary(const char *diagnostics, MereaderTuiError *error) {
    const char *start = diagnostics;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    size_t length = strlen(start);
    while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '\t' ||
                          start[length - 1] == '\r' || start[length - 1] == '\n')) {
        length--;
    }
    if (length > 300) {
        length = 300;
    }
    char *summary = mereader_tui_strndup(start, length, error);
    if (summary == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char value = (unsigned char) summary[index];
        if (value == '\r' || value == '\n' || value == '\t') {
            summary[index] = ' ';
        } else if (value == 0xc2U && index + 1U < length &&
                   (unsigned char)summary[index + 1U] >= 0x80U &&
                   (unsigned char)summary[index + 1U] <= 0x9fU) {
            summary[index] = '?';
            summary[++index] = '?';
        } else if (value < 0x20U || (value >= 0x7fU && value <= 0x9fU)) {
            summary[index] = '?';
        }
    }
    return summary;
}

static bool diagnostics_indicate_drm(const char *diagnostics) {
    return mereader_tui_contains_casefold(diagnostics, "document is encrypted") ||
           mereader_tui_contains_casefold(diagnostics, "encrypted document") ||
           mereader_tui_contains_casefold(diagnostics, "drm protected") ||
           mereader_tui_contains_casefold(diagnostics, "decryption key") ||
           mereader_tui_contains_casefold(diagnostics, "encryption key");
}

static bool diagnostics_indicate_replica(const char *diagnostics) {
    return mereader_tui_contains_casefold(diagnostics, "print replica") ||
           mereader_tui_contains_casefold(diagnostics, "replica book") || mereader_tui_contains_casefold(diagnostics, "azw4");
}

static void set_extraction_error(const char *diagnostics, int status, bool missing_epub, MereaderTuiError *error) {
    if (diagnostics_indicate_drm(diagnostics)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED,
                       "this MOBI/AZW book is DRM protected; %s cannot decrypt protected Kindle books",
                       MEREADER_TUI_NAME);
        return;
    }
    if (diagnostics_indicate_replica(diagnostics)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED,
                       "Kindle Print Replica/AZW4 books are not supported by the EPUB reader");
        return;
    }
    if (mereader_tui_contains_casefold(diagnostics, "unknown option") ||
        mereader_tui_contains_casefold(diagnostics, "invalid option")) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                       "mobitool was built without EPUB extraction support; install libmobi with XML writer support");
        return;
    }
    if (missing_epub) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                       "mobitool completed without producing the expected EPUB file");
        return;
    }

    MereaderTuiError summary_error = {0};
    char *summary = diagnostic_summary(diagnostics, &summary_error);
    if (summary == NULL) {
        if (error != NULL && mereader_tui_error_is_set(&summary_error)) {
            *error = summary_error;
        }
        return;
    }
    if (summary[0] == '\0') {
        free(summary);
        if (WIFSIGNALED(status)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool was terminated by signal %d", WTERMSIG(status));
        } else {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool failed with exit status %d",
                           WIFEXITED(status) ? WEXITSTATUS(status) : status);
        }
        return;
    }
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "mobitool extraction failed: %s", summary);
    free(summary);
}

static MereaderTuiDocumentFormat mobi_format(const MereaderTuiDocument *document, const char *path) {
    if (document->format == MEREADER_TUI_FORMAT_AZW) {
        return MEREADER_TUI_FORMAT_AZW;
    }
    const char *extension = mereader_tui_path_extension(path);
    return extension != NULL &&
                   (mereader_tui_casecmp(extension, ".azw") == 0 || mereader_tui_casecmp(extension, ".azw3") == 0 ||
                    mereader_tui_casecmp(extension, ".azw4") == 0) ?
               MEREADER_TUI_FORMAT_AZW :
               MEREADER_TUI_FORMAT_MOBI;
}

bool mereader_tui_mobi_open(MereaderTuiDocument *document, const char *path, MereaderTuiError *error) {
    if (document == NULL || path == NULL || path[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document and MOBI/AZW path are required");
        return false;
    }
    MereaderTuiDocumentFormat format = mobi_format(document, path);
    char *absolute_path = mereader_tui_realpath(path, error);
    if (absolute_path == NULL) {
        return false;
    }
    if (strlen(absolute_path) >= FILENAME_MAX) {
        free(absolute_path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "MOBI/AZW path is too long for mobitool");
        return false;
    }
    char *temporary_directory = NULL;
    if (!make_private_temp(&temporary_directory, error)) {
        free(absolute_path);
        return false;
    }
    char *expected_output = mobitool_expected_output(absolute_path, temporary_directory, error);
    if (expected_output == NULL) {
        remove_temp_directory(temporary_directory);
        free(temporary_directory);
        free(absolute_path);
        return false;
    }

    char *diagnostics = NULL;
    int status = 0;
    if (!run_mobitool(absolute_path, temporary_directory, expected_output, &diagnostics, &status, error)) {
        free(expected_output);
        remove_temp_directory(temporary_directory);
        free(temporary_directory);
        free(absolute_path);
        return false;
    }
    if (diagnostics_indicate_drm(diagnostics)) {
        set_extraction_error(diagnostics, status, false, error);
        free(expected_output);
        free(diagnostics);
        remove_temp_directory(temporary_directory);
        free(temporary_directory);
        free(absolute_path);
        return false;
    }

    bool output_exists = false;
    bool inspected = expected_output_exists(expected_output, &output_exists, error);
    bool exited_successfully = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!inspected || !exited_successfully || !output_exists) {
        if (inspected) {
            set_extraction_error(diagnostics, status, !output_exists, error);
        }
        free(expected_output);
        free(diagnostics);
        remove_temp_directory(temporary_directory);
        free(temporary_directory);
        free(absolute_path);
        return false;
    }

    // mobitool selects the KF8 half of a hybrid by default; passing -7 would regress AZW3 rendering.
    bool opened = mereader_tui_epub_open(document, expected_output, temporary_directory, error);
    free(expected_output);
    free(diagnostics);
    if (!opened) {
        remove_temp_directory(temporary_directory);
        free(temporary_directory);
        free(absolute_path);
        return false;
    }
    document->format = format;
    free(temporary_directory);
    free(absolute_path);
    return true;
}
