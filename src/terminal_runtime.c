#include "terminal_runtime.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const int BACA_EXIT_SIGNALS[] = {SIGINT, SIGTERM, SIGHUP};
static volatile sig_atomic_t baca_exit_signal = 0;

typedef struct BacaTerminalRuntime {
  sigset_t previous_signal_mask;
  struct sigaction previous_handlers[BACA_ARRAY_LEN(BACA_EXIT_SIGNALS)];
  size_t installed_handlers;
  bool started;
  bool signals_blocked;
} BacaTerminalRuntime;

static BacaTerminalRuntime baca_terminal_runtime;

static void request_exit(int signal_number) {
  if (baca_exit_signal == 0) {
    baca_exit_signal = signal_number;
  }
}

static bool block_exit_signals(sigset_t *previous, BacaError *error) {
  sigset_t signals;
  if (sigemptyset(&signals) != 0) {
    baca_error_set(error, BACA_ERROR_EXTERNAL,
                   "cannot create exit signal mask: %s", strerror(errno));
    return false;
  }
  for (size_t index = 0U; index < BACA_ARRAY_LEN(BACA_EXIT_SIGNALS); ++index) {
    if (sigaddset(&signals, BACA_EXIT_SIGNALS[index]) != 0) {
      baca_error_set(error, BACA_ERROR_EXTERNAL,
                     "cannot create exit signal mask: %s", strerror(errno));
      return false;
    }
  }
  const int status = pthread_sigmask(SIG_BLOCK, &signals, previous);
  if (status != 0) {
    baca_error_set(error, BACA_ERROR_EXTERNAL, "cannot block exit signals: %s",
                   strerror(status));
    return false;
  }
  return true;
}

static bool restore_signal_mask(const sigset_t *previous, BacaError *error) {
  const int status = pthread_sigmask(SIG_SETMASK, previous, NULL);
  if (status != 0) {
    baca_error_set(error, BACA_ERROR_EXTERNAL, "cannot restore signal mask: %s",
                   strerror(status));
    return false;
  }
  return true;
}

static bool capture_signal_handlers(BacaError *error) {
  for (size_t index = 0U; index < BACA_ARRAY_LEN(BACA_EXIT_SIGNALS); ++index) {
    if (sigaction(BACA_EXIT_SIGNALS[index], NULL,
                  &baca_terminal_runtime.previous_handlers[index]) != 0) {
      const int saved_errno = errno;
      baca_error_set(error, BACA_ERROR_EXTERNAL,
                     "cannot inspect signal handler: %s",
                     strerror(saved_errno));
      return false;
    }
  }
  return true;
}

static void restore_signal_handlers(void) {
  while (baca_terminal_runtime.installed_handlers > 0U) {
    --baca_terminal_runtime.installed_handlers;
    (void)sigaction(
        BACA_EXIT_SIGNALS[baca_terminal_runtime.installed_handlers],
        &baca_terminal_runtime
             .previous_handlers[baca_terminal_runtime.installed_handlers],
        NULL);
  }
}

static bool install_signal_handlers(BacaError *error) {
  struct sigaction action = {0};
  action.sa_handler = request_exit;
  (void)sigemptyset(&action.sa_mask);
  for (size_t index = 0U; index < BACA_ARRAY_LEN(BACA_EXIT_SIGNALS); ++index) {
    if (sigaction(BACA_EXIT_SIGNALS[index], &action, NULL) != 0) {
      const int saved_errno = errno;
      restore_signal_handlers();
      baca_error_set(error, BACA_ERROR_EXTERNAL,
                     "cannot install signal handler: %s",
                     strerror(saved_errno));
      return false;
    }
    baca_terminal_runtime.installed_handlers = index + 1U;
  }
  return true;
}

bool baca_terminal_runtime_begin(BacaError *error) {
  if (baca_terminal_runtime.started) {
    baca_error_set(error, BACA_ERROR_INTERNAL,
                   "terminal runtime is already active");
    return false;
  }
  if (!block_exit_signals(&baca_terminal_runtime.previous_signal_mask, error)) {
    return false;
  }
  baca_terminal_runtime.signals_blocked = true;
  baca_exit_signal = 0;
  if (!capture_signal_handlers(error)) {
    (void)restore_signal_mask(&baca_terminal_runtime.previous_signal_mask,
                              NULL);
    baca_terminal_runtime = (BacaTerminalRuntime){0};
    return false;
  }
  baca_terminal_runtime.started = true;
  return true;
}

bool baca_terminal_runtime_activate(BacaError *error) {
  if (!baca_terminal_runtime.started) {
    baca_error_set(error, BACA_ERROR_INTERNAL,
                   "terminal runtime is not initialized");
    return false;
  }
  if (!install_signal_handlers(error)) {
    return false;
  }
  if (!restore_signal_mask(&baca_terminal_runtime.previous_signal_mask,
                           error)) {
    return false;
  }
  baca_terminal_runtime.signals_blocked = false;
  return true;
}

bool baca_terminal_runtime_interrupted(void) { return baca_exit_signal != 0; }

int baca_terminal_runtime_finish(int result, BacaError *error) {
  if (!baca_terminal_runtime.started) {
    return result;
  }
  if (!baca_terminal_runtime.signals_blocked) {
    BacaError mask_error = {0};
    if (block_exit_signals(&baca_terminal_runtime.previous_signal_mask,
                           &mask_error)) {
      baca_terminal_runtime.signals_blocked = true;
    } else if (result == EXIT_SUCCESS) {
      if (error != NULL) {
        *error = mask_error;
      }
      result = EXIT_FAILURE;
    }
  }
  const int signal_number = (int)baca_exit_signal;
  restore_signal_handlers();
  if (baca_terminal_runtime.signals_blocked) {
    BacaError mask_error = {0};
    if (!restore_signal_mask(&baca_terminal_runtime.previous_signal_mask,
                             &mask_error) &&
        result == EXIT_SUCCESS) {
      if (error != NULL) {
        *error = mask_error;
      }
      result = EXIT_FAILURE;
    }
  }
  baca_terminal_runtime = (BacaTerminalRuntime){0};
  baca_exit_signal = 0;
  if (result == EXIT_SUCCESS && signal_number != 0) {
    result = 128 + signal_number;
  }
  return result;
}

double baca_terminal_monotonic_seconds(void) {
  struct timespec now = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0.0;
  }
  return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

bool baca_terminal_graphics_write(void *user_data, const void *data,
                                  size_t length) {
  (void)user_data;
  const unsigned char *cursor = data;
  size_t remaining = length;
  while (remaining > 0U) {
    const ssize_t written = write(STDOUT_FILENO, cursor, remaining);
    if (written > 0) {
      cursor += (size_t)written;
      remaining -= (size_t)written;
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

void baca_terminal_probe_cell_pixels(bool pixel_mode, int *width, int *height) {
  *width = pixel_mode ? 8 : 1;
  *height = pixel_mode ? 16 : 2;
  if (!pixel_mode) {
    return;
  }
  struct winsize size = {0};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0U &&
      size.ws_row > 0U && size.ws_xpixel >= size.ws_col &&
      size.ws_ypixel >= size.ws_row) {
    *width = (int)(size.ws_xpixel / size.ws_col);
    *height = (int)(size.ws_ypixel / size.ws_row);
  }
}
