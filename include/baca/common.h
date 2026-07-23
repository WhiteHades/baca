#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BACA_NAME "mereader-tui"
#define BACA_VERSION "0.1.1"
#define BACA_ARRAY_LEN(values) (sizeof(values) / sizeof((values)[0]))

typedef enum BacaErrorCode : uint8_t {
    BACA_ERROR_NONE = 0,
    BACA_ERROR_ARGUMENT,
    BACA_ERROR_NOT_FOUND,
    BACA_ERROR_UNSUPPORTED,
    BACA_ERROR_CORRUPT,
    BACA_ERROR_IO,
    BACA_ERROR_MEMORY,
    BACA_ERROR_DATABASE,
    BACA_ERROR_EXTERNAL,
    BACA_ERROR_INTERNAL,
} BacaErrorCode;

typedef struct BacaError {
    BacaErrorCode code;
    char message[512];
} BacaError;

typedef struct BacaBuffer {
    unsigned char *data;
    size_t length;
    size_t capacity;
} BacaBuffer;

typedef struct BacaString {
    char *data;
    size_t length;
    size_t capacity;
} BacaString;

[[nodiscard]] bool baca_error_is_set(const BacaError *error);
void baca_error_clear(BacaError *error);
void baca_error_set(BacaError *error, BacaErrorCode code, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

[[nodiscard]] void *baca_reallocarray(void *pointer, size_t count, size_t size, BacaError *error);
[[nodiscard]] char *baca_strdup(const char *value, BacaError *error);
[[nodiscard]] char *baca_strndup(const char *value, size_t length, BacaError *error);
[[nodiscard]] void *baca_array_reserve(void *items, size_t *capacity, size_t item_size, size_t needed,
                                       BacaError *error);

void baca_buffer_free(BacaBuffer *buffer);
[[nodiscard]] bool baca_buffer_append(BacaBuffer *buffer, const void *data, size_t length, BacaError *error);

void baca_string_free(BacaString *string);
[[nodiscard]] bool baca_string_append(BacaString *string, const char *value, BacaError *error);
[[nodiscard]] bool baca_string_append_n(BacaString *string, const char *value, size_t length, BacaError *error);
[[nodiscard]] bool baca_string_append_char(BacaString *string, char value, BacaError *error);
[[nodiscard]] char *baca_string_take(BacaString *string);

[[nodiscard]] bool baca_read_file(const char *path, BacaBuffer *output, BacaError *error);
[[nodiscard]] bool baca_write_file(const char *path, const void *data, size_t length, BacaError *error);
[[nodiscard]] bool baca_mkdirs(const char *path, BacaError *error);
[[nodiscard]] char *baca_realpath(const char *path, BacaError *error);
[[nodiscard]] char *baca_path_join(const char *left, const char *right, BacaError *error);
[[nodiscard]] char *baca_path_dirname(const char *path, BacaError *error);
[[nodiscard]] char *baca_path_basename(const char *path, BacaError *error);
[[nodiscard]] char *baca_path_stem(const char *path, BacaError *error);
[[nodiscard]] const char *baca_path_extension(const char *path);
[[nodiscard]] char *baca_uri_decode(const char *value, BacaError *error);
[[nodiscard]] char *baca_uri_resolve(const char *base, const char *reference, BacaError *error);
[[nodiscard]] bool baca_is_external_uri(const char *value);
[[nodiscard]] bool baca_file_exists(const char *path);
[[nodiscard]] bool baca_directory_exists(const char *path);
[[nodiscard]] bool baca_remove_tree(const char *path, BacaError *error);
[[nodiscard]] char *baca_make_temp_directory(const char *prefix, BacaError *error);
[[nodiscard]] char *baca_xdg_config_path(const char *filename, BacaError *error);
[[nodiscard]] char *baca_xdg_cache_path(const char *filename, BacaError *error);
[[nodiscard]] char *baca_now_iso8601(BacaError *error);
[[nodiscard]] size_t baca_utf8_width(const char *text, size_t length);
[[nodiscard]] size_t baca_utf8_next(const char *text, size_t length, size_t offset, int *columns);
[[nodiscard]] int baca_casecmp(const char *left, const char *right);
[[nodiscard]] bool baca_contains_casefold(const char *text, const char *query);
