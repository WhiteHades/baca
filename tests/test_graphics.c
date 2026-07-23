#include "test_support.h"

#include "baca/app.h"
#include "baca/graphics.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <zip.h>

static const unsigned char graphics_pixel_png[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 0x04U, 0x00U, 0x00U, 0x00U, 0xb5U,
    0x1cU, 0x0cU, 0x02U, 0x00U, 0x00U, 0x00U, 0x0bU, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xdaU, 0x63U, 0x64U, 0xf8U, 0x0fU, 0x00U, 0x01U, 0x05U,
    0x01U, 0x01U, 0x27U, 0x18U, 0xe3U, 0x66U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
};

static const unsigned char graphics_two_frame_gif[] = {
    0x47U, 0x49U, 0x46U, 0x38U, 0x39U, 0x61U, 0x01U, 0x00U, 0x01U, 0x00U,
    0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xffU, 0xffU, 0xffU,
    0x21U, 0xf9U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x2cU, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x02U, 0x02U, 0x44U, 0x01U, 0x00U,
    0x21U, 0xf9U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x2cU, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x02U, 0x02U, 0x44U, 0x01U, 0x00U, 0x3bU,
};

static const char graphics_tall_svg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='32768'>"
    "<rect width='100%' height='100%' fill='red'/></svg>";

static const char graphics_wide_svg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='32768' height='1'>"
    "<rect width='100%' height='100%' fill='red'/></svg>";

typedef struct Capture {
  BacaString output;
  size_t maximum_write;
  bool fail_writes;
  bool failed;
} Capture;

typedef struct CellCapture {
  int count;
  int row;
  int column;
  uint32_t foreground;
  uint32_t background;
} CellCapture;

typedef struct FdWriter {
  int fd;
} FdWriter;

typedef struct ColorPageRenderer {
  const char *color;
  size_t renders;
} ColorPageRenderer;

static bool capture_write(void *user_data, const void *data, size_t length) {
  Capture *capture = user_data;
  if (capture->fail_writes) {
    return false;
  }
  if (length > capture->maximum_write) {
    capture->maximum_write = length;
  }
  BacaError error = {0};
  if (!baca_string_append_n(&capture->output, data, length, &error)) {
    capture->failed = true;
    return false;
  }
  return true;
}

static bool capture_cell(void *user_data, int row, int column,
                         uint32_t foreground, uint32_t background) {
  CellCapture *capture = user_data;
  ++capture->count;
  capture->row = row;
  capture->column = column;
  capture->foreground = foreground;
  capture->background = background;
  return true;
}

static bool fd_write_all(void *user_data, const void *data, size_t length) {
  const FdWriter *writer = user_data;
  const unsigned char *cursor = data;
  while (length > 0U) {
    const ssize_t written = write(writer->fd, cursor, length);
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

static bool image_test_opener(void *user_data, const char *target,
                              const char *preferred, BacaError *error) {
  FdWriter writer = {.fd = *(const int *)user_data};
  const char *viewer = preferred == NULL ? "" : preferred;
  if (!fd_write_all(&writer, target, strlen(target)) ||
      !fd_write_all(&writer, "\n", 1U) ||
      !fd_write_all(&writer, viewer, strlen(viewer)) ||
      !fd_write_all(&writer, "\n", 1U)) {
    baca_error_set(error, BACA_ERROR_IO,
                   "could not capture standalone image opener target");
    return false;
  }
  return true;
}

int baca_image_pty_child(void) {
  const char *path = getenv("BACA_IMAGE_PTY_PATH");
  const char *held_path = getenv("BACA_IMAGE_PTY_HELD_PATH");
  const char *screenshot_directory = getenv("BACA_IMAGE_PTY_SCREENSHOT_DIR");
  const char *descriptor_value = getenv("BACA_IMAGE_PTY_OPEN_FD");
  if (path == NULL || held_path == NULL || screenshot_directory == NULL ||
      descriptor_value == NULL ||
      sizeof(graphics_tall_svg) != sizeof(graphics_wide_svg)) {
    return 131;
  }
  char *end = NULL;
  const long parsed = strtol(descriptor_value, &end, 10);
  if (end == descriptor_value || *end != '\0' || parsed < 0 ||
      parsed > INT_MAX) {
    return 132;
  }
  const int descriptor = (int)parsed;
  BacaApp app = {0};
  BacaError error = {0};
  if (!baca_app_init(&app, path, true, &error)) {
    (void)fprintf(stderr, "image PTY init: %s\n", error.message);
    return 133;
  }
  if (rename(path, held_path) != 0 ||
      !baca_write_file(path, graphics_wide_svg,
                       sizeof(graphics_wide_svg) - 1U, &error) ||
      chdir(screenshot_directory) != 0) {
    (void)fprintf(stderr, "image PTY replacement: %s\n",
                  error.message[0] == '\0' ? strerror(errno) : error.message);
    (void)baca_app_free(&app, NULL);
    return 134;
  }
  app.external_opener = image_test_opener;
  app.external_opener_data = (void *)&descriptor;
  const int run_result = baca_app_run(&app, &error);
  BacaError free_error = {0};
  if (!baca_app_free(&app, &free_error)) {
    (void)fprintf(stderr, "image PTY save: %s\n", free_error.message);
    return 135;
  }
  return run_result;
}

static bool zip_add_fixture(zip_t *archive, const char *name, const void *data,
                            size_t length, bool stored) {
  zip_source_t *source = zip_source_buffer(archive, data, length, 0);
  if (source == NULL) {
    return false;
  }
  const zip_int64_t index =
      zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
  if (index < 0) {
    zip_source_free(source);
    return false;
  }
  return !stored || zip_set_file_compression(
                        archive, (zip_uint64_t)index, ZIP_CM_STORE, 0U) == 0;
}

static char *create_tui_epub(void) {
  static const char mimetype[] = "application/epub+zip";
  static const char container[] =
      "<?xml version=\"1.0\"?>"
      "<container version=\"1.0\" "
      "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
      "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
      "media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles></container>";
  static const char opf[] =
      "<?xml version=\"1.0\"?>"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" "
      "unique-identifier=\"id\"><metadata "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
      "<dc:title>PTY image</dc:title><dc:identifier id=\"id\">pty</dc:identifier>"
      "</metadata><manifest>"
      "<item id=\"chapter\" href=\"chapter.xhtml\" "
      "media-type=\"application/xhtml+xml\"/>"
      "<item id=\"pixel\" href=\"pixel.png\" media-type=\"image/png\"/>"
      "</manifest><spine><itemref idref=\"chapter\"/></spine></package>";
  static const char chapter[] =
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
      "<img src=\"pixel.png\" alt=\"PTY\"/></body></html>";

  if (!baca_test_mkdir("tui-pty")) {
    return NULL;
  }
  char *path = baca_test_path("tui-pty/image.epub");
  if (path == NULL) {
    return NULL;
  }
  int zip_error = 0;
  zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
  if (archive == NULL) {
    free(path);
    return NULL;
  }
  const bool added =
      zip_add_fixture(archive, "mimetype", mimetype, sizeof(mimetype) - 1U,
                      true) &&
      zip_add_fixture(archive, "META-INF/container.xml", container,
                      sizeof(container) - 1U, false) &&
      zip_add_fixture(archive, "OEBPS/content.opf", opf, sizeof(opf) - 1U,
                      false) &&
      zip_add_fixture(archive, "OEBPS/chapter.xhtml", chapter,
                      sizeof(chapter) - 1U, false) &&
      zip_add_fixture(archive, "OEBPS/pixel.png", graphics_pixel_png,
                      sizeof(graphics_pixel_png), false);
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

static bool read_pty_capture(int fd, BacaString *output) {
  char buffer[4096] = {0};
  BacaError error = {0};
  for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    const int ready = poll(&descriptor, 1U, output->length == 0U ? 100 : 10);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
      return output->length > 0U;
    }
    const ssize_t length = read(fd, buffer, sizeof(buffer));
    if (length > 0 &&
        !baca_string_append_n(output, buffer, (size_t)length, &error)) {
      return false;
    }
    if (length <= 0 && errno != EINTR) {
      return output->length > 0U;
    }
  }
  return output->length > 0U;
}

static bool drain_pty(int fd, BacaString *output) {
  char buffer[4096] = {0};
  BacaError error = {0};
  for (;;) {
    const ssize_t length = read(fd, buffer, sizeof(buffer));
    if (length > 0) {
      if (!baca_string_append_n(output, buffer, (size_t)length, &error)) {
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

static bool graphics_wait_for(int descriptor, BacaString *output, size_t start,
                              const char *needle) {
  for (unsigned attempt = 0U; attempt < 300U; ++attempt) {
    struct pollfd poll_descriptor = {.fd = descriptor, .events = POLLIN};
    const int ready = poll(&poll_descriptor, 1U, 20);
    if (ready < 0 && errno != EINTR) {
      return false;
    }
    if (!drain_pty(descriptor, output)) {
      return false;
    }
    if (output->data != NULL && start <= output->length &&
        strstr(output->data + start, needle) != NULL) {
      return true;
    }
  }
  return false;
}

static bool graphics_drain_until_idle(int descriptor, BacaString *output) {
  for (unsigned attempt = 0U; attempt < 30U; ++attempt) {
    struct pollfd poll_descriptor = {.fd = descriptor, .events = POLLIN};
    const int ready = poll(&poll_descriptor, 1U, 50);
    if (ready < 0 && errno != EINTR) {
      return false;
    }
    if (!drain_pty(descriptor, output)) {
      return false;
    }
    if (ready == 0) {
      return true;
    }
  }
  return false;
}

static bool graphics_send_mouse_click(int descriptor, int x, int y) {
  char sequence[96] = {0};
  const int length = snprintf(sequence, sizeof(sequence),
                              "\033[<0;%d;%dM\033[<0;%d;%dm", x + 1,
                              y + 1, x + 1, y + 1);
  FdWriter writer = {.fd = descriptor};
  return length > 0 && (size_t)length < sizeof(sequence) &&
         fd_write_all(&writer, sequence, (size_t)length);
}

static bool graphics_screenshot_contains(const char *directory,
                                         const char *needle) {
  DIR *opened = opendir(directory);
  if (opened == NULL) {
    return false;
  }
  bool found = false;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(opened);
    if (entry == NULL) {
      break;
    }
    const size_t length = strlen(entry->d_name);
    if (length < 17U || strncmp(entry->d_name, "mereader-tui_", 13U) != 0 ||
        strcmp(entry->d_name + length - 4U, ".svg") != 0) {
      continue;
    }
    BacaError error = {0};
    char *path = baca_path_join(directory, entry->d_name, &error);
    BacaBuffer contents = {0};
    if (path != NULL && baca_read_file(path, &contents, &error) &&
        contents.data != NULL &&
        strstr((const char *)contents.data, needle) != NULL) {
      found = true;
    }
    baca_buffer_free(&contents);
    free(path);
    break;
  }
  const int saved_errno = errno;
  const bool closed = closedir(opened) == 0;
  return found && saved_errno == 0 && closed;
}

static bool run_tui_pty_capture(BacaImageMode mode, unsigned short columns,
                                 const char *marker, BacaString *output) {
  const char *config_relative = mode == BACA_IMAGE_MODE_KITTY
                                     ? "tui-pty/kitty/mereader-tui/config.ini"
                                     : "tui-pty/ansi/mereader-tui/config.ini";
  const char *config_text =
      mode == BACA_IMAGE_MODE_KITTY
          ? columns > BACA_GRAPHICS_MAX_COLUMNS
                ? "[General]\nImageMode = kitty\nMaxTextWidth = 2000\n"
                : "[General]\nImageMode = kitty\n"
          : "[General]\nImageMode = ansi\n";
  char *image_path = create_tui_epub();
  if (image_path == NULL ||
      !baca_test_write_text(config_relative, config_text)) {
    free(image_path);
    return false;
  }
  char *config_root = baca_test_path(mode == BACA_IMAGE_MODE_KITTY
                                         ? "tui-pty/kitty"
                                         : "tui-pty/ansi");
  if (config_root == NULL) {
    free(image_path);
    return false;
  }

  struct winsize size = {.ws_row = 12U, .ws_col = columns};
  int master = -1;
  (void)fflush(NULL);
  const pid_t child = forkpty(&master, NULL, NULL, &size);
  if (child < 0) {
    free(image_path);
    free(config_root);
    return false;
  }
  if (child == 0) {
    (void)setenv("TERM", "xterm-256color", 1);
    (void)setenv("COLORTERM", "truecolor", 1);
    (void)unsetenv("TMUX");
    (void)unsetenv("STY");
    if (mode == BACA_IMAGE_MODE_KITTY) {
      (void)setenv("TERM_PROGRAM", "kitty", 1);
      (void)setenv("KITTY_WINDOW_ID", "1", 1);
    } else {
      (void)unsetenv("TERM_PROGRAM");
      (void)unsetenv("KITTY_WINDOW_ID");
    }
    (void)setenv("XDG_CONFIG_HOME", config_root, 1);
    (void)execl("./build/mereader-tui", "mereader-tui", image_path, (char *)NULL);
    _exit(127);
  }
  free(image_path);
  free(config_root);

  if (fcntl(master, F_SETFL, O_NONBLOCK) != 0) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, NULL, 0);
    (void)close(master);
    return false;
  }
  bool sent_quit = false;
  bool completed = false;
  int status = 0;
  for (unsigned attempt = 0U; attempt < 600U; ++attempt) {
    struct pollfd descriptor = {.fd = master, .events = POLLIN};
    const int ready = poll(&descriptor, 1U, 20);
    if (ready < 0 && errno != EINTR) {
      break;
    }
    if (!drain_pty(master, output)) {
      break;
    }
    if (!sent_quit && output->data != NULL &&
        strstr(output->data, marker) != NULL) {
      const char quit = 'q';
      if (write(master, &quit, 1U) != 1) {
        break;
      }
      sent_quit = true;
    }
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      completed = true;
      break;
    }
    if (waited < 0) {
      break;
    }
    if ((descriptor.revents & POLLHUP) != 0) {
      (void)poll(NULL, 0U, 20);
    }
  }
  if (!completed) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
  }
  (void)drain_pty(master, output);
  (void)close(master);
  return completed && sent_quit && WIFEXITED(status) &&
         WEXITSTATUS(status) == EXIT_SUCCESS;
}

static size_t count_bytes(const char *haystack, const char *needle) {
  size_t count = 0U;
  const size_t needle_length = strlen(needle);
  for (const char *cursor = haystack; (cursor = strstr(cursor, needle)) != NULL;
       cursor += needle_length) {
    ++count;
  }
  return count;
}

static bool append_image_uri(BacaDocument *document, const char *uri,
                             const char *alt, BacaError *error) {
  BacaBlock block = {
      .kind = BACA_BLOCK_IMAGE,
      .value.image =
          {
              .uri = baca_strdup(uri, error),
              .alt = baca_strdup(alt, error),
              .page_index = -1,
              .intrinsic_width = 1,
              .intrinsic_height = 1,
          },
  };
  if (block.value.image.uri != NULL && block.value.image.alt != NULL &&
      baca_document_add_image_block(document, &block, error)) {
    return true;
  }
  free(block.value.image.uri);
  free(block.value.image.alt);
  return false;
}

static bool append_cache_image(BacaDocument *document, const char *alt,
                                BacaError *error) {
  static const char data_uri[] =
      "data:image/"
      "png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+"
      "A8AAQUB"
      "AScY42YAAAAASUVORK5CYII=";
  return append_image_uri(document, data_uri, alt, error);
}

static bool append_cache_svg(BacaDocument *document, const char *color,
                             BacaError *error) {
  char uri[256] = {0};
  const int length = snprintf(
      uri, sizeof(uri),
      "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='1' "
      "height='1'><rect width='1' height='1' fill='%s'/></svg>",
      color);
  return length > 0 && (size_t)length < sizeof(uri) &&
         append_image_uri(document, uri, color, error);
}

static bool render_color_page(BacaDocument *document, int page_index, int width,
                              int height, uint32_t background,
                              BacaResource *resource, BacaError *error) {
  (void)background;
  ColorPageRenderer *renderer = document->backend;
  char svg[256] = {0};
  const int length = snprintf(
      svg, sizeof(svg),
      "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d'>"
      "<rect width='100%%' height='100%%' fill='%s'/></svg>",
      width, height, renderer->color);
  if (page_index != 0 || length <= 0 || (size_t)length >= sizeof(svg)) {
    baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid color page render");
    return false;
  }
  resource->data = (unsigned char *)baca_strndup(svg, (size_t)length, error);
  resource->mime_type = baca_strdup("image/svg+xml", error);
  if (resource->data == NULL || resource->mime_type == NULL) {
    baca_resource_free(resource);
    return false;
  }
  resource->length = (size_t)length;
  ++renderer->renders;
  return true;
}

static const BacaDocumentOps color_page_ops = {
    .render_page = render_color_page,
};

static bool append_color_page(BacaDocument *document,
                              ColorPageRenderer *renderer,
                              BacaError *error) {
  if (!append_image_uri(document, "page://0", renderer->color, error)) {
    return false;
  }
  document->blocks[0].value.image.page_index = 0;
  document->backend = renderer;
  document->ops = &color_page_ops;
  return true;
}

static BacaTestResult test_mode_selection(void) {
  BacaGraphicsEnvironment environment = {
      .term_program = "ghostty",
      .term = "xterm-256color",
      .colorterm = "truecolor",
      .colors = true,
      .output_is_tty = true,
  };
  TEST_ASSERT_INT(
      baca_graphics_select_mode(BACA_IMAGE_MODE_AUTO, false, &environment),
      BACA_IMAGE_MODE_KITTY);
  TEST_ASSERT(baca_graphics_truecolor_available(&environment));

  environment.tmux = "/tmp/tmux-1/default,1,0";
  TEST_ASSERT_INT(baca_graphics_multiplexer(&environment),
                  BACA_GRAPHICS_MULTIPLEXER_TMUX);
  TEST_ASSERT_INT(
      baca_graphics_select_mode(BACA_IMAGE_MODE_AUTO, false, &environment),
      BACA_IMAGE_MODE_ANSI);
  TEST_ASSERT_INT(
      baca_graphics_select_mode(BACA_IMAGE_MODE_KITTY, true, &environment),
      BACA_IMAGE_MODE_KITTY);

  environment.tmux = NULL;
  environment.term_program = NULL;
  environment.term = "screen-256color";
  environment.sty = "123.test";
  TEST_ASSERT_INT(baca_graphics_multiplexer(&environment),
                  BACA_GRAPHICS_MULTIPLEXER_SCREEN);
  TEST_ASSERT_INT(
      baca_graphics_select_mode(BACA_IMAGE_MODE_AUTO, false, &environment),
      BACA_IMAGE_MODE_ANSI);

  environment.sty = NULL;
  environment.term = "xterm-256color";
  environment.kitty_window_id = "4";
  TEST_ASSERT_INT(
      baca_graphics_select_mode(BACA_IMAGE_MODE_AUTO, false, &environment),
      BACA_IMAGE_MODE_KITTY);
  environment.kitty_window_id = NULL;
  TEST_ASSERT_INT(
      baca_graphics_select_mode(BACA_IMAGE_MODE_ANSI, true, &environment),
      BACA_IMAGE_MODE_ANSI);
  environment.colors = false;
  TEST_ASSERT_INT(
      baca_graphics_select_mode(BACA_IMAGE_MODE_ANSI, true, &environment),
      BACA_IMAGE_MODE_PLACEHOLDER);
  environment.output_is_tty = false;
  TEST_ASSERT_INT(
      baca_graphics_select_mode(BACA_IMAGE_MODE_KITTY, true, &environment),
      BACA_IMAGE_MODE_PLACEHOLDER);
  TEST_ASSERT_INT(baca_graphics_select_mode(BACA_IMAGE_MODE_PLACEHOLDER, true,
                                            &environment),
                  BACA_IMAGE_MODE_PLACEHOLDER);
  return BACA_TEST_PASS;
}

static BacaTestResult test_dimension_probing_and_malformed_resources(void) {
  BacaResource resource = {
      .data = (unsigned char *)graphics_pixel_png,
      .length = sizeof(graphics_pixel_png),
      .mime_type = "image/png",
  };
  BacaError error = {0};
  int width = 0;
  int height = 0;
  TEST_ASSERT_MSG(
      baca_graphics_probe_resource(&resource, &width, &height, &error), "%s",
      error.message);
  TEST_ASSERT_INT(width, 1);
  TEST_ASSERT_INT(height, 1);

  static const unsigned char svg[] =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"7\" "
      "height=\"3\"><rect width=\"7\" height=\"3\"/></svg>";
  resource = (BacaResource){
      .data = (unsigned char *)svg,
      .length = sizeof(svg) - 1U,
      .mime_type = "image/svg+xml",
  };
  TEST_ASSERT_MSG(
      baca_graphics_probe_resource(&resource, &width, &height, &error), "%s",
      error.message);
  TEST_ASSERT_INT(width, 7);
  TEST_ASSERT_INT(height, 3);

  resource = (BacaResource){
      .data = (unsigned char *)"not an image",
      .length = 12U,
      .mime_type = "image/png",
  };
  TEST_ASSERT(
      !baca_graphics_probe_resource(&resource, &width, &height, &error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  TEST_ASSERT_INT(width, 0);
  TEST_ASSERT_INT(height, 0);

  resource = (BacaResource){
      .data = (unsigned char *)graphics_two_frame_gif,
      .length = sizeof(graphics_two_frame_gif),
      .mime_type = "image/gif",
  };
  TEST_ASSERT(!baca_graphics_probe_resource(&resource, &width, &height, &error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
  TEST_ASSERT_INT(width, 0);
  TEST_ASSERT_INT(height, 0);

  static const unsigned char oversized_svg[] =
      "<svg xmlns='http://www.w3.org/2000/svg' width='32769' height='1'/>";
  resource = (BacaResource){
      .data = (unsigned char *)oversized_svg,
      .length = sizeof(oversized_svg) - 1U,
      .mime_type = "image/svg+xml",
  };
  TEST_ASSERT(!baca_graphics_probe_resource(&resource, &width, &height, &error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);

  unsigned char byte = 0U;
  resource = (BacaResource){
      .data = &byte,
      .length = BACA_GRAPHICS_MAX_INPUT_BYTES + 1U,
      .mime_type = "image/png",
  };
  TEST_ASSERT(!baca_graphics_probe_resource(&resource, &width, &height, &error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
  return BACA_TEST_PASS;
}

static BacaTestResult test_alpha_compositing(void) {
  const unsigned char half_red[4] = {255U, 0U, 0U, 128U};
  unsigned char output[3] = {0};
  baca_graphics_composite_pixel(0x0000ffU, half_red, output);
  TEST_ASSERT_INT(output[0], 128);
  TEST_ASSERT_INT(output[1], 0);
  TEST_ASSERT_INT(output[2], 127);

  const unsigned char transparent[4] = {1U, 2U, 3U, 0U};
  baca_graphics_composite_pixel(0x123456U, transparent, output);
  TEST_ASSERT_INT(output[0], 0x12);
  TEST_ASSERT_INT(output[1], 0x34);
  TEST_ASSERT_INT(output[2], 0x56);

  static const char alpha_svg[] =
      "data:image/svg+xml,<svg xmlns=\"http://www.w3.org/2000/svg\" "
      "width=\"1\" height=\"1\">"
      "<rect width=\"1\" height=\"1\" fill=\"%23ff0000\" "
      "fill-opacity=\"0.5\"/></svg>";
  BacaDocument document = {0};
  BacaError error = {0};
  TEST_ASSERT(append_image_uri(&document, alpha_svg, "alpha", &error));
  BacaGraphicsContext *context = baca_graphics_create(
      1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0x0000ffU, &error);
  TEST_ASSERT(context != NULL);
  BacaGraphicsSurface surface = {0};
  TEST_ASSERT_MSG(
      baca_graphics_prepare(context, &document, 0U, 1, 1, &surface, &error),
      "%s", error.message);
  TEST_ASSERT_INT(surface.pixels[0], 128);
  TEST_ASSERT_INT(surface.pixels[1], 0);
  TEST_ASSERT_INT(surface.pixels[2], 127);
  TEST_ASSERT(baca_graphics_set_background(context, 0xffffffU, &error));
  TEST_ASSERT_INT(surface.pixels[0], 128);
  baca_graphics_surface_release(&surface);
  TEST_ASSERT(
      baca_graphics_prepare(context, &document, 0U, 1, 1, &surface, &error));
  TEST_ASSERT_INT(surface.pixels[0], 255);
  TEST_ASSERT_INT(surface.pixels[1], 127);
  TEST_ASSERT_INT(surface.pixels[2], 127);
  baca_graphics_surface_release(&surface);
  baca_graphics_free(context);
  baca_document_close(&document);
  return BACA_TEST_PASS;
}

static BacaTestResult test_ansi_output_and_clipping(void) {
  static const unsigned char pixels[] = {
      255U, 0U,   0U,   255U, 0U,   0U,   255U, 0U,   0U,   0U,   0U,   255U,
      0U,   0U,   255U, 0U,   0U,   255U, 0U,   255U, 0U,   0U,   255U, 0U,
      0U,   255U, 0U,   255U, 255U, 255U, 255U, 255U, 255U, 255U, 255U, 255U,
  };
  const BacaGraphicsSurface surface = {
      .pixels = pixels,
      .pixel_bytes = sizeof(pixels),
      .width = 3,
      .height = 4,
      .rowstride = 9,
      .image_id = 9U,
  };
  const BacaGraphicsRect cover = {
      .row = 0, .column = 2, .rows = 1, .columns = 1};
  const BacaGraphicsPlacement placement = {
      .row = -1,
      .column = 1,
      .viewport_rows = 2,
      .viewport_columns = 3,
      .occlusions = &cover,
      .occlusion_count = 1U,
  };
  Capture capture = {0};
  BacaError error = {0};
  TEST_ASSERT_MSG(baca_graphics_render_ansi(&surface, &placement, capture_write,
                                            &capture, &error),
                  "%s", error.message);
  TEST_ASSERT(!capture.failed);
  TEST_ASSERT(strstr(capture.output.data, "\0337\033[1;2H") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "\033[38;2;0;255;0m") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "\033[48;2;255;255;255m") != NULL);
  TEST_ASSERT_SIZE(count_bytes(capture.output.data, "\xe2\x96\x80"), 1U);
  TEST_ASSERT(strstr(capture.output.data, "\033[0m\0338") != NULL);

  CellCapture cells = {0};
  TEST_ASSERT(baca_graphics_render_cells(&surface, &placement, capture_cell,
                                         &cells, &error));
  TEST_ASSERT_INT(cells.count, 1);
  TEST_ASSERT_INT(cells.row, 0);
  TEST_ASSERT_INT(cells.column, 1);
  TEST_ASSERT_INT((int)cells.foreground, 0x00ff00);
  TEST_ASSERT_INT((int)cells.background, 0xffffff);
  baca_string_free(&capture.output);
  return BACA_TEST_PASS;
}

static BacaTestResult test_kitty_chunking_place_and_delete(void) {
  BacaError error = {0};
  BacaGraphicsContext *context = baca_graphics_create(
      1024U * 1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
  TEST_ASSERT(context != NULL);
  unsigned char png[7000] = {0};
  memcpy(png, "\x89PNG\r\n\x1a\n", 8U);
  for (size_t index = 8U; index < sizeof(png); ++index) {
    png[index] = (unsigned char)(index * 37U + 11U);
  }
  Capture capture = {0};
  TEST_ASSERT_MSG(baca_graphics_kitty_transmit(context, 77U, png, sizeof(png),
                                               capture_write, &capture, &error),
                  "%s", error.message);
  TEST_ASSERT(strstr(capture.output.data,
                     "\033_Ga=t,f=100,i=77,q=2,m=1;iVBORw0K") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "a=T") == NULL);
  TEST_ASSERT(strstr(capture.output.data, "\033_Gm=1;") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "\033_Gm=0;") != NULL);
  TEST_ASSERT_SIZE(count_bytes(capture.output.data, "\033_G"), 3U);

  const char *cursor = capture.output.data;
  for (size_t chunk = 0U; chunk < 3U; ++chunk) {
    const char *command = strstr(cursor, "\033_G");
    TEST_ASSERT(command != NULL);
    const char *payload = strchr(command, ';');
    const char *end = payload == NULL ? NULL : strstr(payload + 1, "\033\\");
    TEST_ASSERT(payload != NULL && end != NULL);
    TEST_ASSERT((size_t)(end - payload - 1) <= BACA_GRAPHICS_KITTY_CHUNK_BYTES);
    cursor = end + 2;
  }
  baca_string_free(&capture.output);

  static const unsigned char pixels[4U * 8U * 3U] = {0};
  const BacaGraphicsSurface surface = {
      .pixels = pixels,
      .pixel_bytes = sizeof(pixels),
      .width = 4,
      .height = 8,
      .rowstride = 12,
      .image_id = 77U,
  };
  const BacaGraphicsPlacement placement = {
      .row = -1,
      .column = 2,
      .viewport_rows = 3,
      .viewport_columns = 8,
  };
  TEST_ASSERT(baca_graphics_kitty_place(context, &surface, &placement,
                                        capture_write, &capture, &error));
  TEST_ASSERT(strstr(capture.output.data, "\0337\033[1;3H") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "a=p,i=77,") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "q=2,x=0,y=2,w=4,h=6,c=4,r=3,C=1") !=
              NULL);
  baca_string_free(&capture.output);

  TEST_ASSERT(baca_graphics_kitty_delete_placements(context, capture_write,
                                                     &capture, &error));
  TEST_ASSERT(strstr(capture.output.data, "a=d,d=i,i=77,p=") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "d=a") == NULL);
  baca_string_free(&capture.output);
  TEST_ASSERT(
      baca_graphics_kitty_delete_all(context, capture_write, &capture, &error));
  TEST_ASSERT(strstr(capture.output.data, "a=d,d=I,i=77,q=2") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "d=A") == NULL);
  baca_string_free(&capture.output);
  baca_graphics_free(context);

  context =
      baca_graphics_create(1024U, BACA_GRAPHICS_MULTIPLEXER_TMUX, 0U, &error);
  TEST_ASSERT(context != NULL);
  TEST_ASSERT(baca_graphics_kitty_transmit(context, 77U, png, 64U,
                                           capture_write, &capture, &error));
  TEST_ASSERT(baca_graphics_kitty_place(context, &surface, &placement,
                                        capture_write, &capture, &error));
  TEST_ASSERT(baca_graphics_kitty_delete_all(context, capture_write, &capture,
                                             &error));
  TEST_ASSERT(strstr(capture.output.data, "\033Ptmux;") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "d=i,i=77") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "d=I,i=77") != NULL);
  baca_string_free(&capture.output);
  baca_graphics_free(context);

  context =
      baca_graphics_create(1024U, BACA_GRAPHICS_MULTIPLEXER_SCREEN, 0U, &error);
  TEST_ASSERT(context != NULL);
  capture.maximum_write = 0U;
  unsigned char screen_png[1000] = {0};
  TEST_ASSERT(baca_graphics_kitty_transmit(
      context, 77U, screen_png, sizeof(screen_png), capture_write, &capture,
      &error));
  TEST_ASSERT(baca_graphics_kitty_place(context, &surface, &placement,
                                        capture_write, &capture, &error));
  TEST_ASSERT(capture.maximum_write < BACA_GRAPHICS_KITTY_SCREEN_SEQUENCE_BYTES);
  TEST_ASSERT(strstr(capture.output.data, "a=t,f=100,i=77") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "a=p,i=77") != NULL);
  baca_string_free(&capture.output);
  TEST_ASSERT(baca_graphics_kitty_delete_all(context, capture_write, &capture,
                                             &error));
  baca_string_free(&capture.output);
  baca_graphics_free(context);
  return BACA_TEST_PASS;
}

static BacaTestResult test_repeated_uri_dedupe_and_context_ids(void) {
  BacaDocument document = {0};
  BacaError error = {0};
  TEST_ASSERT(append_cache_image(&document, "one", &error));
  TEST_ASSERT(append_cache_image(&document, "two", &error));
  BacaGraphicsContext *first_context = baca_graphics_create(
      1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
  BacaGraphicsContext *second_context = baca_graphics_create(
      1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
  TEST_ASSERT(first_context != NULL && second_context != NULL);

  BacaGraphicsSurface first = {0};
  BacaGraphicsSurface repeated = {0};
  BacaGraphicsSurface other_context = {0};
  TEST_ASSERT(baca_graphics_prepare(first_context, &document, 0U, 4, 2,
                                    &first, &error));
  TEST_ASSERT(baca_graphics_prepare(first_context, &document, 1U, 4, 2,
                                    &repeated, &error));
  TEST_ASSERT_SIZE(baca_graphics_cache_stats(first_context).entries, 1U);
  TEST_ASSERT_INT((int)repeated.image_id, (int)first.image_id);

  TEST_ASSERT(baca_graphics_prepare(second_context, &document, 0U, 4, 2,
                                    &other_context, &error));
  TEST_ASSERT(other_context.image_id != first.image_id);

  baca_graphics_surface_release(&other_context);
  baca_graphics_surface_release(&repeated);
  baca_graphics_surface_release(&first);
  baca_graphics_free(second_context);
  baca_graphics_free(first_context);
  baca_document_close(&document);
  return BACA_TEST_PASS;
}

static BacaTestResult test_cache_is_scoped_to_document_address_and_identity(void) {
  BacaDocument red_document = {0};
  BacaDocument blue_document = {0};
  ColorPageRenderer red_renderer = {.color = "red"};
  ColorPageRenderer blue_renderer = {.color = "blue"};
  BacaError error = {0};
  TEST_ASSERT(append_color_page(&red_document, &red_renderer, &error));
  TEST_ASSERT(append_color_page(&blue_document, &blue_renderer, &error));

  BacaGraphicsContext *context = baca_graphics_create(
      1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
  TEST_ASSERT(context != NULL);
  BacaGraphicsSurface red = {0};
  BacaGraphicsSurface blue = {0};
  TEST_ASSERT_MSG(baca_graphics_prepare(context, &red_document, 0U, 1, 1,
                                        &red, &error),
                  "%s", error.message);
  TEST_ASSERT_MSG(baca_graphics_prepare(context, &blue_document, 0U, 1, 1,
                                        &blue, &error),
                  "%s", error.message);
  TEST_ASSERT(red.pixels[0] > 200U && red.pixels[1] < 50U &&
              red.pixels[2] < 50U);
  TEST_ASSERT(blue.pixels[0] < 50U && blue.pixels[1] < 50U &&
              blue.pixels[2] > 200U);
  TEST_ASSERT_SIZE(red_renderer.renders, 1U);
  TEST_ASSERT_SIZE(blue_renderer.renders, 1U);
  TEST_ASSERT_SIZE(baca_graphics_cache_stats(context).entries, 2U);
  TEST_ASSERT(red.image_id != blue.image_id);

  red_document.instance_id = 1U;
  red_renderer.color = "#00ff00";
  BacaGraphicsSurface reopened = {0};
  TEST_ASSERT_MSG(baca_graphics_prepare(context, &red_document, 0U, 1, 1,
                                        &reopened, &error),
                  "%s", error.message);
  TEST_ASSERT(reopened.pixels[0] < 50U && reopened.pixels[1] > 200U &&
              reopened.pixels[2] < 50U);
  TEST_ASSERT_SIZE(red_renderer.renders, 2U);
  TEST_ASSERT_SIZE(baca_graphics_cache_stats(context).entries, 3U);
  TEST_ASSERT(reopened.image_id != red.image_id);

  baca_graphics_surface_release(&reopened);
  baca_graphics_surface_release(&blue);
  baca_graphics_surface_release(&red);
  baca_graphics_free(context);
  baca_document_close(&blue_document);
  baca_document_close(&red_document);
  return BACA_TEST_PASS;
}

static BacaTestResult test_cache_resize_lru_memory_and_pairs(void) {
  BacaDocument document = {0};
  BacaError error = {0};
  TEST_ASSERT(append_cache_svg(&document, "red", &error));
  TEST_ASSERT(append_cache_svg(&document, "green", &error));
  TEST_ASSERT(append_cache_svg(&document, "blue", &error));
  BacaGraphicsContext *context = baca_graphics_create(
      100U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0x000000U, &error);
  TEST_ASSERT(context != NULL);
  BacaGraphicsSurface first = {0};
  TEST_ASSERT_MSG(
      baca_graphics_prepare(context, &document, 0U, 4, 2, &first, &error), "%s",
      error.message);
  const uint32_t first_id = first.image_id;
  BacaGraphicsCacheStats stats = baca_graphics_cache_stats(context);
  TEST_ASSERT_SIZE(stats.entries, 1U);
  TEST_ASSERT(stats.bytes <= stats.maximum_bytes);

  const BacaGraphicsPlacement placement = {
      .row = 0,
      .column = 0,
      .viewport_rows = 2,
      .viewport_columns = 4,
  };
  Capture capture = {0};
  TEST_ASSERT(baca_graphics_kitty_draw(context, &first, &placement,
                                       capture_write, &capture, &error));
  TEST_ASSERT(strstr(capture.output.data, "f=100") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "a=p") != NULL);
  baca_string_free(&capture.output);
  baca_graphics_surface_release(&first);

  BacaGraphicsSurface second = {0};
  TEST_ASSERT_MSG(
      baca_graphics_prepare(context, &document, 1U, 4, 2, &second, &error),
      "%s", error.message);
  const uint32_t second_id = second.image_id;
  baca_graphics_surface_release(&second);
  TEST_ASSERT(baca_graphics_prepare(context, &document, 0U, 4, 2, &first,
                                    &error));
  TEST_ASSERT_INT((int)first.image_id, (int)first_id);
  baca_graphics_surface_release(&first);

  BacaGraphicsSurface third = {0};
  TEST_ASSERT(baca_graphics_prepare(context, &document, 2U, 4, 2, &third,
                                    &error));
  baca_graphics_surface_release(&third);
  stats = baca_graphics_cache_stats(context);
  TEST_ASSERT_SIZE(stats.entries, 2U);
  TEST_ASSERT(stats.bytes <= 100U);
  TEST_ASSERT(baca_graphics_prepare(context, &document, 0U, 4, 2, &first,
                                    &error));
  TEST_ASSERT_INT((int)first.image_id, (int)first_id);
  baca_graphics_surface_release(&first);
  TEST_ASSERT(baca_graphics_prepare(context, &document, 1U, 4, 2, &second,
                                    &error));
  TEST_ASSERT(second.image_id != second_id);
  baca_graphics_surface_release(&second);

  BacaGraphicsSurface stale = {0};
  TEST_ASSERT(baca_graphics_prepare(context, &document, 0U, 4, 2, &stale,
                                    &error));

  const uint64_t before_resize = stats.generation;
  TEST_ASSERT(baca_graphics_resize(context, 80, 24, &error));
  TEST_ASSERT(strstr(capture.output.data, "d=i,i=") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "d=I,i=") != NULL);
  baca_string_free(&capture.output);
  stats = baca_graphics_cache_stats(context);
  TEST_ASSERT_SIZE(stats.entries, 0U);
  TEST_ASSERT(stats.generation > before_resize);
  CellCapture stale_cells = {0};
  TEST_ASSERT(baca_graphics_render_cells(&stale, &placement, capture_cell,
                                         &stale_cells, &error));
  TEST_ASSERT(!baca_graphics_kitty_draw(context, &stale, &placement,
                                        capture_write, &capture, &error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
  baca_graphics_surface_release(&stale);
  const uint64_t after_resize = stats.generation;
  TEST_ASSERT(baca_graphics_resize(context, 80, 24, &error));
  TEST_ASSERT_SIZE(baca_graphics_cache_stats(context).generation, after_resize);

  TEST_ASSERT(
      baca_graphics_prepare(context, &document, 0U, 4, 2, &first, &error));
  TEST_ASSERT(first.image_id != first_id);
  TEST_ASSERT(baca_graphics_kitty_draw(context, &first, &placement,
                                       capture_write, &capture, &error));
  TEST_ASSERT(strstr(capture.output.data, "f=100") != NULL);
  baca_string_free(&capture.output);
  baca_graphics_surface_release(&first);
  TEST_ASSERT(baca_graphics_set_background(context, 0xffffffU, &error));
  TEST_ASSERT(strstr(capture.output.data, "d=i,i=") != NULL);
  TEST_ASSERT(strstr(capture.output.data, "d=I,i=") != NULL);
  baca_string_free(&capture.output);
  TEST_ASSERT_SIZE(baca_graphics_cache_stats(context).entries, 0U);
  TEST_ASSERT(baca_graphics_prepare(context, &document, 0U, 4, 2, &first,
                                    &error));
  TEST_ASSERT(baca_graphics_kitty_draw(context, &first, &placement,
                                       capture_write, &capture, &error));
  TEST_ASSERT(strstr(capture.output.data, "f=100") != NULL);
  baca_graphics_surface_release(&first);
  TEST_ASSERT(baca_graphics_kitty_delete_all(context, capture_write, &capture,
                                             &error));
  baca_string_free(&capture.output);

  bool created = false;
  TEST_ASSERT_INT(baca_graphics_pair(context, 1U, 2U, 10, 2, &created), 10);
  TEST_ASSERT(created);
  TEST_ASSERT_INT(baca_graphics_pair(context, 3U, 4U, 10, 2, &created), 11);
  TEST_ASSERT(created);
  const short nearest = baca_graphics_pair(context, 5U, 6U, 10, 2, &created);
  TEST_ASSERT(!created && (nearest == 10 || nearest == 11));
  TEST_ASSERT_SIZE(baca_graphics_pair_count(context), 2U);

  baca_graphics_free(context);
  baca_document_close(&document);
  return BACA_TEST_PASS;
}

static BacaTestResult test_bounded_decode_and_hostile_geometry(void) {
  static const char large_svg[] =
      "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' "
      "width='10000' height='1000'><rect width='10000' height='1000' "
      "fill='red'/></svg>";
  BacaDocument document = {0};
  BacaError error = {0};
  TEST_ASSERT(append_image_uri(&document, large_svg, "large", &error));
  document.blocks[0].value.image.intrinsic_width = 10000;
  document.blocks[0].value.image.intrinsic_height = 1000;
  BacaGraphicsContext *context = baca_graphics_create(
      8U * 1024U * 1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
  TEST_ASSERT(context != NULL);
  BacaGraphicsSurface bounded = {0};
  TEST_ASSERT_MSG(baca_graphics_prepare(context, &document, 0U, INT_MAX,
                                        INT_MAX, &bounded, &error),
                  "%s", error.message);
  TEST_ASSERT_INT(bounded.width, BACA_GRAPHICS_MAX_COLUMNS);
  TEST_ASSERT_INT(bounded.height, BACA_GRAPHICS_MAX_ROWS * 2);
  TEST_ASSERT(bounded.pixel_bytes <= 8U * 1024U * 1024U);
  baca_graphics_surface_release(&bounded);
  baca_graphics_free(context);
  baca_document_close(&document);

  static const unsigned char pixels[] = {1U, 2U, 3U, 4U, 5U, 6U};
  const BacaGraphicsSurface surface = {
      .pixels = pixels,
      .pixel_bytes = sizeof(pixels),
      .width = 1,
      .height = 2,
      .rowstride = 3,
      .image_id = 1U,
  };
  BacaGraphicsPlacement placement = {
      .row = INT_MIN,
      .column = INT_MIN,
      .viewport_rows = INT_MAX,
      .viewport_columns = INT_MAX,
  };
  CellCapture cells = {0};
  TEST_ASSERT(baca_graphics_render_cells(&surface, &placement, capture_cell,
                                         &cells, &error));
  TEST_ASSERT_INT(cells.count, 0);
  placement.row = INT_MAX;
  placement.column = INT_MAX;
  TEST_ASSERT(baca_graphics_render_cells(&surface, &placement, capture_cell,
                                         &cells, &error));
  TEST_ASSERT_INT(cells.count, 0);

  const BacaGraphicsRect overflow_cover = {
      .row = INT_MAX,
      .column = INT_MAX,
      .rows = INT_MAX,
      .columns = INT_MAX,
  };
  placement = (BacaGraphicsPlacement){
      .viewport_rows = 1,
      .viewport_columns = 1,
      .occlusions = &overflow_cover,
      .occlusion_count = 1U,
  };
  TEST_ASSERT(baca_graphics_render_cells(&surface, &placement, capture_cell,
                                         &cells, &error));
  TEST_ASSERT_INT(cells.count, 1);
  placement.occlusion_count = BACA_GRAPHICS_MAX_OCCLUSIONS + 1U;
  TEST_ASSERT(!baca_graphics_render_cells(&surface, &placement, capture_cell,
                                          &cells, &error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);

  BacaGraphicsSurface short_surface = surface;
  short_surface.pixel_bytes = sizeof(pixels) - 1U;
  placement.occlusion_count = 0U;
  TEST_ASSERT(!baca_graphics_render_cells(&short_surface, &placement,
                                          capture_cell, &cells, &error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
  return BACA_TEST_PASS;
}

static BacaTestResult test_palette_bounds_and_local_error_isolation(void) {
  static const unsigned color_counts[] = {8U, 16U, 88U, 256U};
  static const uint32_t colors[] = {0x000000U, 0x7f1234U, 0xffffffU};
  for (size_t count_index = 0U; count_index < BACA_ARRAY_LEN(color_counts);
       ++count_index) {
    for (size_t color_index = 0U; color_index < BACA_ARRAY_LEN(colors);
         ++color_index) {
      TEST_ASSERT(baca_graphics_rgb_to_palette(colors[color_index],
                                               color_counts[count_index]) <
                  color_counts[count_index]);
    }
  }
  TEST_ASSERT_INT((int)baca_graphics_rgb_to_palette(0x8b00cdU, 88U), 34);
  TEST_ASSERT_INT((int)baca_graphics_rgb_to_palette(0xcd00ffU, 88U), 51);
  TEST_ASSERT_INT((int)baca_graphics_rgb_to_palette(0x2e2e2eU, 88U), 80);
  TEST_ASSERT_INT((int)baca_graphics_rgb_to_palette(0xa2a2a2U, 88U), 84);
  TEST_ASSERT_INT((int)baca_graphics_rgb_to_palette(0x8b0000U, 87U), 1);

  BacaDocument document = {0};
  BacaError error = {0};
  TEST_ASSERT(append_image_uri(
      &document, "data:image/png;base64,bm90LWFuLWltYWdl", "bad", &error));
  TEST_ASSERT(append_cache_svg(&document, "red", &error));
  TEST_ASSERT(append_cache_svg(&document, "blue", &error));
  BacaGraphicsContext *context = baca_graphics_create(
      1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
  TEST_ASSERT(context != NULL);
  BacaGraphicsSurface bad = {0};
  TEST_ASSERT(!baca_graphics_prepare(context, &document, 0U, 2, 1, &bad,
                                     &error));
  TEST_ASSERT_SIZE(baca_graphics_cache_stats(context).entries, 0U);

  BacaGraphicsSurface first = {0};
  BacaGraphicsSurface second = {0};
  TEST_ASSERT(baca_graphics_prepare(context, &document, 1U, 2, 1, &first,
                                    &error));
  TEST_ASSERT(baca_graphics_prepare(context, &document, 2U, 2, 1, &second,
                                    &error));
  const BacaGraphicsPlacement placement = {
      .viewport_rows = 1,
      .viewport_columns = 2,
  };
  Capture failed = {.fail_writes = true};
  TEST_ASSERT(!baca_graphics_kitty_draw(context, &first, &placement,
                                        capture_write, &failed, &error));
  TEST_ASSERT_ERROR(error, BACA_ERROR_IO);

  Capture capture = {0};
  TEST_ASSERT(baca_graphics_kitty_draw(context, &second, &placement,
                                       capture_write, &capture, &error));
  char image_marker[64] = {0};
  const int marker_length = snprintf(image_marker, sizeof(image_marker),
                                     "a=t,f=100,i=%u", second.image_id);
  TEST_ASSERT(marker_length > 0 && (size_t)marker_length < sizeof(image_marker));
  TEST_ASSERT(strstr(capture.output.data, image_marker) != NULL);
  TEST_ASSERT(baca_graphics_kitty_delete_all(context, capture_write, &capture,
                                             &error));
  TEST_ASSERT(count_bytes(capture.output.data, "d=I,i=") >= 2U);
  baca_string_free(&capture.output);
  baca_graphics_surface_release(&second);
  baca_graphics_surface_release(&first);
  baca_graphics_free(context);
  baca_document_close(&document);
  return BACA_TEST_PASS;
}

static BacaTestResult test_pty_ansi_and_kitty_capture(void) {
  int master = -1;
  int slave = -1;
  TEST_ASSERT(openpty(&master, &slave, NULL, NULL, NULL) == 0);
  struct termios terminal = {0};
  TEST_ASSERT(tcgetattr(slave, &terminal) == 0);
  cfmakeraw(&terminal);
  TEST_ASSERT(tcsetattr(slave, TCSANOW, &terminal) == 0);
  TEST_ASSERT(fcntl(master, F_SETFL, O_NONBLOCK) == 0);

  static const unsigned char pixels[] = {255U, 0U, 0U, 0U, 0U, 255U};
  const BacaGraphicsSurface surface = {
      .pixels = pixels,
      .pixel_bytes = sizeof(pixels),
      .width = 1,
      .height = 2,
      .rowstride = 3,
      .image_id = 42U,
  };
  const BacaGraphicsPlacement placement = {
      .viewport_rows = 1,
      .viewport_columns = 1,
  };
  FdWriter writer = {.fd = slave};
  BacaError error = {0};
  TEST_ASSERT(baca_graphics_render_ansi(&surface, &placement, fd_write_all,
                                        &writer, &error));
  BacaGraphicsContext *context = baca_graphics_create(
      1024U, BACA_GRAPHICS_MULTIPLEXER_NONE, 0U, &error);
  TEST_ASSERT(context != NULL);
  TEST_ASSERT(baca_graphics_kitty_transmit(
      context, surface.image_id, graphics_pixel_png, sizeof(graphics_pixel_png),
      fd_write_all, &writer, &error));
  TEST_ASSERT(baca_graphics_kitty_place(context, &surface, &placement,
                                        fd_write_all, &writer, &error));
  TEST_ASSERT(baca_graphics_kitty_delete_all(context, fd_write_all, &writer,
                                             &error));
  TEST_ASSERT(tcdrain(slave) == 0);

  BacaString output = {0};
  TEST_ASSERT(read_pty_capture(master, &output));
  TEST_ASSERT(strstr(output.data, "\033[38;2;255;0;0m") != NULL);
  TEST_ASSERT(strstr(output.data, "a=t,f=100,i=42") != NULL);
  TEST_ASSERT(strstr(output.data, "a=p,i=42") != NULL);
  TEST_ASSERT(strstr(output.data, "d=i,i=42") != NULL);
  TEST_ASSERT(strstr(output.data, "d=I,i=42") != NULL);
  TEST_ASSERT(strstr(output.data, "d=a") == NULL &&
              strstr(output.data, "d=A") == NULL);
  baca_string_free(&output);
  baca_graphics_free(context);
  TEST_ASSERT(close(slave) == 0);
  TEST_ASSERT(close(master) == 0);
  return BACA_TEST_PASS;
}

static BacaTestResult test_tui_pty_fallback_and_protocol_capture(void) {
  BacaString kitty = {0};
  TEST_ASSERT_MSG(
      run_tui_pty_capture(BACA_IMAGE_MODE_KITTY, 40U, "a=t,f=100", &kitty),
      "Kitty PTY capture failed after %zu bytes: %s", kitty.length,
      kitty.data == NULL ? "(empty)" : kitty.data);
  TEST_ASSERT(strstr(kitty.data, "IMAGE") != NULL);
  TEST_ASSERT(strstr(kitty.data, "a=t,f=100") != NULL);
  TEST_ASSERT(strstr(kitty.data, "a=p,") != NULL);
  TEST_ASSERT(strstr(kitty.data, "d=I,i=") != NULL);
  baca_string_free(&kitty);

  BacaString ansi = {0};
  TEST_ASSERT(run_tui_pty_capture(BACA_IMAGE_MODE_ANSI, 40U, "\033[38;2;", &ansi));
  TEST_ASSERT(strstr(ansi.data, "\033[38;2;") != NULL);
  TEST_ASSERT(strstr(ansi.data, "\xe2\x96\x80") != NULL);
  TEST_ASSERT(strstr(ansi.data, "\033_G") == NULL);
  baca_string_free(&ansi);

  BacaString wide = {0};
  TEST_ASSERT_MSG(
      run_tui_pty_capture(BACA_IMAGE_MODE_KITTY, 1200U, "a=p,", &wide),
      "wide Kitty PTY capture failed after %zu bytes: %s", wide.length,
      wide.data == NULL ? "(empty)" : wide.data);
  TEST_ASSERT(strstr(wide.data, "\0337\033[1;89H") != NULL);
  TEST_ASSERT(strstr(wide.data,
                     "q=2,x=0,y=0,w=1024,h=24,c=1024,r=12,C=1") != NULL);
  baca_string_free(&wide);
  return BACA_TEST_PASS;
}

static BacaTestResult test_tui_tall_standalone_placeholder_and_stable_click(void) {
  TEST_ASSERT_SIZE(sizeof(graphics_tall_svg), sizeof(graphics_wide_svg));
  TEST_ASSERT(baca_test_write("tui-tall/image @.svg", graphics_tall_svg,
                              sizeof(graphics_tall_svg) - 1U));
  TEST_ASSERT(baca_test_write_text(
      "tui-tall/config/mereader-tui/config.ini",
      "[General]\nImageMode=kitty\nPreferredImageViewer=capture-viewer\n"
      "MaxTextWidth=80\nPageScrollDuration=0\n"
      "[Keymaps]\nScreenshot=s\n"));
  TEST_ASSERT(baca_test_mkdir("tui-tall/screenshots"));
  char *path = baca_test_path("tui-tall/image @.svg");
  char *held_path = baca_test_path("tui-tall/image @.svg.held");
  char *config_root = baca_test_path("tui-tall/config");
  char *screenshot_directory = baca_test_path("tui-tall/screenshots");
  TEST_ASSERT(path != NULL && held_path != NULL && config_root != NULL &&
              screenshot_directory != NULL);
  (void)unlink(held_path);

  int opener_pipe[2] = {-1, -1};
  TEST_ASSERT(pipe(opener_pipe) == 0);
  const struct winsize size = {
      .ws_row = 12U,
      .ws_col = 40U,
      .ws_xpixel = 400U,
      .ws_ypixel = 192U,
  };
  int master = -1;
  (void)fflush(NULL);
  const pid_t child = forkpty(&master, NULL, NULL, &size);
  TEST_ASSERT(child >= 0);
  if (child == 0) {
    (void)close(opener_pipe[0]);
    (void)setenv("TERM", "xterm-256color", 1);
    (void)setenv("COLORTERM", "truecolor", 1);
    (void)setenv("TERM_PROGRAM", "kitty", 1);
    (void)setenv("KITTY_WINDOW_ID", "1", 1);
    (void)unsetenv("TMUX");
    (void)unsetenv("STY");
    (void)setenv("XDG_CONFIG_HOME", config_root, 1);
    char descriptor[32] = {0};
    (void)snprintf(descriptor, sizeof(descriptor), "%d", opener_pipe[1]);
    (void)setenv("BACA_IMAGE_PTY_CHILD", "1", 1);
    (void)setenv("BACA_IMAGE_PTY_PATH", path, 1);
    (void)setenv("BACA_IMAGE_PTY_HELD_PATH", held_path, 1);
    (void)setenv("BACA_IMAGE_PTY_SCREENSHOT_DIR", screenshot_directory, 1);
    (void)setenv("BACA_IMAGE_PTY_OPEN_FD", descriptor, 1);
    (void)execl("./build/tests/test_mereader_tui", "test_mereader_tui", (char *)NULL);
    _exit(127);
  }

  (void)close(opener_pipe[1]);
  BacaString output = {0};
  BacaString opened = {0};
  unsigned stage = 0U;
  bool completed = false;
  int status = 0;
  bool ok = fcntl(master, F_SETFL, O_NONBLOCK) == 0 &&
            fcntl(opener_pipe[0], F_SETFL, O_NONBLOCK) == 0 &&
            graphics_wait_for(master, &output, 0U, "IMAGE") &&
            graphics_drain_until_idle(master, &output) &&
            output.data != NULL && strstr(output.data, "a=t,f=100") == NULL;
  if (ok) {
    stage = 1U;
  }

  FdWriter master_writer = {.fd = master};
  size_t start = output.length;
  ok = ok && fd_write_all(&master_writer, "s", 1U) &&
       graphics_wait_for(master, &output, start, "Saved screenshot:") &&
       graphics_screenshot_contains(screenshot_directory, "IMAGE");
  if (ok) {
    stage = 2U;
  }

  ok = ok && fd_write_all(&master_writer, "q", 1U) &&
       graphics_drain_until_idle(master, &output) &&
       graphics_send_mouse_click(master, 2, 0) &&
       graphics_drain_until_idle(master, &output) &&
       graphics_drain_until_idle(opener_pipe[0], &opened) &&
       opened.length == 0U;
  if (ok) {
    stage = 3U;
  }

  ok = ok && graphics_send_mouse_click(master, 19, 0) &&
       graphics_wait_for(opener_pipe[0], &opened, 0U, "capture-viewer\n");
  if (ok) {
    stage = 4U;
  }

  char *target = NULL;
  char *viewer = NULL;
  if (ok && opened.data != NULL) {
    char *target_end = strchr(opened.data, '\n');
    char *viewer_end = target_end == NULL ? NULL : strchr(target_end + 1, '\n');
    if (target_end != NULL && viewer_end != NULL) {
      *target_end = '\0';
      *viewer_end = '\0';
      target = opened.data;
      viewer = target_end + 1;
    }
  }
  const char *temporary_root = getenv("TMPDIR");
  char export_prefix[PATH_MAX] = {0};
  const int export_prefix_length = temporary_root == NULL
                                       ? -1
                                       : snprintf(export_prefix,
                                                  sizeof(export_prefix),
                                                   "%s/mereader-tui-image-",
                                                  temporary_root);
  const char *target_basename = target == NULL ? NULL : strrchr(target, '/');
  target_basename = target_basename == NULL ? target : target_basename + 1;
  struct stat target_status;
  struct stat held_status;
  struct stat replacement_status;
  BacaError error = {0};
  BacaBuffer target_bytes = {0};
  BacaBuffer replacement_bytes = {0};
  ok = ok && target != NULL && viewer != NULL &&
       strcmp(viewer, "capture-viewer") == 0 && export_prefix_length > 0 &&
       (size_t)export_prefix_length < sizeof(export_prefix) &&
       strncmp(target, export_prefix, (size_t)export_prefix_length) == 0 &&
       target_basename != NULL && strcmp(target_basename, "image__.svg") == 0 &&
       strcmp(target, path) != 0 && stat(target, &target_status) == 0 &&
       S_ISREG(target_status.st_mode) &&
       (target_status.st_mode & (S_IRWXG | S_IRWXO)) == 0 &&
       stat(held_path, &held_status) == 0 &&
       stat(path, &replacement_status) == 0 &&
       (target_status.st_dev != held_status.st_dev ||
        target_status.st_ino != held_status.st_ino) &&
       (target_status.st_dev != replacement_status.st_dev ||
        target_status.st_ino != replacement_status.st_ino) &&
       baca_read_file(target, &target_bytes, &error) &&
       baca_read_file(path, &replacement_bytes, &error) &&
       target_bytes.length == sizeof(graphics_tall_svg) - 1U &&
       replacement_bytes.length == sizeof(graphics_wide_svg) - 1U &&
       memcmp(target_bytes.data, graphics_tall_svg,
              sizeof(graphics_tall_svg) - 1U) == 0 &&
       memcmp(replacement_bytes.data, graphics_wide_svg,
              sizeof(graphics_wide_svg) - 1U) == 0;
  if (ok) {
    stage = 5U;
  }
  baca_buffer_free(&target_bytes);
  baca_buffer_free(&replacement_bytes);

  ok = ok && output.data != NULL && strstr(output.data, "a=t,f=100") == NULL &&
       fd_write_all(&master_writer, "qq", 2U);
  for (unsigned attempt = 0U; attempt < 300U && ok; ++attempt) {
    struct pollfd poll_descriptor = {.fd = master, .events = POLLIN};
    const int ready = poll(&poll_descriptor, 1U, 20);
    if (ready < 0 && errno != EINTR) {
      ok = false;
      break;
    }
    if (!drain_pty(master, &output)) {
      ok = false;
      break;
    }
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      completed = true;
      break;
    }
    if (waited < 0) {
      ok = false;
      break;
    }
    if ((poll_descriptor.revents & POLLHUP) != 0) {
      (void)poll(NULL, 0U, 20);
    }
  }
  if (!completed) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
  }
  if (ok && completed && target != NULL) {
    errno = 0;
    ok = stat(target, &target_status) != 0 && errno == ENOENT;
    if (ok) {
      stage = 6U;
    }
  }
  (void)drain_pty(master, &output);
  (void)drain_pty(opener_pipe[0], &opened);
  (void)close(master);
  (void)close(opener_pipe[0]);
  baca_string_free(&opened);
  const size_t output_length = output.length;
  char output_excerpt[512] = {0};
  if (output.data != NULL) {
    (void)snprintf(output_excerpt, sizeof(output_excerpt), "%.480s", output.data);
  }
  baca_string_free(&output);
  free(screenshot_directory);
  free(config_root);
  free(held_path);
  free(path);
  if (!ok || !completed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return baca_test_fail_at(
        __FILE__, __LINE__,
        "tall standalone PTY failed at stage %u after %zu bytes (status=%d): %s",
        stage, output_length, status, output_excerpt);
  }
  return BACA_TEST_PASS;
}

const BacaTestCase *baca_graphics_test_cases(size_t *count) {
  static const BacaTestCase cases[] = {
      {.name = "mode_selection", .function = test_mode_selection},
      {.name = "dimension_probing_and_malformed_resources",
       .function = test_dimension_probing_and_malformed_resources},
      {.name = "alpha_compositing", .function = test_alpha_compositing},
      {.name = "ansi_output_and_clipping",
       .function = test_ansi_output_and_clipping},
      {.name = "kitty_chunking_place_and_delete",
       .function = test_kitty_chunking_place_and_delete},
      {.name = "cache_resize_lru_memory_and_pairs",
       .function = test_cache_resize_lru_memory_and_pairs},
      {.name = "repeated_uri_dedupe_and_context_ids",
       .function = test_repeated_uri_dedupe_and_context_ids},
      {.name = "cache_is_scoped_to_document_address_and_identity",
       .function = test_cache_is_scoped_to_document_address_and_identity},
      {.name = "bounded_decode_and_hostile_geometry",
       .function = test_bounded_decode_and_hostile_geometry},
      {.name = "palette_bounds_and_local_error_isolation",
       .function = test_palette_bounds_and_local_error_isolation},
      {.name = "pty_ansi_and_kitty_capture",
       .function = test_pty_ansi_and_kitty_capture},
      {.name = "tui_pty_fallback_and_protocol_capture",
       .function = test_tui_pty_fallback_and_protocol_capture},
      {.name = "tui_tall_standalone_placeholder_and_stable_click",
       .function = test_tui_tall_standalone_placeholder_and_stable_click},
  };
  *count = BACA_ARRAY_LEN(cases);
  return cases;
}
