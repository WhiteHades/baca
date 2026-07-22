#pragma once

#include "baca/common.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef enum BacaTestResult {
    BACA_TEST_PASS = 0,
    BACA_TEST_FAIL,
    BACA_TEST_SKIP,
} BacaTestResult;

typedef BacaTestResult (*BacaTestFunction)(void);

typedef struct BacaTestCase {
    const char *name;
    BacaTestFunction function;
} BacaTestCase;

typedef struct BacaTestSuite {
    const char *name;
    const BacaTestCase *cases;
    size_t count;
} BacaTestSuite;

[[nodiscard]] bool baca_test_support_init(void);
void baca_test_support_cleanup(void);
[[nodiscard]] const char *baca_test_root(void);
[[nodiscard]] char *baca_test_path(const char *relative);
[[nodiscard]] bool baca_test_mkdir(const char *relative);
[[nodiscard]] bool baca_test_write(const char *relative, const void *data, size_t length);
[[nodiscard]] bool baca_test_write_text(const char *relative, const char *text);
[[nodiscard]] size_t baca_test_count_directories(const char *relative, const char *prefix);

[[nodiscard]] BacaTestResult baca_test_fail_at(const char *file, int line, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
[[nodiscard]] BacaTestResult baca_test_skip(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
[[nodiscard]] int baca_test_run(const BacaTestSuite *suites, size_t suite_count);

[[nodiscard]] const BacaTestCase *baca_common_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_catalog_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_comic_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_config_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_database_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_document_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_fb2_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_graphics_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_library_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_layout_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_pdf_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_remote_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_search_test_cases(size_t *count);
[[nodiscard]] const BacaTestCase *baca_text_test_cases(size_t *count);
[[nodiscard]] int baca_image_pty_child(void);
[[nodiscard]] int baca_pdf_pty_child(void);

#define TEST_ASSERT(condition)                                                                                \
    do {                                                                                                      \
        if (!(condition)) {                                                                                   \
            return baca_test_fail_at(__FILE__, __LINE__, "assertion failed: %s", #condition);              \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_MSG(condition, ...)                                                                       \
    do {                                                                                                      \
        if (!(condition)) {                                                                                   \
            return baca_test_fail_at(__FILE__, __LINE__, __VA_ARGS__);                                       \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_SIZE(actual, expected)                                                                    \
    do {                                                                                                      \
        size_t baca_test_actual_ = (actual);                                                                  \
        size_t baca_test_expected_ = (expected);                                                              \
        if (baca_test_actual_ != baca_test_expected_) {                                                       \
            return baca_test_fail_at(__FILE__, __LINE__, "%s was %zu, expected %zu", #actual,             \
                                     baca_test_actual_, baca_test_expected_);                                 \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_INT(actual, expected)                                                                     \
    do {                                                                                                      \
        int baca_test_actual_ = (actual);                                                                     \
        int baca_test_expected_ = (expected);                                                                 \
        if (baca_test_actual_ != baca_test_expected_) {                                                       \
            return baca_test_fail_at(__FILE__, __LINE__, "%s was %d, expected %d", #actual,               \
                                     baca_test_actual_, baca_test_expected_);                                 \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_DOUBLE(actual, expected, tolerance)                                                       \
    do {                                                                                                      \
        double baca_test_actual_ = (actual);                                                                  \
        double baca_test_expected_ = (expected);                                                              \
        double baca_test_tolerance_ = (tolerance);                                                            \
        if (!isfinite(baca_test_actual_) ||                                                                   \
            fabs(baca_test_actual_ - baca_test_expected_) > baca_test_tolerance_) {                           \
            return baca_test_fail_at(__FILE__, __LINE__, "%s was %.17g, expected %.17g (+/- %.3g)",       \
                                     #actual, baca_test_actual_, baca_test_expected_, baca_test_tolerance_);   \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_STR(actual, expected)                                                                     \
    do {                                                                                                      \
        const char *baca_test_actual_ = (actual);                                                             \
        const char *baca_test_expected_ = (expected);                                                         \
        if (baca_test_actual_ == NULL || baca_test_expected_ == NULL ||                                       \
            strcmp(baca_test_actual_, baca_test_expected_) != 0) {                                           \
            return baca_test_fail_at(__FILE__, __LINE__, "%s was \"%s\", expected \"%s\"", #actual, \
                                     baca_test_actual_ == NULL ? "(null)" : baca_test_actual_,               \
                                     baca_test_expected_ == NULL ? "(null)" : baca_test_expected_);          \
        }                                                                                                     \
    } while (false)

#define TEST_ASSERT_ERROR(error, expected)                                                                    \
    do {                                                                                                      \
        BacaErrorCode baca_test_actual_ = (error).code;                                                       \
        BacaErrorCode baca_test_expected_ = (expected);                                                       \
        if (baca_test_actual_ != baca_test_expected_) {                                                       \
            return baca_test_fail_at(__FILE__, __LINE__, "%s.code was %u, expected %u (%s)", #error,       \
                                     (unsigned)baca_test_actual_, (unsigned)baca_test_expected_,              \
                                     (error).message);                                                        \
        }                                                                                                     \
    } while (false)
