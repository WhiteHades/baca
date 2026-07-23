#include "test_support.h"

#include "mereader-tui/library.h"

#include <cairo.h>
#include <cairo-pdf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zip.h>

static const unsigned char library_pixel_png[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 0x04U, 0x00U, 0x00U, 0x00U, 0xb5U,
    0x1cU, 0x0cU, 0x02U, 0x00U, 0x00U, 0x00U, 0x0bU, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xdaU, 0x63U, 0x64U, 0xf8U, 0x0fU, 0x00U, 0x01U, 0x05U,
    0x01U, 0x01U, 0x27U, 0x18U, 0xe3U, 0x66U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
};

[[nodiscard]] int mereader_tui_platform_block_exit_signals(sigset_t *previous);

typedef struct LibraryPtyEnvironment {
    char *cache;
    char *config;
    char *home;
    char *temporary;
} LibraryPtyEnvironment;

typedef struct LibraryPtyProcess {
    pid_t pid;
    int master;
    int status;
    MereaderTuiString output;
    size_t output_checkpoint;
    bool exited;
} LibraryPtyProcess;

typedef struct LibraryPtyScreen {
    char *cells;
    size_t rows;
    size_t columns;
    size_t row;
    size_t column;
    size_t saved_row;
    size_t saved_column;
} LibraryPtyScreen;

typedef struct LibrarySignalMaskResult {
    int status;
    bool blocked;
} LibrarySignalMaskResult;

static MereaderTuiHistoryEntry library_entry(const char *filepath, const char *title, const char *author, double progress,
                                       const char *last_read) {
    return (MereaderTuiHistoryEntry){
        .filepath = (char *)filepath,
        .title = (char *)title,
        .author = (char *)author,
        .reading_progress = progress,
        .last_read = (char *)last_read,
    };
}

static void *library_check_exit_signal_mask(void *context) {
    LibrarySignalMaskResult *result = context;
    sigset_t current;
    result->status = pthread_sigmask(SIG_BLOCK, NULL, &current);
    result->blocked = result->status == 0 && sigismember(&current, SIGINT) == 1 &&
                      sigismember(&current, SIGTERM) == 1 && sigismember(&current, SIGHUP) == 1;
    return NULL;
}

static void library_pty_environment_free(LibraryPtyEnvironment *environment) {
    free(environment->cache);
    free(environment->config);
    free(environment->home);
    free(environment->temporary);
    *environment = (LibraryPtyEnvironment){0};
}

static bool library_pty_environment_init(const char *name, LibraryPtyEnvironment *environment) {
    char relative[256] = {0};
    (void)snprintf(relative, sizeof(relative), "library-pty/%s/cache", name);
    if (!mereader_tui_test_mkdir(relative)) {
        return false;
    }
    environment->cache = mereader_tui_test_path(relative);
    (void)snprintf(relative, sizeof(relative), "library-pty/%s/config", name);
    if (!mereader_tui_test_mkdir(relative)) {
        library_pty_environment_free(environment);
        return false;
    }
    environment->config = mereader_tui_test_path(relative);
    (void)snprintf(relative, sizeof(relative), "library-pty/%s/home", name);
    if (!mereader_tui_test_mkdir(relative)) {
        library_pty_environment_free(environment);
        return false;
    }
    environment->home = mereader_tui_test_path(relative);
    (void)snprintf(relative, sizeof(relative), "library-pty/%s/tmp", name);
    if (!mereader_tui_test_mkdir(relative)) {
        library_pty_environment_free(environment);
        return false;
    }
    environment->temporary = mereader_tui_test_path(relative);
    if (environment->cache == NULL || environment->config == NULL || environment->home == NULL ||
        environment->temporary == NULL) {
        library_pty_environment_free(environment);
        return false;
    }
    return true;
}

static bool library_zip_add(zip_t *archive, const char *name, const char *text, bool stored) {
    zip_source_t *source = zip_source_buffer(archive, text, strlen(text), 0);
    if (source == NULL) {
        return false;
    }
    const zip_int64_t index = zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
    if (index < 0) {
        zip_source_free(source);
        return false;
    }
    return !stored || zip_set_file_compression(archive, (zip_uint64_t)index, ZIP_CM_STORE, 0U) == 0;
}

static char *library_create_epub(const char *relative, const char *title, const char *author, const char *body) {
    static const char mimetype[] = "application/epub+zip";
    static const char container[] =
        "<?xml version=\"1.0\"?>"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
        "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    char opf[2048] = {0};
    char chapter[2048] = {0};
    const int opf_length = snprintf(
        opf, sizeof(opf),
        "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" "
        "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>%s</dc:title><dc:creator>%s</dc:creator><dc:identifier id=\"id\">library</dc:identifier>"
        "</metadata><manifest><item id=\"chapter\" href=\"chapter.xhtml\" "
        "media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"chapter\"/></spine></package>",
        title, author);
    const int chapter_length =
        snprintf(chapter, sizeof(chapter),
                 "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>%s</p></body></html>", body);
    if (opf_length < 0 || (size_t)opf_length >= sizeof(opf) || chapter_length < 0 ||
        (size_t)chapter_length >= sizeof(chapter)) {
        return NULL;
    }

    char *path = mereader_tui_test_path(relative);
    MereaderTuiError error = {0};
    char *directory = path == NULL ? NULL : mereader_tui_path_dirname(path, &error);
    if (path == NULL || directory == NULL || !mereader_tui_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return NULL;
    }
    free(directory);

    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    if (archive == NULL) {
        free(path);
        return NULL;
    }
    const bool added = library_zip_add(archive, "mimetype", mimetype, true) &&
                       library_zip_add(archive, "META-INF/container.xml", container, false) &&
                       library_zip_add(archive, "OEBPS/content.opf", opf, false) &&
                       library_zip_add(archive, "OEBPS/chapter.xhtml", chapter, false);
    if (!added) {
        zip_discard(archive);
        free(path);
        return NULL;
    }
    if (zip_close(archive) != 0) {
        zip_discard(archive);
        free(path);
        return NULL;
    }
    return path;
}

static char *library_create_pdf(const char *relative, const char *text) {
    char *path = mereader_tui_test_path(relative);
    MereaderTuiError error = {0};
    char *directory = path == NULL ? NULL : mereader_tui_path_dirname(path, &error);
    if (path == NULL || directory == NULL || !mereader_tui_mkdirs(directory, &error)) {
        free(directory);
        free(path);
        return NULL;
    }
    free(directory);

    cairo_surface_t *surface = cairo_pdf_surface_create(path, 400.0, 300.0);
    cairo_t *context = cairo_create(surface);
    cairo_select_font_face(context, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 18.0);
    cairo_move_to(context, 24.0, 48.0);
    cairo_show_text(context, text);
    cairo_show_page(context);
    cairo_destroy(context);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        free(path);
        return NULL;
    }
    return path;
}

static bool library_seed_history(const LibraryPtyEnvironment *environment, const MereaderTuiHistoryEntry *entries,
                                 size_t count) {
    MereaderTuiError error = {0};
    char *database_path = mereader_tui_path_join(environment->cache, "mereader-tui/mereader-tui.db", &error);
    char *directory = database_path == NULL ? NULL : mereader_tui_path_dirname(database_path, &error);
    MereaderTuiDatabase database = {0};
    bool seeded = database_path != NULL && directory != NULL && mereader_tui_mkdirs(directory, &error) &&
                  mereader_tui_database_open(&database, database_path, &error) && mereader_tui_database_migrate(&database, &error);
    for (size_t index = 0U; seeded && index < count; ++index) {
        seeded = mereader_tui_database_save_progress(&database, &entries[index], &error);
    }
    if (!seeded) {
        fprintf(stderr, "library PTY fixture: %s\n", error.message);
    }
    mereader_tui_database_close(&database);
    free(directory);
    free(database_path);
    return seeded;
}

static bool library_configure_root(const LibraryPtyEnvironment *environment, const char *root) {
    MereaderTuiError error = {0};
    char *path = mereader_tui_path_join(environment->config, "mereader-tui/config.ini", &error);
    char *directory = path == NULL ? NULL : mereader_tui_path_dirname(path, &error);
    MereaderTuiString text = {0};
    const bool ready = path != NULL && directory != NULL && mereader_tui_mkdirs(directory, &error) &&
                       mereader_tui_string_append(&text, "[General]\nLibraryPath = ", &error) &&
                       mereader_tui_string_append(&text, root, &error) && mereader_tui_string_append_char(&text, '\n', &error);
    bool written = false;
    if (ready) {
        FILE *file = fopen(path, "wb");
        if (file != NULL) {
            written = fwrite(text.data, 1U, text.length, file) == text.length;
            written = fclose(file) == 0 && written;
        }
    }
    mereader_tui_string_free(&text);
    free(directory);
    free(path);
    return written;
}

static bool library_configure_root_with_mode(const LibraryPtyEnvironment *environment, const char *root,
                                             const char *image_mode) {
    if (!library_configure_root(environment, root)) {
        return false;
    }
    MereaderTuiError error = {0};
    char *path = mereader_tui_path_join(environment->config, "mereader-tui/config.ini", &error);
    FILE *file = path == NULL ? NULL : fopen(path, "ab");
    bool written = false;
    if (file != NULL) {
        written = fprintf(file, "ImageMode = %s\n", image_mode) > 0;
        written = fclose(file) == 0 && written;
    }
    free(path);
    return written;
}

static bool library_append_config(const LibraryPtyEnvironment *environment, const char *text) {
    MereaderTuiError error = {0};
    char *path = mereader_tui_path_join(environment->config, "mereader-tui/config.ini", &error);
    FILE *file = path == NULL ? NULL : fopen(path, "ab");
    bool written = false;
    if (file != NULL) {
        written = fwrite(text, 1U, strlen(text), file) == strlen(text);
        written = fclose(file) == 0 && written;
    }
    free(path);
    return written;
}

static bool library_format_preference_matches(const LibraryPtyEnvironment *environment, const char *root,
                                              const char *suffix) {
    MereaderTuiError error = {0};
    char *database_path = mereader_tui_path_join(environment->cache, "mereader-tui/mereader-tui.db", &error);
    MereaderTuiDatabase database = {0};
    MereaderTuiFormatPreferences preferences = {0};
    bool matches = database_path != NULL && mereader_tui_database_open(&database, database_path, &error) &&
                   mereader_tui_database_format_preferences(&database, root, &preferences, &error) &&
                   preferences.length == 1U && preferences.items[0].relative_path != NULL;
    if (matches) {
        const size_t path_length = strlen(preferences.items[0].relative_path);
        const size_t suffix_length = strlen(suffix);
        matches = suffix_length <= path_length &&
                  strcmp(preferences.items[0].relative_path + path_length - suffix_length, suffix) == 0;
    }
    mereader_tui_format_preferences_free(&preferences);
    mereader_tui_database_close(&database);
    free(database_path);
    return matches;
}

static bool library_database_execute(const LibraryPtyEnvironment *environment, const char *sql) {
    MereaderTuiError error = {0};
    char *database_path = mereader_tui_path_join(environment->cache, "mereader-tui/mereader-tui.db", &error);
    sqlite3 *database = NULL;
    int status = database_path == NULL ? SQLITE_NOMEM : sqlite3_open(database_path, &database);
    char *message = NULL;
    if (status == SQLITE_OK) {
        status = sqlite3_exec(database, sql, NULL, NULL, &message);
    }
    if (status != SQLITE_OK) {
        fprintf(stderr, "library PTY database mutation: %s\n",
                message != NULL ? message : database == NULL ? sqlite3_errstr(status) : sqlite3_errmsg(database));
    }
    sqlite3_free(message);
    if (database != NULL) {
        (void)sqlite3_close(database);
    }
    free(database_path);
    return status == SQLITE_OK;
}

static bool library_pty_drain(LibraryPtyProcess *process, bool *had_data) {
    char buffer[4096] = {0};
    MereaderTuiError error = {0};
    if (had_data != NULL) {
        *had_data = false;
    }
    for (;;) {
        const ssize_t length = read(process->master, buffer, sizeof(buffer));
        if (length > 0) {
            if (had_data != NULL) {
                *had_data = true;
            }
            if (!mereader_tui_string_append_n(&process->output, buffer, (size_t)length, &error)) {
                return false;
            }
        } else if (length < 0 && errno == EINTR) {
            continue;
        } else if (length < 0 && (errno == EAGAIN || errno == EIO)) {
            return true;
        } else {
            return length == 0;
        }
    }
}

static bool library_pty_spawn(const LibraryPtyEnvironment *environment, const char *term, unsigned short rows,
                              unsigned short columns, const char *path, LibraryPtyProcess *process) {
    *process = (LibraryPtyProcess){.master = -1};
    const struct winsize size = {.ws_row = rows, .ws_col = columns};
    (void)fflush(NULL);
    process->pid = forkpty(&process->master, NULL, NULL, &size);
    if (process->pid < 0) {
        return false;
    }
    if (process->pid == 0) {
        (void)setenv("TERM", term, 1);
        (void)setenv("HOME", environment->home, 1);
        (void)setenv("XDG_CONFIG_HOME", environment->config, 1);
        (void)setenv("XDG_CACHE_HOME", environment->cache, 1);
        (void)setenv("TMPDIR", environment->temporary, 1);
        (void)unsetenv("TERM_PROGRAM");
        (void)unsetenv("KITTY_WINDOW_ID");
        (void)unsetenv("TMUX");
        (void)unsetenv("STY");
        if (strcmp(term, "dumb") == 0 || strcmp(term, "vt100") == 0) {
            (void)unsetenv("COLORTERM");
        } else {
            (void)setenv("COLORTERM", "truecolor", 1);
        }
        if (path == NULL) {
            (void)execl("./build/mereader-tui", "mereader-tui", (char *)NULL);
        } else {
            (void)execl("./build/mereader-tui", "mereader-tui", path, (char *)NULL);
        }
        _exit(127);
    }
    const int flags = fcntl(process->master, F_GETFL);
    if (flags < 0 || fcntl(process->master, F_SETFL, flags | O_NONBLOCK) != 0) {
        (void)kill(process->pid, SIGKILL);
        (void)waitpid(process->pid, NULL, 0);
        (void)close(process->master);
        process->master = -1;
        return false;
    }
    return true;
}

static bool library_pty_resize(LibraryPtyProcess *process, unsigned short rows, unsigned short columns) {
    const struct winsize size = {.ws_row = rows, .ws_col = columns};
    return process != NULL && process->master >= 0 && ioctl(process->master, TIOCSWINSZ, &size) == 0;
}

static int64_t library_pty_now_milliseconds(void) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * INT64_C(1000) + (int64_t)now.tv_nsec / INT64_C(1000000);
}

static bool library_pty_wait_for(LibraryPtyProcess *process, const char *needle) {
    const int64_t started = library_pty_now_milliseconds();
    if (started < 0) {
        return false;
    }
    const int64_t deadline = started + INT64_C(12000);
    while (library_pty_now_milliseconds() < deadline) {
        struct pollfd descriptor = {.fd = process->master, .events = POLLIN};
        const int ready = poll(&descriptor, 1U, 20);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if ((descriptor.revents & POLLNVAL) != 0 || !library_pty_drain(process, NULL)) {
            return false;
        }
        if (process->output.data != NULL && process->output_checkpoint <= process->output.length &&
            strstr(process->output.data + process->output_checkpoint, needle) != NULL) {
            return true;
        }
        const pid_t waited = waitpid(process->pid, &process->status, WNOHANG);
        if (waited == process->pid) {
            process->exited = true;
            return false;
        }
        if (waited < 0) {
            return false;
        }
    }
    return false;
}

static bool library_pty_send(LibraryPtyProcess *process, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length > 0U) {
        const ssize_t written = write(process->master, cursor, length);
        if (written > 0) {
            cursor += (size_t)written;
            length -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool library_pty_send_text(LibraryPtyProcess *process, const char *text) {
    return library_pty_send(process, text, strlen(text));
}

static void library_pty_clear_output(LibraryPtyProcess *process) {
    process->output_checkpoint = process->output.length;
}

static bool library_pty_settle(LibraryPtyProcess *process) {
    const int64_t started = library_pty_now_milliseconds();
    if (started < 0) {
        return false;
    }
    const int64_t deadline = started + INT64_C(1000);
    int64_t quiet_since = -1;
    while (library_pty_now_milliseconds() < deadline) {
        struct pollfd descriptor = {.fd = process->master, .events = POLLIN};
        const int ready = poll(&descriptor, 1U, 20);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bool had_data = false;
        if ((descriptor.revents & POLLNVAL) != 0 || !library_pty_drain(process, &had_data)) {
            return false;
        }
        const int64_t now = library_pty_now_milliseconds();
        if (now < 0) {
            return false;
        }
        if (had_data) {
            quiet_since = -1;
        } else if (quiet_since < 0) {
            quiet_since = now;
        } else if (now - quiet_since >= INT64_C(60)) {
            return true;
        }
        const pid_t waited = waitpid(process->pid, &process->status, WNOHANG);
        if (waited == process->pid) {
            process->exited = true;
            return false;
        }
        if (waited < 0 && errno != EINTR) {
            return false;
        }
    }
    return false;
}

static void library_pty_screen_delete_characters(LibraryPtyScreen *screen, size_t count) {
    char *line = screen->cells + screen->row * screen->columns;
    const size_t available = screen->columns - screen->column;
    const size_t removed = count < available ? count : available;
    if (removed < available) {
        memmove(line + screen->column, line + screen->column + removed, available - removed);
    }
    memset(line + screen->columns - removed, ' ', removed);
}

static void library_pty_screen_insert_characters(LibraryPtyScreen *screen, size_t count) {
    char *line = screen->cells + screen->row * screen->columns;
    const size_t available = screen->columns - screen->column;
    const size_t inserted = count < available ? count : available;
    if (inserted < available) {
        memmove(line + screen->column + inserted, line + screen->column, available - inserted);
    }
    memset(line + screen->column, ' ', inserted);
}

static void library_pty_screen_erase_characters(LibraryPtyScreen *screen, size_t count) {
    const size_t available = screen->columns - screen->column;
    const size_t erased = count < available ? count : available;
    memset(screen->cells + screen->row * screen->columns + screen->column, ' ', erased);
}

static void library_pty_screen_clear(LibraryPtyScreen *screen) {
    memset(screen->cells, ' ', screen->rows * screen->columns);
}

static size_t library_pty_screen_parameter(const int parameters[8], size_t count, size_t index,
                                           size_t fallback) {
    if (index >= count || parameters[index] <= 0) {
        return fallback;
    }
    return (size_t)parameters[index];
}

static void library_pty_screen_move(LibraryPtyScreen *screen, size_t row, size_t column) {
    screen->row = row < screen->rows ? row : screen->rows - 1U;
    screen->column = column < screen->columns ? column : screen->columns - 1U;
}

static void library_pty_screen_private_mode(LibraryPtyScreen *screen, unsigned char final,
                                            const int parameters[8], size_t count) {
    if ((final == 'h' || final == 'l') && count > 0U && parameters[0] == 1049) {
        library_pty_screen_clear(screen);
        library_pty_screen_move(screen, 0U, 0U);
    }
}

static void library_pty_screen_erase_line(LibraryPtyScreen *screen, int mode) {
    char *line = screen->cells + screen->row * screen->columns;
    if (mode == 2) {
        memset(line, ' ', screen->columns);
    } else if (mode == 1) {
        memset(line, ' ', screen->column + 1U);
    } else {
        memset(line + screen->column, ' ', screen->columns - screen->column);
    }
}

static void library_pty_screen_erase_display(LibraryPtyScreen *screen, int mode) {
    const size_t offset = screen->row * screen->columns + screen->column;
    const size_t length = screen->rows * screen->columns;
    if (mode == 2 || mode == 3) {
        library_pty_screen_clear(screen);
    } else if (mode == 1) {
        memset(screen->cells, ' ', offset + 1U);
    } else {
        memset(screen->cells + offset, ' ', length - offset);
    }
}

static void library_pty_screen_csi(LibraryPtyScreen *screen, unsigned char final, const int parameters[8],
                                   size_t count, bool private_mode) {
    if (private_mode) {
        library_pty_screen_private_mode(screen, final, parameters, count);
        return;
    }
    const size_t first = library_pty_screen_parameter(parameters, count, 0U, 1U);
    const size_t second = library_pty_screen_parameter(parameters, count, 1U, 1U);
    switch (final) {
    case '@':
        library_pty_screen_insert_characters(screen, first);
        break;
    case 'A':
        screen->row = first > screen->row ? 0U : screen->row - first;
        break;
    case 'B':
        library_pty_screen_move(screen, screen->row + first, screen->column);
        break;
    case 'C':
        library_pty_screen_move(screen, screen->row, screen->column + first);
        break;
    case 'D':
        screen->column = first > screen->column ? 0U : screen->column - first;
        break;
    case 'E':
        library_pty_screen_move(screen, screen->row + first, 0U);
        break;
    case 'F':
        screen->row = first > screen->row ? 0U : screen->row - first;
        screen->column = 0U;
        break;
    case 'G':
    case '`':
        library_pty_screen_move(screen, screen->row, first - 1U);
        break;
    case 'H':
    case 'f':
        library_pty_screen_move(screen, first - 1U, second - 1U);
        break;
    case 'J':
        library_pty_screen_erase_display(screen, count > 0U ? parameters[0] : 0);
        break;
    case 'K':
        library_pty_screen_erase_line(screen, count > 0U ? parameters[0] : 0);
        break;
    case 'P':
        library_pty_screen_delete_characters(screen, first);
        break;
    case 'X':
        library_pty_screen_erase_characters(screen, first);
        break;
    case 'd':
        library_pty_screen_move(screen, first - 1U, screen->column);
        break;
    case 's':
        screen->saved_row = screen->row;
        screen->saved_column = screen->column;
        break;
    case 'u':
        library_pty_screen_move(screen, screen->saved_row, screen->saved_column);
        break;
    default:
        break;
    }
}

static void library_pty_screen_write(LibraryPtyScreen *screen, unsigned char value) {
    if (screen->column >= screen->columns) {
        screen->column = 0U;
        if (screen->row + 1U < screen->rows) {
            ++screen->row;
        }
    }
    screen->cells[screen->row * screen->columns + screen->column] =
        value >= 0x20U && value < 0x7fU ? (char)value : '?';
    ++screen->column;
}

static bool library_pty_screen_replay(const LibraryPtyProcess *process, size_t rows, size_t columns,
                                      LibraryPtyScreen *screen) {
    if (process->output.data == NULL || rows == 0U || columns == 0U || rows > SIZE_MAX / columns) {
        return false;
    }
    *screen = (LibraryPtyScreen){.rows = rows, .columns = columns};
    screen->cells = malloc(rows * columns);
    if (screen->cells == NULL) {
        return false;
    }
    library_pty_screen_clear(screen);

    const unsigned char *data = (const unsigned char *)process->output.data;
    for (size_t index = 0U; index < process->output.length;) {
        const unsigned char value = data[index++];
        if (value == 0x1bU && index < process->output.length) {
            const unsigned char next = data[index++];
            if (next == '[') {
                int parameters[8] = {0};
                size_t count = 1U;
                bool private_mode = false;
                while (index < process->output.length) {
                    const unsigned char part = data[index++];
                    if (part >= '0' && part <= '9') {
                        if (parameters[count - 1U] <= (INT_MAX - 9) / 10) {
                            parameters[count - 1U] = parameters[count - 1U] * 10 + (int)(part - '0');
                        }
                    } else if (part == ';' && count < MEREADER_TUI_ARRAY_LEN(parameters)) {
                        ++count;
                    } else if (part == '?' && count == 1U && parameters[0] == 0) {
                        private_mode = true;
                    } else if (part >= 0x40U && part <= 0x7eU) {
                        library_pty_screen_csi(screen, part, parameters, count, private_mode);
                        break;
                    }
                }
            } else if (next == ']') {
                while (index < process->output.length && data[index] != 0x07U) {
                    if (data[index] == 0x1bU && index + 1U < process->output.length && data[index + 1U] == '\\') {
                        index += 2U;
                        break;
                    }
                    ++index;
                }
                if (index < process->output.length && data[index] == 0x07U) {
                    ++index;
                }
            } else if (next == '7') {
                screen->saved_row = screen->row;
                screen->saved_column = screen->column;
            } else if (next == '8') {
                library_pty_screen_move(screen, screen->saved_row, screen->saved_column);
            } else if ((next == '(' || next == ')') && index < process->output.length) {
                ++index;
            }
            continue;
        }
        if (value == '\r') {
            screen->column = 0U;
        } else if (value == '\n') {
            if (screen->row + 1U < screen->rows) {
                ++screen->row;
            }
        } else if (value == '\b') {
            if (screen->column > 0U) {
                --screen->column;
            }
        } else if (value == '\t') {
            library_pty_screen_move(screen, screen->row, (screen->column + 8U) & ~7U);
        } else if (value >= 0x20U && value != 0x7fU) {
            library_pty_screen_write(screen, value);
        }
    }
    return true;
}

static bool library_pty_screen_line_matches(const LibraryPtyProcess *process, size_t rows, size_t columns,
                                            const char *needle, const char *suffix) {
    LibraryPtyScreen screen = {0};
    if (!library_pty_screen_replay(process, rows, columns, &screen)) {
        return false;
    }
    char *line = malloc(columns + 1U);
    bool matched = false;
    if (line != NULL) {
        for (size_t row = 0U; row < rows; ++row) {
            memcpy(line, screen.cells + row * columns, columns);
            line[columns] = '\0';
            if (strstr(line, needle) == NULL) {
                continue;
            }
            size_t length = columns;
            while (length > 0U && line[length - 1U] == ' ') {
                --length;
            }
            const size_t suffix_length = suffix == NULL ? 0U : strlen(suffix);
            matched = suffix == NULL ||
                      (suffix_length <= length && memcmp(line + length - suffix_length, suffix, suffix_length) == 0);
            if (matched) {
                break;
            }
        }
    }
    free(line);
    free(screen.cells);
    return matched;
}

static bool library_pty_wait_exit(LibraryPtyProcess *process) {
    if (process->exited) {
        return true;
    }
    const int64_t started = library_pty_now_milliseconds();
    if (started < 0) {
        return false;
    }
    const int64_t deadline = started + INT64_C(12000);
    while (library_pty_now_milliseconds() < deadline) {
        if (!library_pty_drain(process, NULL)) {
            return false;
        }
        const pid_t waited = waitpid(process->pid, &process->status, WNOHANG);
        if (waited == process->pid) {
            process->exited = true;
            (void)library_pty_drain(process, NULL);
            return true;
        }
        if (waited < 0 && errno != EINTR) {
            return false;
        }
        struct pollfd descriptor = {.fd = process->master, .events = POLLIN};
        const int ready = poll(&descriptor, 1U, 20);
        if (ready < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            return false;
        }
    }
    return false;
}

static void library_pty_process_free(LibraryPtyProcess *process) {
    if (process->pid > 0 && !process->exited) {
        (void)kill(process->pid, SIGKILL);
        while (waitpid(process->pid, &process->status, 0) < 0 && errno == EINTR) {
        }
    }
    if (process->master >= 0) {
        (void)close(process->master);
    }
    mereader_tui_string_free(&process->output);
    *process = (LibraryPtyProcess){.master = -1};
}

static MereaderTuiTestResult test_projection_and_literal_filtering(void) {
    MereaderTuiHistoryEntry entries[] = {
        library_entry("/books/one.epub", "One [Draft]", "Alice", 0.1, "2026-03-03 00:00:00"),
        library_entry("/shelf/two.epub", "Second", "Bob", 0.2, "2026-03-02 00:00:00"),
        library_entry("/books/three.epub", "Third", "Carol", 0.3, "2026-03-01 00:00:00"),
    };
    MereaderTuiHistory history = {.items = entries, .length = MEREADER_TUI_ARRAY_LEN(entries)};
    MereaderTuiLibraryView view = {0};
    MereaderTuiError error = {0};

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "[DRAFT]", MEREADER_TUI_LIBRARY_SORT_RECENT, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(view.length, 1U);
    TEST_ASSERT_STR(view.rows[0].entry->filepath, "/books/one.epub");
    mereader_tui_library_view_free(&view);

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "bOb", MEREADER_TUI_LIBRARY_SORT_RECENT, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(view.length, 1U);
    TEST_ASSERT_STR(view.rows[0].entry->filepath, "/shelf/two.epub");
    mereader_tui_library_view_free(&view);

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "/BOOKS/", MEREADER_TUI_LIBRARY_SORT_RECENT, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(view.length, 2U);
    mereader_tui_library_view_free(&view);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_sorting_and_filepath_ties(void) {
    MereaderTuiHistoryEntry entries[] = {
        library_entry("/z.epub", "Same", "Beta", 0.1, "2026-03-02 00:00:00"),
        library_entry("/a.epub", "same", "Beta", 0.2, "2026-03-02 00:00:00"),
        library_entry("/m.epub", "Alpha", "alpha", 0.3, "2026-03-03 00:00:00"),
    };
    MereaderTuiHistory history = {.items = entries, .length = MEREADER_TUI_ARRAY_LEN(entries)};
    MereaderTuiLibraryView view = {0};
    MereaderTuiError error = {0};

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "", MEREADER_TUI_LIBRARY_SORT_RECENT, &error), "%s",
                    error.message);
    TEST_ASSERT_STR(view.rows[0].entry->filepath, "/m.epub");
    TEST_ASSERT_STR(view.rows[1].entry->filepath, "/a.epub");
    TEST_ASSERT_STR(view.rows[2].entry->filepath, "/z.epub");
    mereader_tui_library_view_free(&view);

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "", MEREADER_TUI_LIBRARY_SORT_TITLE, &error), "%s",
                    error.message);
    TEST_ASSERT_STR(view.rows[0].entry->filepath, "/m.epub");
    TEST_ASSERT_STR(view.rows[1].entry->filepath, "/a.epub");
    TEST_ASSERT_STR(view.rows[2].entry->filepath, "/z.epub");
    mereader_tui_library_view_free(&view);

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "", MEREADER_TUI_LIBRARY_SORT_AUTHOR, &error), "%s",
                    error.message);
    TEST_ASSERT_STR(view.rows[0].entry->filepath, "/m.epub");
    TEST_ASSERT_STR(view.rows[1].entry->filepath, "/a.epub");
    TEST_ASSERT_STR(view.rows[2].entry->filepath, "/z.epub");
    mereader_tui_library_view_free(&view);

    TEST_ASSERT(mereader_tui_library_sort_next(MEREADER_TUI_LIBRARY_SORT_RECENT) == MEREADER_TUI_LIBRARY_SORT_TITLE);
    TEST_ASSERT(mereader_tui_library_sort_next(MEREADER_TUI_LIBRARY_SORT_TITLE) == MEREADER_TUI_LIBRARY_SORT_AUTHOR);
    TEST_ASSERT(mereader_tui_library_sort_next(MEREADER_TUI_LIBRARY_SORT_AUTHOR) == MEREADER_TUI_LIBRARY_SORT_RECENT);
    TEST_ASSERT_STR(mereader_tui_library_sort_name(MEREADER_TUI_LIBRARY_SORT_AUTHOR), "author");
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_recent_sorting_rejects_invalid_timestamps(void) {
    MereaderTuiHistoryEntry entries[] = {
        library_entry("/invalid-z.epub", "Invalid Z", "A", 0.1, "9999-99-99 99:99:99"),
        library_entry("/older.epub", "Older", "A", 0.2, "2025-12-31 23:59:59"),
        library_entry("/missing.epub", "Missing", "A", 0.3, NULL),
        library_entry("/newer.epub", "Newer", "A", 0.4, "2026-01-01 00:00:00"),
        library_entry("/invalid-a.epub", "Invalid A", "A", 0.5, "2026-01-01 00:00:00junk"),
    };
    MereaderTuiHistory history = {.items = entries, .length = MEREADER_TUI_ARRAY_LEN(entries)};
    MereaderTuiLibraryView view = {0};
    MereaderTuiError error = {0};

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "", MEREADER_TUI_LIBRARY_SORT_RECENT, &error), "%s",
                    error.message);
    TEST_ASSERT_STR(view.rows[0].entry->filepath, "/newer.epub");
    TEST_ASSERT_STR(view.rows[1].entry->filepath, "/older.epub");
    TEST_ASSERT_STR(view.rows[2].entry->filepath, "/invalid-a.epub");
    TEST_ASSERT_STR(view.rows[3].entry->filepath, "/invalid-z.epub");
    TEST_ASSERT_STR(view.rows[4].entry->filepath, "/missing.epub");
    mereader_tui_library_view_free(&view);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_detached_worker_exit_signal_mask(void) {
    sigset_t previous;
    LibrarySignalMaskResult mask = {0};
    pthread_t thread = {0};
    const int block_status = mereader_tui_platform_block_exit_signals(&previous);
    const int create_status = block_status == 0 ? pthread_create(&thread, NULL, library_check_exit_signal_mask, &mask)
                                                : block_status;
    const int restore_status = block_status == 0 ? pthread_sigmask(SIG_SETMASK, &previous, NULL) : 0;
    const int join_status = create_status == 0 ? pthread_join(thread, NULL) : 0;

    TEST_ASSERT_INT(block_status, 0);
    TEST_ASSERT_INT(restore_status, 0);
    TEST_ASSERT_INT(create_status, 0);
    TEST_ASSERT_INT(join_status, 0);
    TEST_ASSERT_INT(mask.status, 0);
    TEST_ASSERT(mask.blocked);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_unicode_and_control_sanitization(void) {
    static const char hostile_title[] = "Caf\xc3\xa9\n\x1b[31m \xe6\x9b\xb8\xc2\x85" "end\xff";
    MereaderTuiHistoryEntry entry =
        library_entry("/unicode.epub", hostile_title, "Au\tthor\x7f", 0.5, "2026-03-01 00:00:00");
    MereaderTuiHistory history = {.items = &entry, .length = 1U};
    MereaderTuiLibraryView view = {0};
    MereaderTuiError error = {0};

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "", MEREADER_TUI_LIBRARY_SORT_TITLE, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(view.length, 1U);
    TEST_ASSERT_STR(view.rows[0].title, "Caf\xc3\xa9[31m \xe6\x9b\xb8" "end\xef\xbf\xbd");
    TEST_ASSERT_STR(view.rows[0].author, "Author");
    TEST_ASSERT(strchr(view.rows[0].title, '\n') == NULL);
    TEST_ASSERT(strchr(view.rows[0].title, '\x1b') == NULL);
    mereader_tui_library_view_free(&view);

    char *truncated = mereader_tui_library_sanitize_text(
        "A\x1b" "B\xc2\x85\xe2\x80\xae\xe6\x9b\xb8" "e\xcc\x81Z", 5U, &error);
    TEST_ASSERT_MSG(truncated != NULL, "%s", error.message);
    TEST_ASSERT_STR(truncated, "AB\xe6\x9b\xb8" "e\xcc\x81");
    TEST_ASSERT_SIZE(mereader_tui_utf8_width(truncated, strlen(truncated)), 5U);
    free(truncated);

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "CAF\xc3\x89", MEREADER_TUI_LIBRARY_SORT_TITLE, &error),
                    "%s", error.message);
    TEST_ASSERT_SIZE(view.length, 1U);
    mereader_tui_library_view_free(&view);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_empty_missing_and_fallback_rows(void) {
    MereaderTuiHistory empty = {0};
    MereaderTuiLibraryView view = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &empty, "", MEREADER_TUI_LIBRARY_SORT_RECENT, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(view.length, 0U);
    mereader_tui_library_view_free(&view);

    MereaderTuiHistoryEntry entries[] = {
        library_entry("/missing/fallback.epub", NULL, NULL, 0.0, NULL),
        library_entry(NULL, "", "", 0.0, NULL),
    };
    MereaderTuiHistory history = {.items = entries, .length = MEREADER_TUI_ARRAY_LEN(entries)};
    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "", MEREADER_TUI_LIBRARY_SORT_TITLE, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(view.length, 2U);
    TEST_ASSERT_STR(view.rows[0].title, "fallback.epub");
    TEST_ASSERT_STR(view.rows[0].author, "");
    TEST_ASSERT_STR(view.rows[1].title, "untitled");
    mereader_tui_library_view_free(&view);

    MereaderTuiHistoryEntry unicode_whitespace =
        library_entry("/missing/unicode.epub", "\xc2\xa0\xe3\x80\x80", "\xe2\x80\x83", 0.0, NULL);
    history = (MereaderTuiHistory){.items = &unicode_whitespace, .length = 1U};
    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "", MEREADER_TUI_LIBRARY_SORT_TITLE, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(view.length, 1U);
    TEST_ASSERT_STR(view.rows[0].title, "unicode.epub");
    TEST_ASSERT_STR(view.rows[0].author, "");
    mereader_tui_library_view_free(&view);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_selection_preservation(void) {
    MereaderTuiHistoryEntry entries[] = {
        library_entry("/c.epub", "Charlie", "C", 0.1, "2026-03-03 00:00:00"),
        library_entry("/a.epub", "Alpha", "A", 0.2, "2026-03-02 00:00:00"),
        library_entry("/b.epub", "Bravo", "B", 0.3, "2026-03-01 00:00:00"),
    };
    MereaderTuiHistory history = {.items = entries, .length = MEREADER_TUI_ARRAY_LEN(entries)};
    MereaderTuiLibraryView view = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "", MEREADER_TUI_LIBRARY_SORT_TITLE, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(mereader_tui_library_preserve_selection(&view, "/b.epub", 0U), 1U);
    TEST_ASSERT_SIZE(mereader_tui_library_preserve_selection(&view, "/gone.epub", 2U), 2U);
    TEST_ASSERT_SIZE(mereader_tui_library_preserve_selection(&view, "/gone.epub", 99U), 2U);
    mereader_tui_library_view_free(&view);

    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &history, "alpha", MEREADER_TUI_LIBRARY_SORT_TITLE, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(mereader_tui_library_preserve_selection(&view, "/b.epub", 9U), 0U);
    mereader_tui_library_view_free(&view);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_deleted_middle_selection_preservation(void) {
    MereaderTuiHistoryEntry before_entries[] = {
        library_entry("/alpha.epub", "Alpha", "A", 0.1, "2026-03-03 00:00:00"),
        library_entry("/missing.epub", "Missing", "M", 0.2, "2026-03-02 00:00:00"),
        library_entry("/charlie.epub", "Charlie", "C", 0.3, "2026-03-01 00:00:00"),
    };
    MereaderTuiHistory before = {.items = before_entries, .length = MEREADER_TUI_ARRAY_LEN(before_entries)};
    MereaderTuiLibraryView view = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &before, "", MEREADER_TUI_LIBRARY_SORT_RECENT, &error), "%s",
                    error.message);
    const size_t deleted_index = mereader_tui_library_preserve_selection(&view, "/missing.epub", 0U);
    TEST_ASSERT_SIZE(deleted_index, 1U);
    mereader_tui_library_view_free(&view);

    MereaderTuiHistoryEntry after_entries[] = {
        before_entries[0],
        before_entries[2],
    };
    MereaderTuiHistory after = {.items = after_entries, .length = MEREADER_TUI_ARRAY_LEN(after_entries)};
    TEST_ASSERT_MSG(mereader_tui_library_view_build(&view, &after, "", MEREADER_TUI_LIBRARY_SORT_RECENT, &error), "%s",
                    error.message);
    const size_t selected = mereader_tui_library_preserve_selection(&view, "/missing.epub", deleted_index);
    TEST_ASSERT_SIZE(selected, 1U);
    TEST_ASSERT_STR(view.rows[selected].entry->filepath, "/charlie.epub");
    mereader_tui_library_view_free(&view);
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_empty_library(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *book = library_create_epub("library-pty/setup/root/book.epub", "Setup Book", "Reader",
                                     "SETUP BOOK BODY");
    char *root = mereader_tui_test_path("library-pty/setup/root");
    MereaderTuiString input = {0};
    MereaderTuiError error = {0};
    bool success = book != NULL && root != NULL && mereader_tui_string_append(&input, root, &error) &&
                   mereader_tui_string_append_char(&input, '\n', &error) &&
                   library_pty_environment_init("empty", &environment) &&
                    library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                    library_pty_wait_for(&process, "Paste the path to your book directory") &&
                    library_pty_send_text(&process, "/definitely/missing/library\n") &&
                    library_pty_wait_for(&process, "cannot use library") &&
                    library_pty_send_text(&process, input.data) && library_pty_wait_for(&process, "library added") &&
                    library_pty_settle(&process) &&
                    library_pty_screen_line_matches(&process, 12U, 60U, "book", NULL) &&
                    library_pty_send_text(&process, "q") &&
                    library_pty_wait_exit(&process) && WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    if (success) {
        char *config = mereader_tui_path_join(environment.config, "mereader-tui/config.ini", &error);
        MereaderTuiBuffer contents = {0};
        success = config != NULL && mereader_tui_read_file(config, &contents, &error) &&
                  strstr((const char *)contents.data, root) != NULL;
        mereader_tui_buffer_free(&contents);
        free(config);
    }
    if (!success) {
        fprintf(stderr, "setup PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    mereader_tui_string_free(&input);
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(book);
    free(root);
    return success;
}

static MereaderTuiTestResult test_pty_empty_library(void) {
    TEST_ASSERT(run_pty_empty_library());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_open_return(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *book = library_create_epub("library-pty/open/book.epub", "Library One", "Reader",
                                     "OPEN RETURN BODY");
    MereaderTuiHistoryEntry entry = library_entry(book, "Library One", "Reader", 0.25, "2026-07-19 12:00:00");
    bool success = book != NULL && library_pty_environment_init("open", &environment) &&
                   library_seed_history(&environment, &entry, 1U) &&
                   library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                   library_pty_wait_for(&process, "Library One");
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "\n") && library_pty_wait_for(&process, "OPEN RETURN BODY");
    }
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "q") && library_pty_wait_for(&process, "Library One") &&
                  library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(book);
    return success;
}

static bool run_pty_typed_path(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *book = library_create_epub("library-pty/typed/book.epub", "Typed Book", "Reader", "TYPED PATH BODY");
    MereaderTuiString input = {0};
    MereaderTuiError error = {0};
    bool success = book != NULL && library_pty_environment_init("typed", &environment) &&
                    mereader_tui_string_append_char(&input, 27, &error) && mereader_tui_string_append_char(&input, 'o', &error) &&
                    mereader_tui_string_append(&input, book, &error) &&
                    mereader_tui_string_append_char(&input, '\n', &error) &&
                    library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                    library_pty_wait_for(&process, "Paste the path to your book directory") &&
                   library_pty_send_text(&process, input.data) && library_pty_wait_for(&process, "TYPED PATH BODY");
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "q") && library_pty_wait_for(&process, "Typed Book") &&
                  library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    mereader_tui_string_free(&input);
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(book);
    return success;
}

static MereaderTuiTestResult test_pty_open_return_and_typed_path(void) {
    TEST_ASSERT(run_pty_open_return());
    TEST_ASSERT(run_pty_typed_path());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_custom_library_keymaps(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    const char *stage = "startup";
    char *first = library_create_epub("library-pty/custom-keys/first.epub", "First Key", "Reader", "FIRST KEY BODY");
    char *second = library_create_epub("library-pty/custom-keys/second.epub", "Second Key", "Reader", "SECOND KEY BODY");
    MereaderTuiHistoryEntry entries[] = {
        library_entry(first, "First Key", "Reader", 0.1, "2026-07-20 12:00:00"),
        library_entry(second, "Second Key", "Reader", 0.2, "2026-07-19 12:00:00"),
    };
    static const char keymaps[] =
        "[Library Keymaps]\n"
        "MoveDown = x\n"
        "First = zz\n"
        "ToggleView = t\n"
        "Help = HH\n"
        "Confirm = ee\n"
        "Close = CC\n"
        "Quit = Q\n";
    bool success = first != NULL && second != NULL && library_pty_environment_init("custom-keys", &environment) &&
                   library_seed_history(&environment, entries, MEREADER_TUI_ARRAY_LEN(entries)) &&
                   library_configure_root(&environment, "auto") && library_append_config(&environment, keymaps) &&
                   library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                   library_pty_wait_for(&process, "HH help") && library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 12U, 60U, "1/2 - recent", NULL);
    if (success) {
        stage = "navigation";
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "j") && library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 12U, 60U, "1/2 - recent", NULL) &&
                  library_pty_send_text(&process, "x") && library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 12U, 60U, "2/2 - recent", NULL) &&
                  library_pty_send_text(&process, "zz") && library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 12U, 60U, "1/2 - recent", NULL);
    }
    if (success) {
        stage = "legacy view key";
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "v") && library_pty_settle(&process) &&
                  !library_pty_screen_line_matches(&process, 12U, 60U, " / card", NULL);
    }
    if (success) {
        stage = "custom view key";
        success = library_pty_send_text(&process, "t") && library_pty_wait_for(&process, " / card");
    }
    if (success) {
        stage = "legacy help key";
        success = library_pty_send_text(&process, "?") && library_pty_settle(&process) &&
                  !library_pty_screen_line_matches(&process, 12U, 60U, "navigation", NULL);
    }
    if (success) {
        stage = "help sequence prefix";
        success = library_pty_send_text(&process, "H") && library_pty_settle(&process) &&
                  !library_pty_screen_line_matches(&process, 12U, 60U, "navigation", NULL);
    }
    if (success) {
        stage = "help sequence";
        success = library_pty_send_text(&process, "H") && library_pty_wait_for(&process, "navigation") &&
                  library_pty_settle(&process);
    }
    if (success) {
        stage = "help labels";
        success = library_pty_screen_line_matches(&process, 12U, 60U, "select the next item", NULL) &&
                  library_pty_screen_line_matches(&process, 12U, 60U, "x / up / k scroll", NULL);
    }
    if (success) {
        stage = "custom help close";
        success = library_pty_send_text(&process, "CC") && library_pty_settle(&process);
    }
    if (success) {
        stage = "custom modal keys";
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "  ") && library_pty_wait_for(&process, "find a book") &&
                  library_pty_send_text(&process, "ee") && library_pty_wait_for(&process, "FIRST KEY BODY") &&
                  library_pty_send_text(&process, "q") && library_pty_wait_for(&process, "First Key") &&
                  library_pty_send_text(&process, "q") && library_pty_settle(&process) && !process.exited &&
                  library_pty_send_text(&process, "HH") && library_pty_wait_for(&process, "navigation") &&
                  library_pty_send_text(&process, "Q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "custom keymaps PTY stage %s capture: %s\n", stage,
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(first);
    free(second);
    return success;
}

static MereaderTuiTestResult test_pty_custom_library_keymaps(void) {
    TEST_ASSERT(run_pty_custom_library_keymaps());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_live_resize(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *first = library_create_epub("library-pty/resize/first.epub", "First Resize", "Reader",
                                      "FIRST RESIZE BODY");
    char *second = library_create_epub("library-pty/resize/second.epub", "Second Resize", "Reader",
                                       "SECOND RESIZE BODY");
    MereaderTuiHistoryEntry entries[] = {
        library_entry(first, "First Resize", "Reader", 0.1, "2026-07-20 12:00:00"),
        library_entry(second, "Second Resize", "Reader", 0.2, "2026-07-19 12:00:00"),
    };
    bool success = first != NULL && second != NULL && library_pty_environment_init("resize", &environment) &&
                   library_seed_history(&environment, entries, MEREADER_TUI_ARRAY_LEN(entries)) &&
                   library_configure_root(&environment, "auto") &&
                   library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                   library_pty_wait_for(&process, "First Resize") && library_pty_send_text(&process, "j") &&
                   library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 12U, 60U, "2/2 - recent", NULL) &&
                   library_pty_resize(&process, 3U, 20U) && library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 3U, 20U, "2/2", NULL) &&
                   library_pty_resize(&process, 16U, 80U) && library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 16U, 80U, "Second Resize", NULL) &&
                   library_pty_screen_line_matches(&process, 16U, 80U, "2/2 - recent", NULL) &&
                   library_pty_send_text(&process, "?") && library_pty_wait_for(&process, "navigation") &&
                   library_pty_settle(&process) && library_pty_resize(&process, 6U, 24U) &&
                   library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 6U, 24U, "help", NULL) &&
                   library_pty_resize(&process, 14U, 80U) && library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 14U, 80U, "navigation", NULL) &&
                   library_pty_send_text(&process, "?") && library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 14U, 80U, "Second Resize", NULL) &&
                   library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                   WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    if (!success) {
        fprintf(stderr, "live resize PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(first);
    free(second);
    return success;
}

static MereaderTuiTestResult test_pty_live_resize(void) {
    TEST_ASSERT(run_pty_live_resize());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_progress_save_failure(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *book = library_create_epub("library-pty/save-failure/book.epub", "Save Failure", "Reader",
                                     "SAVE FAILURE BODY");
    MereaderTuiHistoryEntry entry = library_entry(book, "Save Failure", "Reader", 0.25, "2026-07-19 12:00:00");
    bool success = book != NULL && library_pty_environment_init("save-failure", &environment) &&
                   library_seed_history(&environment, &entry, 1U) &&
                   library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                   library_pty_wait_for(&process, "Save Failure") && library_pty_send_text(&process, "\n") &&
                   library_pty_wait_for(&process, "SAVE FAILURE BODY") &&
                   library_database_execute(
                       &environment,
                       "CREATE TRIGGER fail_progress BEFORE INSERT ON reading_history "
                       "BEGIN SELECT RAISE(FAIL, 'forced save failure'); END;");
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "q") && library_pty_wait_for(&process, "progress not saved") &&
                  library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(book);
    return success;
}

static MereaderTuiTestResult test_pty_progress_save_failure(void) {
    TEST_ASSERT(run_pty_progress_save_failure());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_literal_filtering(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *needle = library_create_epub("library-pty/filter/needle.epub", "Needle [One]", "Alice",
                                       "FILTER TARGET BODY");
    bool fixtures = needle != NULL && mereader_tui_test_write_text("library-pty/filter/other.epub", "other");
    char *other = mereader_tui_test_path("library-pty/filter/other.epub");
    MereaderTuiHistoryEntry entries[] = {
        library_entry(needle, "Needle [One]", "Alice", 0.1, "2026-07-19 12:00:00"),
        library_entry(other, "Other Book", "Bob", 0.2, "2026-07-18 12:00:00"),
    };
    bool success = fixtures && needle != NULL && other != NULL && library_pty_environment_init("filter", &environment) &&
                    library_seed_history(&environment, entries, MEREADER_TUI_ARRAY_LEN(entries)) &&
                    library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                    library_pty_wait_for(&process, "Needle [One]") && library_pty_wait_for(&process, "Other Book") &&
                    library_pty_send_text(&process, "j") && library_pty_settle(&process) &&
                    library_pty_screen_line_matches(&process, 12U, 60U, "2/2 - recent", NULL);
    if (success) {
        success = library_pty_send_text(&process, "/[one]\n") && library_pty_wait_for(&process, "- /[one]") &&
                  library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 12U, 60U, "Needle [One]", NULL) &&
                  !library_pty_screen_line_matches(&process, 12U, 60U, "Other Book", NULL);
    }
    if (success) {
        const char escape = 27;
        library_pty_clear_output(&process);
        success = library_pty_send(&process, &escape, 1U) && library_pty_wait_for(&process, "Other Book") &&
                  library_pty_send_text(&process, "/[one]\n") && library_pty_wait_for(&process, "- /[one]");
    }
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "\n") && library_pty_wait_for(&process, "FILTER TARGET BODY");
    }
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "q") && library_pty_wait_for(&process, "Other Book") &&
                  library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "filter PTY capture: %s\n", process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(needle);
    free(other);
    return success;
}

static MereaderTuiTestResult test_pty_literal_filtering(void) {
    TEST_ASSERT(run_pty_literal_filtering());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_sorting_and_refresh(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    bool fixtures = mereader_tui_test_write_text("library-pty/sort/alpha.epub", "alpha") &&
                    mereader_tui_test_write_text("library-pty/sort/bravo.epub", "bravo") &&
                    mereader_tui_test_write_text("library-pty/sort/charlie.epub", "charlie");
    char *alpha = mereader_tui_test_path("library-pty/sort/alpha.epub");
    char *bravo = mereader_tui_test_path("library-pty/sort/bravo.epub");
    char *charlie = mereader_tui_test_path("library-pty/sort/charlie.epub");
    char *missing = mereader_tui_test_path("library-pty/sort/missing.epub");
    MereaderTuiHistoryEntry entries[] = {
        library_entry(missing, "Missing", "M", 0.0, "2026-07-20 12:00:00"),
        library_entry(alpha, "Zulu", "Z", 0.1, "2026-07-19 12:00:00"),
        library_entry(bravo, "Bravo", "B", 0.2, "2026-07-18 12:00:00"),
        library_entry(charlie, "Charlie", "C", 0.3, "2026-07-17 12:00:00"),
    };
    bool success = fixtures && alpha != NULL && bravo != NULL && charlie != NULL && missing != NULL &&
                   library_pty_environment_init("sort", &environment) &&
                   library_seed_history(&environment, entries, MEREADER_TUI_ARRAY_LEN(entries)) &&
                   library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                   library_pty_wait_for(&process, "Charlie") && library_pty_send_text(&process, "jjr") &&
                   library_pty_wait_for(&process, "library refreshed");
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "s") && library_pty_wait_for(&process, "1/3 - title") &&
                  process.output.data != NULL &&
                  strstr(process.output.data + process.output_checkpoint, "Missing") == NULL;
    }
    if (success) {
        const char *output = process.output.data + process.output_checkpoint;
        const char *bravo_position = strstr(output, "Bravo");
        const char *charlie_position = strstr(output, "Charlie");
        const char *zulu_position = strstr(output, "Zulu");
        success = bravo_position != NULL && charlie_position != NULL && zulu_position != NULL &&
                  bravo_position < charlie_position && charlie_position < zulu_position &&
                  library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "sort PTY capture: %s\n", process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(alpha);
    free(bravo);
    free(charlie);
    free(missing);
    return success;
}

static MereaderTuiTestResult test_pty_sorting_and_refresh(void) {
    TEST_ASSERT(run_pty_sorting_and_refresh());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_refresh_deleted_middle(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    bool fixtures = mereader_tui_test_write_text("library-pty/refresh-middle/alpha.epub", "alpha") &&
                    mereader_tui_test_write_text("library-pty/refresh-middle/delta.epub", "delta");
    char *alpha = mereader_tui_test_path("library-pty/refresh-middle/alpha.epub");
    char *missing = mereader_tui_test_path("library-pty/refresh-middle/missing.epub");
    char *charlie = library_create_epub("library-pty/refresh-middle/charlie.epub", "Charlie", "C",
                                        "REFRESH MIDDLE BODY");
    char *delta = mereader_tui_test_path("library-pty/refresh-middle/delta.epub");
    MereaderTuiHistoryEntry entries[] = {
        library_entry(alpha, "Alpha", "A", 0.1, "2026-07-20 12:00:00"),
        library_entry(missing, "Missing", "M", 0.2, "2026-07-19 12:00:00"),
        library_entry(charlie, "Charlie", "C", 0.3, "2026-07-18 12:00:00"),
        library_entry(delta, "Delta", "D", 0.4, "2026-07-17 12:00:00"),
    };
    bool success = fixtures && alpha != NULL && missing != NULL && charlie != NULL && delta != NULL &&
                   library_pty_environment_init("refresh-middle", &environment) &&
                   library_seed_history(&environment, entries, MEREADER_TUI_ARRAY_LEN(entries)) &&
                   library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                   library_pty_wait_for(&process, "Missing") && library_pty_send_text(&process, "jr") &&
                   library_pty_wait_for(&process, "library refreshed");
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "\n") && library_pty_wait_for(&process, "REFRESH MIDDLE BODY");
    }
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "q") && library_pty_wait_for(&process, "Charlie") &&
                  library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "refresh middle PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(alpha);
    free(missing);
    free(charlie);
    free(delta);
    return success;
}

static MereaderTuiTestResult test_pty_refresh_deleted_middle(void) {
    TEST_ASSERT(run_pty_refresh_deleted_middle());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_tiny_monochrome(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    bool fixture = mereader_tui_test_write_text("library-pty/tiny/book.epub", "tiny");
    char *book = mereader_tui_test_path("library-pty/tiny/book.epub");
    MereaderTuiHistoryEntry entry = library_entry(book, "Tiny Unicode \xe6\x9b\xb8", "Author", 0.5,
                                           "2026-07-19 12:00:00");
    static const char commands[] = {'G', 2, 6, 'g', 'g', 'q'};
    bool success = fixture && book != NULL && library_pty_environment_init("tiny", &environment) &&
                   library_seed_history(&environment, &entry, 1U) &&
                   library_pty_spawn(&environment, "vt100", 5U, 20U, NULL, &process) &&
                   library_pty_wait_for(&process, "Tiny") && process.output.data != NULL &&
                   strstr(process.output.data, "Aut") != NULL && strstr(process.output.data, "50%") != NULL &&
                   library_pty_send(&process, commands, sizeof(commands)) && library_pty_wait_exit(&process) &&
                   WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    if (!success) {
        fprintf(stderr, "tiny PTY capture: %s\n", process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(book);
    return success;
}

static MereaderTuiTestResult test_pty_tiny_monochrome(void) {
    TEST_ASSERT(run_pty_tiny_monochrome());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_tiny_height(unsigned short rows) {
    char name[32] = {0};
    (void)snprintf(name, sizeof(name), "height-%hu", rows);
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *book = library_create_epub("library-pty/heights/book.epub", "Tiny Height", "Reader",
                                     "TINY HEIGHT BODY");
    MereaderTuiHistoryEntry entry = library_entry(book, "Tiny Height", "Reader", 0.5, "2026-07-19 12:00:00");
    bool success = book != NULL && library_pty_environment_init(name, &environment) &&
                   library_seed_history(&environment, &entry, 1U) &&
                   library_pty_spawn(&environment, "xterm-256color", rows, 40U, NULL, &process) &&
                   library_pty_wait_for(&process, "?1049h") && library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, rows, 40U, "Tiny Height", NULL);
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "\n") && library_pty_wait_for(&process, "TINY HEIGHT BODY");
    }
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "q") && library_pty_wait_for(&process, "Tiny Height") &&
                  library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "height %hu PTY capture: %s\n", rows,
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(book);
    return success;
}

static MereaderTuiTestResult test_pty_height_one(void) {
    TEST_ASSERT(run_pty_tiny_height(1U));
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_pty_height_two(void) {
    TEST_ASSERT(run_pty_tiny_height(2U));
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_pty_height_three(void) {
    TEST_ASSERT(run_pty_tiny_height(3U));
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_height_one_filter_prompt(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    bool success = library_pty_environment_init("height-one-prompt", &environment) &&
                   library_configure_root(&environment, environment.temporary) &&
                   library_pty_spawn(&environment, "xterm-256color", 1U, 40U, NULL, &process) &&
                   library_pty_wait_for(&process, "?1049h") && library_pty_settle(&process);
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "/NEEDLZ") && library_pty_wait_for(&process, "Z") &&
                  library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 1U, 40U, "NEEDLZ", NULL);
    }
    if (success) {
        static const char commands[] = {27, 'q'};
        success = library_pty_send(&process, commands, sizeof(commands)) && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "height one prompt PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    return success;
}

static MereaderTuiTestResult test_pty_height_one_filter_prompt(void) {
    TEST_ASSERT(run_pty_height_one_filter_prompt());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_narrow_path_prompt(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    bool success = library_pty_environment_init("narrow-prompt", &environment) &&
                   library_configure_root(&environment, environment.temporary) &&
                   library_pty_spawn(&environment, "xterm-256color", 2U, 5U, NULL, &process) &&
                   library_pty_wait_for(&process, "?1049h") && library_pty_settle(&process);
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "oXYZ") && library_pty_wait_for(&process, "Z") &&
                  library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 2U, 5U, "Z", NULL);
    }
    if (success) {
        static const char commands[] = {27, 'q'};
        success = library_pty_send(&process, commands, sizeof(commands)) && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "narrow prompt PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    return success;
}

static MereaderTuiTestResult test_pty_narrow_path_prompt(void) {
    TEST_ASSERT(run_pty_narrow_path_prompt());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_strict_timestamps(void) {
    static const char *const titles[] = {
        "Valid Time", "Trailing Time", "Month Time", "Day Time",
        "Hour Time",  "Minute Time",   "Second Time", "Fraction Time",
    };
    static const char *const timestamps[] = {
        NULL,
        "2026-07-19 12:00:00junk",
        "2026-13-01 12:00:00",
        "2025-02-29 12:00:00",
        "2026-07-19 24:00:00",
        "2026-07-19 12:60:00",
        "2026-07-19 12:00:60",
        "2026-07-19 12:00:00.1234567",
    };
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *paths[MEREADER_TUI_ARRAY_LEN(titles)] = {0};
    MereaderTuiHistoryEntry entries[MEREADER_TUI_ARRAY_LEN(titles)] = {0};
    MereaderTuiError error = {0};
    char *now = mereader_tui_now_iso8601(&error);
    bool fixtures = now != NULL;
    for (size_t index = 0U; fixtures && index < MEREADER_TUI_ARRAY_LEN(titles); ++index) {
        char relative[128] = {0};
        (void)snprintf(relative, sizeof(relative), "library-pty/timestamps/%zu.epub", index);
        fixtures = mereader_tui_test_write_text(relative, "timestamp");
        paths[index] = fixtures ? mereader_tui_test_path(relative) : NULL;
        fixtures = fixtures && paths[index] != NULL;
        entries[index] = library_entry(paths[index], titles[index], "A", 0.5,
                                       index == 0U ? now : timestamps[index]);
    }
    bool success = fixtures && library_pty_environment_init("timestamps", &environment) &&
                   library_seed_history(&environment, entries, MEREADER_TUI_ARRAY_LEN(entries)) &&
                   library_pty_spawn(&environment, "xterm-256color", 12U, 80U, NULL, &process) &&
                   library_pty_wait_for(&process, "Valid Time") && library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 12U, 80U, "Valid Time", "now");
    for (size_t index = 1U; success && index < MEREADER_TUI_ARRAY_LEN(titles); ++index) {
        success = library_pty_screen_line_matches(&process, 12U, 80U, titles[index], "-");
    }
    if (success) {
        success = library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "timestamp PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(paths); ++index) {
        free(paths[index]);
    }
    free(now);
    return success;
}

static MereaderTuiTestResult test_pty_strict_timestamps(void) {
    TEST_ASSERT(run_pty_strict_timestamps());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_signal_restore(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    bool success = library_pty_environment_init("signal", &environment) &&
                    library_pty_spawn(&environment, "xterm-256color", 12U, 60U, NULL, &process) &&
                    library_pty_wait_for(&process, "Paste the path to your book directory") &&
                    kill(process.pid, SIGTERM) == 0 &&
                   library_pty_wait_exit(&process) && WIFEXITED(process.status) &&
                   WEXITSTATUS(process.status) == 128 + SIGTERM && process.output.data != NULL &&
                   strstr(process.output.data, "?1049l") != NULL;
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    return success;
}

static MereaderTuiTestResult test_pty_signal_restore(void) {
    TEST_ASSERT(run_pty_signal_restore());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_direct_path_bypass(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *book = library_create_epub("library-pty/direct/book.epub", "Direct Book", "Reader", "DIRECT PATH BODY");
    bool success = book != NULL && library_pty_environment_init("direct", &environment) &&
                   library_pty_spawn(&environment, "xterm-256color", 12U, 60U, book, &process) &&
                   library_pty_wait_for(&process, "DIRECT PATH BODY") && process.output.data != NULL &&
                   strstr(process.output.data, "No books yet") == NULL;
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0 &&
                  (process.output.data == NULL || strstr(process.output.data, "No books yet") == NULL);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(book);
    return success;
}

static MereaderTuiTestResult test_pty_direct_path_bypass(void) {
    TEST_ASSERT(run_pty_direct_path_bypass());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_calibre_navigation(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *epub = library_create_epub("library-pty/calibre/root/Alice Writer/First Book (42)/first.epub",
                                     "First Book", "Alice Writer", "CALIBRE EPUB BODY");
    char *pdf = library_create_pdf("library-pty/calibre/root/Alice Writer/First Book (42)/first.pdf",
                                   "CALIBRE PDF BODY");
    bool fixtures = epub != NULL && pdf != NULL &&
                    mereader_tui_test_write_text("library-pty/calibre/root/metadata.db", "") &&
                    mereader_tui_test_write_text("library-pty/calibre/root/Bob Writer/Second Book (7)/second.epub",
                                         "second");
    char *root = mereader_tui_test_path("library-pty/calibre/root");
    bool success = fixtures && root != NULL && library_pty_environment_init("calibre", &environment) &&
                    library_configure_root_with_mode(&environment, root, "placeholder") &&
                    library_pty_spawn(&environment, "xterm-256color", 14U, 80U, NULL, &process) &&
                    library_pty_wait_for(&process, "mereader-tui / all books") &&
                    library_pty_wait_for(&process, "First Book [EPUB +1]") &&
                    library_pty_settle(&process) &&
                    library_pty_screen_line_matches(&process, 14U, 80U, "? help", NULL);
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "?") && library_pty_wait_for(&process, "navigation") &&
                  library_pty_send_text(&process, "jjjjjjjjjjjjjjjj") &&
                  library_pty_wait_for(&process, "space space") && library_pty_send_text(&process, "?") &&
                  library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "mereader-tui / all books", NULL);
    }
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "v") &&
                  library_pty_wait_for(&process, " / card") && library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "mereader-tui / all books / card", NULL) &&
                  library_pty_wait_for(&process, "formats  EPUB  PDF") &&
                  library_pty_send_text(&process, "v  First") &&
                  library_pty_wait_for(&process, "find a book") && library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "First Book", NULL) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "Alice Writer", NULL);
    }
    if (success) {
        library_pty_clear_output(&process);
        success = library_pty_send_text(&process, "\n") &&
                  library_pty_wait_for(&process, "CALIBRE EPUB BODY") &&
                  library_pty_send_text(&process, "q") &&
                  library_pty_wait_for(&process, "mereader-tui / all books") &&
                  library_pty_send_text(&process, "a") &&
                  library_pty_wait_for(&process, "uthors") && library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "mereader-tui / authors", NULL) &&
                  library_pty_wait_for(&process, "Alice Writer");
    }
    if (success) {
        success = library_pty_send_text(&process, "\n") &&
                  library_pty_wait_for(&process, "First Book [EPUB +1]") && library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "mereader-tui / authors / Alice Writer", NULL);
    }
    if (success) {
        success = library_pty_send_text(&process, "f") &&
                  library_pty_wait_for(&process, "First Book [PDF]") && library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "mereader-tui / formats / First Book", NULL) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "First Book [EPUB]", NULL) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "First Book [PDF]", NULL);
    }
    if (success) {
        success = library_pty_send_text(&process, "j\n") &&
                  library_pty_wait_for(&process, "CALIBRE PDF BODY");
    }
    if (success) {
        success = library_pty_send_text(&process, "q") &&
                  library_pty_wait_for(&process, "First Book [PDF +1]") &&
                  library_format_preference_matches(&environment, root, "first.pdf") &&
                  library_pty_settle(&process) &&
                  library_pty_screen_line_matches(&process, 14U, 80U, "mereader-tui / all books", NULL) &&
                  library_pty_send_text(&process, "q") && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (success) {
        library_pty_process_free(&process);
        success = library_pty_spawn(&environment, "xterm-256color", 14U, 80U, NULL, &process) &&
                  library_pty_wait_for(&process, "First Book [PDF +1]") &&
                  library_pty_send_text(&process, "\n") &&
                  library_pty_wait_for(&process, "CALIBRE PDF BODY") &&
                  library_pty_send_text(&process, "q") &&
                  library_pty_wait_for(&process, "First Book [PDF +1]") &&
                  library_pty_send_text(&process, "q") &&
                  library_pty_wait_exit(&process) && WIFEXITED(process.status) &&
                  WEXITSTATUS(process.status) == 0 &&
                  library_format_preference_matches(&environment, root, "first.pdf");
    }
    if (!success) {
        fprintf(stderr, "calibre PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(root);
    free(epub);
    free(pdf);
    return success;
}

static MereaderTuiTestResult test_pty_calibre_navigation(void) {
    TEST_ASSERT(run_pty_calibre_navigation());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_cover_preview(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    char *epub = library_create_epub("library-pty/cover/root/Writer/Cover Book (1)/book.epub",
                                     "Cover Book", "Writer", "COVER BODY");
    bool fixtures = epub != NULL && mereader_tui_test_write_text("library-pty/cover/root/metadata.db", "") &&
                    mereader_tui_test_write("library-pty/cover/root/Writer/Cover Book (1)/cover.png",
                                    library_pixel_png, sizeof(library_pixel_png));
    char *root = mereader_tui_test_path("library-pty/cover/root");
    bool success = fixtures && root != NULL && library_pty_environment_init("cover", &environment) &&
                   library_configure_root_with_mode(&environment, root, "kitty") &&
                   library_pty_spawn(&environment, "xterm-256color", 14U, 80U, NULL, &process) &&
                   library_pty_wait_for(&process, "Cover Book") && library_pty_send_text(&process, "v") &&
                   library_pty_wait_for(&process, "a=t,f=100") && library_pty_send_text(&process, "q") &&
                   library_pty_wait_exit(&process) && WIFEXITED(process.status) &&
                   WEXITSTATUS(process.status) == 0;
    if (!success) {
        fprintf(stderr, "cover PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    free(root);
    free(epub);
    return success;
}

static MereaderTuiTestResult test_pty_cover_preview(void) {
    TEST_ASSERT(run_pty_cover_preview());
    return MEREADER_TUI_TEST_PASS;
}

static bool run_pty_picker_scroll(void) {
    LibraryPtyEnvironment environment = {0};
    LibraryPtyProcess process = {.master = -1};
    const char *titles[] = {"First Pick", "Second Pick", "Third Pick", "Fourth Pick"};
    char *paths[MEREADER_TUI_ARRAY_LEN(titles)] = {0};
    MereaderTuiHistoryEntry entries[MEREADER_TUI_ARRAY_LEN(titles)] = {0};
    bool fixtures = true;
    for (size_t index = 0U; fixtures && index < MEREADER_TUI_ARRAY_LEN(titles); ++index) {
        char relative[128] = {0};
        (void)snprintf(relative, sizeof(relative), "library-pty/picker-scroll/%zu.epub", index);
        fixtures = mereader_tui_test_write_text(relative, "pick");
        paths[index] = fixtures ? mereader_tui_test_path(relative) : NULL;
        fixtures = fixtures && paths[index] != NULL;
        entries[index] = library_entry(paths[index], titles[index], "Reader", (double)index / 10.0,
                                       "2026-07-19 12:00:00");
    }
    static const char down_twice[] = "\033OB\033OB";
    bool success = fixtures && library_pty_environment_init("picker-scroll", &environment) &&
                   library_seed_history(&environment, entries, MEREADER_TUI_ARRAY_LEN(entries)) &&
                   library_pty_spawn(&environment, "xterm-256color", 10U, 60U, NULL, &process) &&
                   library_pty_wait_for(&process, "First Pick") && library_pty_send_text(&process, "  ") &&
                   library_pty_wait_for(&process, "find a book") &&
                   library_pty_send(&process, down_twice, sizeof(down_twice) - 1U) &&
                   library_pty_settle(&process) &&
                   library_pty_screen_line_matches(&process, 10U, 60U, "Third Pick", NULL) &&
                   !library_pty_screen_line_matches(&process, 10U, 60U, "First Pick", NULL);
    if (success) {
        static const char close[] = {27, 'q'};
        success = library_pty_send(&process, close, sizeof(close)) && library_pty_wait_exit(&process) &&
                  WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
    }
    if (!success) {
        fprintf(stderr, "picker scroll PTY capture: %s\n",
                process.output.data == NULL ? "(empty)" : process.output.data);
    }
    library_pty_process_free(&process);
    library_pty_environment_free(&environment);
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(paths); ++index) {
        free(paths[index]);
    }
    return success;
}

static MereaderTuiTestResult test_pty_picker_scroll(void) {
    TEST_ASSERT(run_pty_picker_scroll());
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_library_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "projection_and_literal_filtering", .function = test_projection_and_literal_filtering},
        {.name = "sorting_and_filepath_ties", .function = test_sorting_and_filepath_ties},
        {.name = "recent_sorting_rejects_invalid_timestamps",
         .function = test_recent_sorting_rejects_invalid_timestamps},
        {.name = "detached_worker_exit_signal_mask", .function = test_detached_worker_exit_signal_mask},
        {.name = "unicode_and_control_sanitization", .function = test_unicode_and_control_sanitization},
        {.name = "empty_missing_and_fallback_rows", .function = test_empty_missing_and_fallback_rows},
        {.name = "selection_preservation", .function = test_selection_preservation},
        {.name = "deleted_middle_selection_preservation", .function = test_deleted_middle_selection_preservation},
        {.name = "pty_empty_library", .function = test_pty_empty_library},
        {.name = "pty_open_return_and_typed_path", .function = test_pty_open_return_and_typed_path},
        {.name = "pty_custom_library_keymaps", .function = test_pty_custom_library_keymaps},
        {.name = "pty_live_resize", .function = test_pty_live_resize},
        {.name = "pty_progress_save_failure", .function = test_pty_progress_save_failure},
        {.name = "pty_literal_filtering", .function = test_pty_literal_filtering},
        {.name = "pty_sorting_and_refresh", .function = test_pty_sorting_and_refresh},
        {.name = "pty_refresh_deleted_middle", .function = test_pty_refresh_deleted_middle},
        {.name = "pty_tiny_monochrome", .function = test_pty_tiny_monochrome},
        {.name = "pty_height_one", .function = test_pty_height_one},
        {.name = "pty_height_two", .function = test_pty_height_two},
        {.name = "pty_height_three", .function = test_pty_height_three},
        {.name = "pty_height_one_filter_prompt", .function = test_pty_height_one_filter_prompt},
        {.name = "pty_narrow_path_prompt", .function = test_pty_narrow_path_prompt},
        {.name = "pty_strict_timestamps", .function = test_pty_strict_timestamps},
        {.name = "pty_signal_restore", .function = test_pty_signal_restore},
        {.name = "pty_direct_path_bypass", .function = test_pty_direct_path_bypass},
        {.name = "pty_calibre_navigation", .function = test_pty_calibre_navigation},
        {.name = "pty_cover_preview", .function = test_pty_cover_preview},
        {.name = "pty_picker_scroll", .function = test_pty_picker_scroll},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
