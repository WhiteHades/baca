#include "test_support.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char *test_root_path;
static bool cleanup_registered;
static char test_detail[1024];

static bool make_test_directory(const char *relative) {
    char *path = mereader_tui_test_path(relative);
    if (path == NULL) {
        return false;
    }
    MereaderTuiError error = {0};
    bool made = mereader_tui_mkdirs(path, &error);
    if (!made) {
        fprintf(stderr, "test setup: %s\n", error.message);
    }
    free(path);
    return made;
}

bool mereader_tui_test_support_init(void) {
    MereaderTuiError error = {0};
    if (!mereader_tui_remove_tree("build/test-tmp", &error) || !mereader_tui_mkdirs("build/test-tmp", &error)) {
        fprintf(stderr, "test setup: %s\n", error.message);
        return false;
    }

    char template[] = "build/test-tmp/run.XXXXXX";
    char *created = mkdtemp(template);
    if (created == NULL) {
        fprintf(stderr, "test setup: could not create local fixture root: %s\n", strerror(errno));
        return false;
    }
    test_root_path = mereader_tui_realpath(created, &error);
    if (test_root_path == NULL) {
        fprintf(stderr, "test setup: %s\n", error.message);
        MereaderTuiError cleanup_error = {0};
        (void)mereader_tui_remove_tree(created, &cleanup_error);
        return false;
    }
    if (!cleanup_registered) {
        if (atexit(mereader_tui_test_support_cleanup) != 0) {
            fprintf(stderr, "test setup: could not register fixture cleanup\n");
            mereader_tui_test_support_cleanup();
            return false;
        }
        cleanup_registered = true;
    }

    if (!make_test_directory("home") || !make_test_directory("xdg-config") ||
        !make_test_directory("xdg-cache") || !make_test_directory("tmp")) {
        mereader_tui_test_support_cleanup();
        return false;
    }
    char *home = mereader_tui_test_path("home");
    char *config = mereader_tui_test_path("xdg-config");
    char *cache = mereader_tui_test_path("xdg-cache");
    char *temporary = mereader_tui_test_path("tmp");
    bool paths_ready = home != NULL && config != NULL && cache != NULL && temporary != NULL;
    bool environment_ready = paths_ready && setenv("HOME", home, 1) == 0 &&
                             setenv("XDG_CONFIG_HOME", config, 1) == 0 &&
                             setenv("XDG_CACHE_HOME", cache, 1) == 0 && setenv("TMPDIR", temporary, 1) == 0;
    free(home);
    free(config);
    free(cache);
    free(temporary);
    if (!environment_ready) {
        fprintf(stderr, "test setup: could not isolate HOME/XDG/TMPDIR\n");
        mereader_tui_test_support_cleanup();
        return false;
    }
    return true;
}

void mereader_tui_test_support_cleanup(void) {
    if (test_root_path == NULL) {
        return;
    }
    MereaderTuiError error = {0};
    if (!mereader_tui_remove_tree(test_root_path, &error)) {
        fprintf(stderr, "test cleanup: %s\n", error.message);
    }
    free(test_root_path);
    test_root_path = NULL;
    if (rmdir("build/test-tmp") != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        fprintf(stderr, "test cleanup: could not remove build/test-tmp: %s\n", strerror(errno));
    }
}

const char *mereader_tui_test_root(void) {
    return test_root_path;
}

char *mereader_tui_test_path(const char *relative) {
    if (test_root_path == NULL || relative == NULL) {
        return NULL;
    }
    MereaderTuiError error = {0};
    char *path = mereader_tui_path_join(test_root_path, relative, &error);
    if (path == NULL) {
        fprintf(stderr, "test fixture path: %s\n", error.message);
    }
    return path;
}

bool mereader_tui_test_mkdir(const char *relative) {
    return make_test_directory(relative);
}

bool mereader_tui_test_write(const char *relative, const void *data, size_t length) {
    char *path = mereader_tui_test_path(relative);
    if (path == NULL) {
        return false;
    }
    MereaderTuiError error = {0};
    char *directory = mereader_tui_path_dirname(path, &error);
    bool written = directory != NULL && mereader_tui_mkdirs(directory, &error) &&
                   mereader_tui_write_file(path, data, length, &error);
    if (!written) {
        fprintf(stderr, "test fixture write: %s\n", error.message);
    }
    free(directory);
    free(path);
    return written;
}

bool mereader_tui_test_write_text(const char *relative, const char *text) {
    return text != NULL && mereader_tui_test_write(relative, text, strlen(text));
}

size_t mereader_tui_test_count_directories(const char *relative, const char *prefix) {
    char *path = mereader_tui_test_path(relative);
    if (path == NULL || prefix == NULL) {
        free(path);
        return SIZE_MAX;
    }
    DIR *directory = opendir(path);
    free(path);
    if (directory == NULL) {
        return errno == ENOENT ? 0U : SIZE_MAX;
    }

    size_t count = 0U;
    size_t prefix_length = strlen(prefix);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                count = SIZE_MAX;
            }
            break;
        }
        if (strncmp(entry->d_name, prefix, prefix_length) != 0) {
            continue;
        }
        int descriptor = dirfd(directory);
        struct stat status;
        if (fstatat(descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(status.st_mode)) {
            count++;
        }
    }
    if (closedir(directory) != 0) {
        return SIZE_MAX;
    }
    return count;
}

MereaderTuiTestResult mereader_tui_test_fail_at(const char *file, int line, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int prefix = snprintf(test_detail, sizeof(test_detail), "%s:%d: ", file, line);
    if (prefix >= 0 && (size_t)prefix < sizeof(test_detail)) {
        (void)vsnprintf(test_detail + (size_t)prefix, sizeof(test_detail) - (size_t)prefix, format, arguments);
    }
    va_end(arguments);
    return MEREADER_TUI_TEST_FAIL;
}

int mereader_tui_test_run(const MereaderTuiTestSuite *suites, size_t suite_count) {
    size_t passed = 0U;
    size_t failed = 0U;
    for (size_t suite_index = 0U; suite_index < suite_count; suite_index++) {
        const MereaderTuiTestSuite *suite = &suites[suite_index];
        for (size_t case_index = 0U; case_index < suite->count; case_index++) {
            const MereaderTuiTestCase *test = &suite->cases[case_index];
            test_detail[0] = '\0';
            MereaderTuiTestResult result = test->function();
            if (result == MEREADER_TUI_TEST_PASS) {
                printf("PASS %s.%s\n", suite->name, test->name);
                passed++;
            } else {
                printf("FAIL %s.%s: %s\n", suite->name, test->name,
                       test_detail[0] == '\0' ? "no diagnostic supplied" : test_detail);
                failed++;
            }
        }
    }
    printf("SUMMARY %zu passed, %zu failed\n", passed, failed);
    return failed == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
