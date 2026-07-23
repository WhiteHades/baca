#include "mereader-tui/remote.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <glib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEREADER_TUI_REMOTE_MAX_URL_BYTES 8192U

typedef struct MereaderTuiRemoteWriter {
    int descriptor;
    size_t length;
    int saved_errno;
    bool exceeded;
} MereaderTuiRemoteWriter;

static pthread_once_t mereader_tui_remote_curl_once = PTHREAD_ONCE_INIT;
static CURLcode mereader_tui_remote_curl_status = CURLE_FAILED_INIT;

static const char *const MEREADER_TUI_REMOTE_EXTENSIONS[] = {
    ".epub", ".epub3", ".mobi", ".prc", ".azw",  ".azw3", ".azw4", ".pdf",
    ".cbz",  ".cbr",   ".cb7",  ".png", ".jpg",  ".jpeg", ".gif",  ".webp",
    ".bmp",  ".svg",   ".txt",  ".md",  ".fb2",  ".download",
};

bool mereader_tui_remote_is_url(const char *value) {
    return value != nullptr &&
           (g_ascii_strncasecmp(value, "http://", 7U) == 0 || g_ascii_strncasecmp(value, "https://", 8U) == 0);
}

static void mereader_tui_remote_curl_initialize(void) {
    mereader_tui_remote_curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

void mereader_tui_remote_file_free(MereaderTuiRemoteFile *file) {
    if (file == nullptr) {
        return;
    }
    free(file->url);
    free(file->path);
    *file = (MereaderTuiRemoteFile){0};
}

static bool mereader_tui_remote_output_empty(const MereaderTuiRemoteFile *file) {
    return file != nullptr && file->url == nullptr && file->path == nullptr;
}

static bool mereader_tui_remote_normalize_url(const char *input, char **normalized, char **scheme,
                                      char **path, MereaderTuiError *error) {
    if (input == nullptr || input[0] == '\0' || strlen(input) > MEREADER_TUI_REMOTE_MAX_URL_BYTES) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid remote URL");
        return false;
    }
    const char *colon = strchr(input, ':');
    const char *separator = strpbrk(input, "/?#");
    if (colon != nullptr && (separator == nullptr || colon < separator)) {
        const size_t scheme_length = (size_t)(colon - input);
        const bool http = scheme_length == 4U && g_ascii_strncasecmp(input, "http", 4U) == 0;
        const bool https = scheme_length == 5U && g_ascii_strncasecmp(input, "https", 5U) == 0;
        if (!http && !https) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED,
                           "only HTTP and HTTPS URLs are supported");
            return false;
        }
    }

    CURLU *url = curl_url();
    if (url == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate URL parser");
        return false;
    }
    CURLUcode status = curl_url_set(url, CURLUPART_URL, input, CURLU_NON_SUPPORT_SCHEME);
    char *curl_scheme = nullptr;
    char *host = nullptr;
    char *user = nullptr;
    char *password = nullptr;
    if (status == CURLUE_OK) {
        status = curl_url_get(url, CURLUPART_SCHEME, &curl_scheme, 0U);
    }
    if (status == CURLUE_OK && mereader_tui_casecmp(curl_scheme, "http") != 0 &&
        mereader_tui_casecmp(curl_scheme, "https") != 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED,
                       "only HTTP and HTTPS URLs are supported");
        goto failure;
    }
    if (status == CURLUE_OK) {
        status = curl_url_get(url, CURLUPART_HOST, &host, 0U);
    }
    const bool has_user = curl_url_get(url, CURLUPART_USER, &user, 0U) == CURLUE_OK;
    const bool has_password = curl_url_get(url, CURLUPART_PASSWORD, &password, 0U) == CURLUE_OK;
    if (status != CURLUE_OK || curl_scheme == nullptr || host == nullptr || host[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid remote URL: %s",
                       status == CURLUE_OK ? "missing host" : curl_url_strerror(status));
        goto failure;
    }
    if (has_user || has_password) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "URLs containing credentials are not supported");
        goto failure;
    }
    if (curl_url_set(url, CURLUPART_FRAGMENT, nullptr, 0U) != CURLUE_OK) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "could not remove URL fragment");
        goto failure;
    }

    char *curl_normalized = nullptr;
    char *curl_path = nullptr;
    status = curl_url_get(url, CURLUPART_URL, &curl_normalized, CURLU_NO_DEFAULT_PORT);
    if (status == CURLUE_OK) {
        status = curl_url_get(url, CURLUPART_PATH, &curl_path, 0U);
    }
    if (status != CURLUE_OK || curl_normalized == nullptr || curl_path == nullptr) {
        curl_free(curl_normalized);
        curl_free(curl_path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "could not normalize remote URL: %s", curl_url_strerror(status));
        goto failure;
    }

    *normalized = mereader_tui_strdup(curl_normalized, error);
    *scheme = mereader_tui_strdup(curl_scheme, error);
    *path = mereader_tui_strdup(curl_path, error);
    curl_free(curl_normalized);
    curl_free(curl_path);
    curl_free(curl_scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(url);
    if (*normalized == nullptr || *scheme == nullptr || *path == nullptr) {
        free(*normalized);
        free(*scheme);
        free(*path);
        *normalized = nullptr;
        *scheme = nullptr;
        *path = nullptr;
        return false;
    }
    return true;

failure:
    curl_free(curl_scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(url);
    return false;
}

bool mereader_tui_remote_validate_url(const char *url, MereaderTuiError *error) {
    mereader_tui_error_clear(error);
    char *normalized = nullptr;
    char *scheme = nullptr;
    char *path = nullptr;
    const bool valid = mereader_tui_remote_normalize_url(url, &normalized, &scheme, &path, error);
    free(normalized);
    free(scheme);
    free(path);
    return valid;
}

static const char *mereader_tui_remote_supported_extension(const char *path) {
    const char *extension = mereader_tui_path_extension(path);
    for (size_t index = 0U; index + 1U < MEREADER_TUI_ARRAY_LEN(MEREADER_TUI_REMOTE_EXTENSIONS); ++index) {
        if (mereader_tui_casecmp(extension, MEREADER_TUI_REMOTE_EXTENSIONS[index]) == 0) {
            return MEREADER_TUI_REMOTE_EXTENSIONS[index];
        }
    }
    return nullptr;
}

static char *mereader_tui_remote_cache_path(const char *directory, const char *hash, const char *extension,
                                    MereaderTuiError *error) {
    char filename[96] = {0};
    const int length = snprintf(filename, sizeof(filename), "%s%s", hash, extension);
    if (length <= 0 || (size_t)length >= sizeof(filename)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "could not build remote cache filename");
        return nullptr;
    }
    return mereader_tui_path_join(directory, filename, error);
}

static bool mereader_tui_remote_cached_file(const char *path, char **resolved, bool *found, MereaderTuiError *error) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            *found = false;
            return true;
        }
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect remote cache '%s': %s", path,
                       strerror(saved_errno));
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 || (uintmax_t)status.st_size > MEREADER_TUI_REMOTE_MAX_BYTES) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "remote cache entry is invalid: %s", path);
        return false;
    }
    if (chmod(path, 0600) != 0) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not secure remote cache '%s': %s", path,
                       strerror(saved_errno));
        return false;
    }
    *resolved = mereader_tui_realpath(path, error);
    *found = *resolved != nullptr;
    return *found;
}

static bool mereader_tui_remote_find_cached(const char *directory, const char *hash, const char *preferred,
                                    char **path, bool *found, MereaderTuiError *error) {
    *found = false;
    if (preferred != nullptr) {
        char *candidate = mereader_tui_remote_cache_path(directory, hash, preferred, error);
        if (candidate == nullptr) {
            return false;
        }
        bool candidate_found = false;
        const bool checked = mereader_tui_remote_cached_file(candidate, path, &candidate_found, error);
        free(candidate);
        if (!checked || candidate_found) {
            *found = candidate_found;
            return checked;
        }
    }
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(MEREADER_TUI_REMOTE_EXTENSIONS); ++index) {
        const char *extension = MEREADER_TUI_REMOTE_EXTENSIONS[index];
        if (preferred != nullptr && strcmp(extension, preferred) == 0) {
            continue;
        }
        char *candidate = mereader_tui_remote_cache_path(directory, hash, extension, error);
        if (candidate == nullptr) {
            return false;
        }
        bool candidate_found = false;
        const bool checked = mereader_tui_remote_cached_file(candidate, path, &candidate_found, error);
        free(candidate);
        if (!checked || candidate_found) {
            *found = candidate_found;
            return checked;
        }
    }
    return true;
}

static size_t mereader_tui_remote_write(char *data, size_t size, size_t count, void *user_data) {
    MereaderTuiRemoteWriter *writer = user_data;
    if (size != 0U && count > SIZE_MAX / size) {
        writer->exceeded = true;
        return CURL_WRITEFUNC_ERROR;
    }
    const size_t length = size * count;
    if (length > MEREADER_TUI_REMOTE_MAX_BYTES - writer->length) {
        writer->exceeded = true;
        return CURL_WRITEFUNC_ERROR;
    }
    size_t offset = 0U;
    while (offset < length) {
        const ssize_t written = write(writer->descriptor, data + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            writer->saved_errno = written == 0 ? EIO : errno;
            return CURL_WRITEFUNC_ERROR;
        }
    }
    writer->length += length;
    return length;
}

static const char *mereader_tui_remote_content_extension(const char *content_type) {
    if (content_type == nullptr) {
        return nullptr;
    }
    static const struct {
        const char *content_type;
        const char *extension;
    } types[] = {
        {"application/epub+zip", ".epub"},
        {"application/pdf", ".pdf"},
        {"application/x-mobipocket-ebook", ".mobi"},
        {"application/vnd.comicbook+zip", ".cbz"},
        {"application/vnd.comicbook-rar", ".cbr"},
        {"image/png", ".png"},
        {"image/jpeg", ".jpg"},
        {"image/gif", ".gif"},
        {"image/webp", ".webp"},
        {"image/bmp", ".bmp"},
        {"image/svg+xml", ".svg"},
        {"text/markdown", ".md"},
        {"text/plain", ".txt"},
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(types); ++index) {
        const size_t length = strlen(types[index].content_type);
        if (g_ascii_strncasecmp(content_type, types[index].content_type, length) == 0 &&
            (content_type[length] == '\0' || content_type[length] == ';' || content_type[length] == ' ')) {
            return types[index].extension;
        }
    }
    return nullptr;
}

static const char *mereader_tui_remote_effective_extension(const char *effective_url) {
    if (effective_url == nullptr) {
        return nullptr;
    }
    CURLU *url = curl_url();
    if (url == nullptr || curl_url_set(url, CURLUPART_URL, effective_url, 0U) != CURLUE_OK) {
        curl_url_cleanup(url);
        return nullptr;
    }
    char *path = nullptr;
    const char *extension = nullptr;
    if (curl_url_get(url, CURLUPART_PATH, &path, 0U) == CURLUE_OK) {
        extension = mereader_tui_remote_supported_extension(path);
    }
    static _Thread_local char result[16];
    if (extension != nullptr) {
        (void)snprintf(result, sizeof(result), "%s", extension);
    }
    curl_free(path);
    curl_url_cleanup(url);
    return extension == nullptr ? nullptr : result;
}

static bool mereader_tui_remote_is_redirect(long status) {
    return status == 301L || status == 302L || status == 303L || status == 307L || status == 308L;
}

static bool mereader_tui_remote_download(const char *url, const char *scheme, const char *directory, const char *hash,
                                 const char *preferred_extension, char **path, MereaderTuiError *error) {
    char *template = mereader_tui_path_join(directory, ".download-XXXXXX", error);
    if (template == nullptr) {
        return false;
    }
    int descriptor = mkstemp(template);
    if (descriptor < 0) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not create remote cache file: %s", strerror(saved_errno));
        free(template);
        return false;
    }
    (void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    if (fchmod(descriptor, 0600) != 0) {
        const int saved_errno = errno;
        (void)close(descriptor);
        (void)unlink(template);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not secure remote cache file: %s", strerror(saved_errno));
        free(template);
        return false;
    }

    (void)pthread_once(&mereader_tui_remote_curl_once, mereader_tui_remote_curl_initialize);
    if (mereader_tui_remote_curl_status != CURLE_OK) {
        (void)close(descriptor);
        (void)unlink(template);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not initialize libcurl: %s",
                       curl_easy_strerror(mereader_tui_remote_curl_status));
        free(template);
        return false;
    }
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        (void)close(descriptor);
        (void)unlink(template);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate HTTP client");
        free(template);
        return false;
    }

    MereaderTuiRemoteWriter writer = {.descriptor = descriptor};
    char curl_error[CURL_ERROR_SIZE] = {0};
    CURLcode status = CURLE_OK;
    const char *ca_bundle = getenv("MEREADER_TUI_TEST_CA_BUNDLE");
    char *current_url = nullptr;
    if ((status = curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https")) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                                   (curl_off_t)MEREADER_TUI_REMOTE_MAX_BYTES)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "")) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_USERAGENT, MEREADER_TUI_NAME "/" MEREADER_TUI_VERSION)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mereader_tui_remote_write)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer)) != CURLE_OK) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not configure HTTP request: %s",
                       curl_easy_strerror(status));
        goto failure;
    }
    if (ca_bundle != nullptr && ca_bundle[0] != '\0' &&
        (status = curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle)) != CURLE_OK) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not configure test CA bundle: %s",
                       curl_easy_strerror(status));
        goto failure;
    }

    current_url = mereader_tui_strdup(url, error);
    if (current_url == nullptr) {
        goto failure;
    }
    const bool initial_https = mereader_tui_casecmp(scheme, "https") == 0;
    char *content_type = nullptr;
    for (unsigned redirects = 0U;;) {
        if (ftruncate(descriptor, 0) != 0 || lseek(descriptor, 0, SEEK_SET) < 0) {
            const int saved_errno = errno;
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not reset remote cache file: %s",
                           strerror(saved_errno));
            goto failure;
        }
        writer.length = 0U;
        writer.saved_errno = 0;
        writer.exceeded = false;
        curl_error[0] = '\0';
        if ((status = curl_easy_setopt(curl, CURLOPT_URL, current_url)) != CURLE_OK) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not configure HTTP request: %s",
                           curl_easy_strerror(status));
            goto failure;
        }

        status = curl_easy_perform(curl);
        if (writer.exceeded || status == CURLE_FILESIZE_EXCEEDED) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                           "remote document exceeds the 256 MiB download limit");
            goto failure;
        }
        if (writer.saved_errno != 0) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not write remote cache file: %s",
                           strerror(writer.saved_errno));
            goto failure;
        }
        if (status != CURLE_OK) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not download '%s': %s", current_url,
                           curl_error[0] != '\0' ? curl_error : curl_easy_strerror(status));
            goto failure;
        }

        long response_status = 0L;
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_status);
        if (mereader_tui_remote_is_redirect(response_status)) {
            char *redirect_url = nullptr;
            (void)curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect_url);
            if (redirect_url == nullptr || redirect_url[0] == '\0') {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                               "remote server returned a redirect without a location");
                goto failure;
            }
            if (redirects >= 5U) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                               "remote document exceeded the 5 redirect limit");
                goto failure;
            }

            char *normalized = nullptr;
            char *redirect_scheme = nullptr;
            char *redirect_path = nullptr;
            if (!mereader_tui_remote_normalize_url(redirect_url, &normalized, &redirect_scheme,
                                                   &redirect_path, error)) {
                goto failure;
            }
            if (initial_https && mereader_tui_casecmp(redirect_scheme, "https") != 0) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED,
                               "HTTPS downloads cannot redirect to HTTP");
                free(normalized);
                free(redirect_scheme);
                free(redirect_path);
                goto failure;
            }
            free(redirect_scheme);
            free(redirect_path);
            free(current_url);
            current_url = normalized;
            ++redirects;
            continue;
        }
        if (response_status < 200L || response_status >= 300L) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                           "remote server returned HTTP status %ld", response_status);
            goto failure;
        }
        (void)curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
        break;
    }

    const char *extension = preferred_extension;
    if (extension == nullptr) {
        extension = mereader_tui_remote_effective_extension(current_url);
    }
    if (extension == nullptr) {
        extension = mereader_tui_remote_content_extension(content_type);
    }
    if (extension == nullptr) {
        extension = ".download";
    }
    char *final_path = mereader_tui_remote_cache_path(directory, hash, extension, error);
    if (final_path == nullptr) {
        goto failure;
    }
    if (fsync(descriptor) != 0) {
        const int saved_errno = errno;
        free(final_path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not finish remote cache file: %s", strerror(saved_errno));
        goto failure;
    }
    if (close(descriptor) != 0) {
        const int saved_errno = errno;
        descriptor = -1;
        free(final_path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close remote cache file: %s", strerror(saved_errno));
        goto failure;
    }
    descriptor = -1;
    if (rename(template, final_path) != 0) {
        const int saved_errno = errno;
        free(final_path);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not publish remote cache file: %s", strerror(saved_errno));
        goto failure;
    }
    free(current_url);
    free(template);
    curl_easy_cleanup(curl);
    *path = final_path;
    return true;

failure:
    free(current_url);
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    (void)unlink(template);
    free(template);
    curl_easy_cleanup(curl);
    return false;
}

bool mereader_tui_remote_fetch(const char *url, MereaderTuiRemoteFile *file, MereaderTuiError *error) {
    mereader_tui_error_clear(error);
    if (!mereader_tui_remote_output_empty(file)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "remote file output must be zero-initialized");
        return false;
    }

    MereaderTuiRemoteFile result = {0};
    char *scheme = nullptr;
    char *url_path = nullptr;
    if (!mereader_tui_remote_normalize_url(url, &result.url, &scheme, &url_path, error)) {
        return false;
    }
    const char *preferred_extension = mereader_tui_remote_supported_extension(url_path);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, result.url, -1);
    if (hash == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not hash remote URL");
        goto failure;
    }
    char *directory = mereader_tui_xdg_cache_path("downloads", error);
    if (directory == nullptr || !mereader_tui_mkdirs(directory, error)) {
        free(directory);
        g_free(hash);
        goto failure;
    }
    if (chmod(directory, 0700) != 0) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not secure remote cache directory: %s", strerror(saved_errno));
        free(directory);
        g_free(hash);
        goto failure;
    }

    bool found = false;
    bool success = mereader_tui_remote_find_cached(directory, hash, preferred_extension, &result.path, &found, error);
    if (success && !found) {
        success = mereader_tui_remote_download(result.url, scheme, directory, hash, preferred_extension, &result.path, error);
    }
    free(directory);
    g_free(hash);
    if (!success) {
        goto failure;
    }

    free(scheme);
    free(url_path);
    *file = result;
    return true;

failure:
    free(scheme);
    free(url_path);
    mereader_tui_remote_file_free(&result);
    return false;
}
