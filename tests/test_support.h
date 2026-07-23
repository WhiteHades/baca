#pragma once

#include "mereader-tui/common.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef enum MereaderTuiTestResult {
    MEREADER_TUI_TEST_PASS = 0,
    MEREADER_TUI_TEST_FAIL,
} MereaderTuiTestResult;

typedef MereaderTuiTestResult (*MereaderTuiTestFunction)(void);

typedef struct MereaderTuiTestCase {
    const char *name;
    MereaderTuiTestFunction function;
} MereaderTuiTestCase;

typedef struct MereaderTuiTestSuite {
    const char *name;
    const MereaderTuiTestCase *cases;
    size_t count;
} MereaderTuiTestSuite;

[[nodiscard]] bool mereader_tui_test_support_init(void);
void mereader_tui_test_support_cleanup(void);
[[nodiscard]] const char *mereader_tui_test_root(void);
[[nodiscard]] char *mereader_tui_test_path(const char *relative);
[[nodiscard]] bool mereader_tui_test_mkdir(const char *relative);
[[nodiscard]] bool mereader_tui_test_write(const char *relative, const void *data, size_t length);
[[nodiscard]] bool mereader_tui_test_write_text(const char *relative, const char *text);
[[nodiscard]] size_t mereader_tui_test_count_directories(const char *relative, const char *prefix);

[[nodiscard]] MereaderTuiTestResult mereader_tui_test_fail_at(const char *file, int line, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
[[nodiscard]] int mereader_tui_test_run(const MereaderTuiTestSuite *suites, size_t suite_count);

[[nodiscard]] const MereaderTuiTestCase *mereader_tui_common_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_catalog_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_comic_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_config_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_database_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_document_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_fb2_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_graphics_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_library_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_library_shelf_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_layout_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_pdf_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_remote_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_search_test_cases(size_t *count);
[[nodiscard]] const MereaderTuiTestCase *mereader_tui_text_test_cases(size_t *count);
[[nodiscard]] int mereader_tui_image_pty_child(void);
[[nodiscard]] int mereader_tui_pdf_pty_child(void);

#define TEST_ASSERT(condition)                                                                                \
    do {                                                                                                      \
        if (!(condition)) {                                                                                   \
            return mereader_tui_test_fail_at(__FILE__, __LINE__, "assertion failed: %s", #condition);              \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_MSG(condition, ...)                                                                       \
    do {                                                                                                      \
        if (!(condition)) {                                                                                   \
            return mereader_tui_test_fail_at(__FILE__, __LINE__, __VA_ARGS__);                                       \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_SIZE(actual, expected)                                                                    \
    do {                                                                                                      \
        size_t mereader_tui_test_actual_ = (actual);                                                                  \
        size_t mereader_tui_test_expected_ = (expected);                                                              \
        if (mereader_tui_test_actual_ != mereader_tui_test_expected_) {                                                       \
            return mereader_tui_test_fail_at(__FILE__, __LINE__, "%s was %zu, expected %zu", #actual,             \
                                     mereader_tui_test_actual_, mereader_tui_test_expected_);                                 \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_INT(actual, expected)                                                                     \
    do {                                                                                                      \
        int mereader_tui_test_actual_ = (actual);                                                                     \
        int mereader_tui_test_expected_ = (expected);                                                                 \
        if (mereader_tui_test_actual_ != mereader_tui_test_expected_) {                                                       \
            return mereader_tui_test_fail_at(__FILE__, __LINE__, "%s was %d, expected %d", #actual,               \
                                     mereader_tui_test_actual_, mereader_tui_test_expected_);                                 \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_DOUBLE(actual, expected, tolerance)                                                       \
    do {                                                                                                      \
        double mereader_tui_test_actual_ = (actual);                                                                  \
        double mereader_tui_test_expected_ = (expected);                                                              \
        double mereader_tui_test_tolerance_ = (tolerance);                                                            \
        if (!isfinite(mereader_tui_test_actual_) ||                                                                   \
            fabs(mereader_tui_test_actual_ - mereader_tui_test_expected_) > mereader_tui_test_tolerance_) {                           \
            return mereader_tui_test_fail_at(__FILE__, __LINE__, "%s was %.17g, expected %.17g (+/- %.3g)",       \
                                     #actual, mereader_tui_test_actual_, mereader_tui_test_expected_, mereader_tui_test_tolerance_);   \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_STR(actual, expected)                                                                     \
    do {                                                                                                      \
        const char *mereader_tui_test_actual_ = (actual);                                                             \
        const char *mereader_tui_test_expected_ = (expected);                                                         \
        if (mereader_tui_test_actual_ == NULL || mereader_tui_test_expected_ == NULL ||                                       \
            strcmp(mereader_tui_test_actual_, mereader_tui_test_expected_) != 0) {                                           \
            return mereader_tui_test_fail_at(__FILE__, __LINE__, "%s was \"%s\", expected \"%s\"", #actual, \
                                     mereader_tui_test_actual_ == NULL ? "(null)" : mereader_tui_test_actual_,               \
                                     mereader_tui_test_expected_ == NULL ? "(null)" : mereader_tui_test_expected_);          \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_ERROR(error, expected)                                                                    \
    do {                                                                                                      \
        MereaderTuiErrorCode mereader_tui_test_actual_ = (error).code;                                                       \
        MereaderTuiErrorCode mereader_tui_test_expected_ = (expected);                                                       \
        if (mereader_tui_test_actual_ != mereader_tui_test_expected_) {                                                       \
            return mereader_tui_test_fail_at(__FILE__, __LINE__, "%s.code was %u, expected %u (%s)", #error,       \
                                     (unsigned)mereader_tui_test_actual_, (unsigned)mereader_tui_test_expected_,              \
                                     (error).message);                                                        \
        }                                                                                                     \
    } while (false)
