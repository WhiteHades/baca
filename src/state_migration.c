#define _GNU_SOURCE

#include "baca/state_migration.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACA_STATE_IMPORT_MARKER ".mereader-tui-legacy-state-import-v1.complete"
#define BACA_STATE_IMPORT_LOCK ".mereader-tui-legacy-state-import-v1.lock"
#define BACA_STATE_IMPORT_RETRIES 50
#define BACA_STATE_IMPORT_RETRY_MS 100

static const char baca_state_import_marker_contents[] =
    "mereader-tui legacy state import v1 complete\n";

typedef struct BacaStatePaths {
  char *config_base;
  char *cache_base;
} BacaStatePaths;

typedef struct BacaStateTemp {
  int descriptor;
  char name[96];
} BacaStateTemp;

static void baca_state_paths_free(BacaStatePaths *paths) {
  free(paths->config_base);
  free(paths->cache_base);
  *paths = (BacaStatePaths){0};
}

static char *baca_state_base_from_path(char *path, BacaError *error) {
  if (path == nullptr) {
    return nullptr;
  }
  char *root = baca_path_dirname(path, error);
  free(path);
  if (root == nullptr) {
    return nullptr;
  }
  char *base = baca_path_dirname(root, error);
  free(root);
  return base;
}

static bool baca_state_paths_init(BacaStatePaths *paths, BacaError *error) {
  paths->config_base = baca_state_base_from_path(
      baca_xdg_config_path("config.ini", error), error);
  if (paths->config_base != nullptr) {
    paths->cache_base = baca_state_base_from_path(
        baca_xdg_cache_path(BACA_NAME ".db", error), error);
  }
  if (paths->config_base == nullptr || paths->cache_base == nullptr) {
    baca_state_paths_free(paths);
    return false;
  }
  return true;
}

static bool baca_state_same_object(const struct stat *left,
                                   const struct stat *right) {
  return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static bool baca_state_same_snapshot(const struct stat *left,
                                     const struct stat *right) {
  return baca_state_same_object(left, right) &&
         left->st_size == right->st_size &&
         left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
         left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
         left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
         left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
}

static bool baca_state_close(int *descriptor, const char *description,
                             BacaError *error) {
  if (*descriptor < 0) {
    return true;
  }
  const int current = *descriptor;
  *descriptor = -1;
  if (close(current) == 0) {
    return true;
  }
  const int saved_errno = errno;
  baca_error_set(error, BACA_ERROR_IO, "could not close %s: %s", description,
                 strerror(saved_errno));
  return false;
}

static bool baca_state_sync_directory(int descriptor, const char *description,
                                      BacaError *error) {
  if (fsync(descriptor) == 0) {
    return true;
  }
  const int saved_errno = errno;
  baca_error_set(error, BACA_ERROR_IO, "could not sync %s: %s", description,
                 strerror(saved_errno));
  return false;
}

static int baca_state_open_base(const char *path, BacaError *error) {
  if (!baca_mkdirs(path, error)) {
    return -1;
  }
  const int descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO, "could not open state base '%s': %s",
                   path, strerror(saved_errno));
    return -1;
  }
  struct stat status = {0};
  if (fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode)) {
    const int saved_errno = errno == 0 ? ENOTDIR : errno;
    (void)close(descriptor);
    baca_error_set(error, BACA_ERROR_CORRUPT, "state base '%s' is unsafe: %s",
                   path, strerror(saved_errno));
    return -1;
  }
  return descriptor;
}

static int baca_state_open_directory_at(int parent, const char *name,
                                        bool create, bool *exists,
                                        const char *description,
                                        BacaError *error) {
  *exists = false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    struct stat before = {0};
    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT) {
        const int saved_errno = errno;
        baca_error_set(error, BACA_ERROR_IO, "could not inspect %s: %s",
                       description, strerror(saved_errno));
        return -1;
      }
      if (!create) {
        return -1;
      }
      if (mkdirat(parent, name, 0700) != 0) {
        if (errno == EEXIST) {
          continue;
        }
        const int saved_errno = errno;
        baca_error_set(error, BACA_ERROR_IO, "could not create %s: %s",
                       description, strerror(saved_errno));
        return -1;
      }
      if (!baca_state_sync_directory(parent, "state parent directory", error)) {
        return -1;
      }
      continue;
    }
    if (!S_ISDIR(before.st_mode)) {
      baca_error_set(error, BACA_ERROR_CORRUPT, "%s is not a safe directory",
                     description);
      return -1;
    }

    const int descriptor =
        openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
      if (errno == ENOENT) {
        continue;
      }
      const int saved_errno = errno;
      baca_error_set(error, BACA_ERROR_IO, "could not open %s: %s", description,
                     strerror(saved_errno));
      return -1;
    }
    struct stat after = {0};
    if (fstat(descriptor, &after) != 0 || !S_ISDIR(after.st_mode) ||
        !baca_state_same_object(&before, &after)) {
      const int saved_errno = errno == 0 ? EIO : errno;
      (void)close(descriptor);
      baca_error_set(error, BACA_ERROR_CORRUPT,
                     "%s changed while it was opened: %s", description,
                     strerror(saved_errno));
      return -1;
    }
    *exists = true;
    return descriptor;
  }
  baca_error_set(error, BACA_ERROR_IO, "%s changed while it was being prepared",
                 description);
  return -1;
}

static int baca_state_open_current_directory_at(int parent, const char *name,
                                                bool create, bool *exists,
                                                const char *description,
                                                BacaError *error) {
  int descriptor = baca_state_open_directory_at(parent, name, create, exists,
                                                description, error);
  if (descriptor < 0) {
    return -1;
  }
  if (fchmod(descriptor, 0700) == 0) {
    return descriptor;
  }
  const int saved_errno = errno;
  (void)close(descriptor);
  baca_error_set(error, BACA_ERROR_IO, "could not secure %s: %s", description,
                 strerror(saved_errno));
  return -1;
}

static int baca_state_open_regular_at(int directory, const char *name,
                                      bool *exists, const char *description,
                                      BacaError *error) {
  *exists = false;
  struct stat before = {0};
  if (fstatat(directory, name, &before, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return -1;
    }
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO, "could not inspect %s: %s",
                   description, strerror(saved_errno));
    return -1;
  }
  if (!S_ISREG(before.st_mode)) {
    baca_error_set(error, BACA_ERROR_CORRUPT, "%s is not a safe regular file",
                   description);
    return -1;
  }

  const int descriptor =
      openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return -1;
    }
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO, "could not open %s: %s", description,
                   strerror(saved_errno));
    return -1;
  }
  struct stat after = {0};
  if (fstat(descriptor, &after) != 0 || !S_ISREG(after.st_mode) ||
      !baca_state_same_object(&before, &after)) {
    const int saved_errno = errno == 0 ? EIO : errno;
    (void)close(descriptor);
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "%s changed while it was opened: %s", description,
                   strerror(saved_errno));
    return -1;
  }
  *exists = true;
  return descriptor;
}

static bool baca_state_regular_exists_at(int directory, const char *name,
                                         bool *exists, const char *description,
                                         BacaError *error) {
  int descriptor =
      baca_state_open_regular_at(directory, name, exists, description, error);
  if (descriptor < 0) {
    return !baca_error_is_set(error);
  }
  return baca_state_close(&descriptor, description, error);
}

static bool baca_state_regular_matches_at(int directory, const char *name,
                                          int descriptor,
                                          const char *description,
                                          BacaError *error) {
  struct stat opened = {0};
  struct stat current = {0};
  if (fstat(descriptor, &opened) == 0 && S_ISREG(opened.st_mode) &&
      fstatat(directory, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISREG(current.st_mode) && baca_state_same_object(&opened, &current)) {
    return true;
  }
  const int saved_errno = errno == 0 ? EIO : errno;
  baca_error_set(error, BACA_ERROR_CORRUPT,
                 "%s changed during state import: %s", description,
                 strerror(saved_errno));
  return false;
}

static bool baca_state_entry_exists_at(int directory, const char *name,
                                       bool *exists, const char *description,
                                       BacaError *error) {
  struct stat status = {0};
  if (fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) == 0) {
    *exists = true;
    return true;
  }
  if (errno == ENOENT) {
    *exists = false;
    return true;
  }
  const int saved_errno = errno;
  baca_error_set(error, BACA_ERROR_IO, "could not inspect %s: %s", description,
                 strerror(saved_errno));
  return false;
}

static int baca_state_lock(int cache_base, BacaError *error) {
  const int descriptor =
      openat(cache_base, BACA_STATE_IMPORT_LOCK,
             O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO, "could not open state import lock: %s",
                   strerror(saved_errno));
    return -1;
  }
  struct stat opened = {0};
  if (fstat(descriptor, &opened) != 0 || !S_ISREG(opened.st_mode) ||
      fchmod(descriptor, 0600) != 0) {
    const int saved_errno = errno == 0 ? EINVAL : errno;
    (void)close(descriptor);
    baca_error_set(error, BACA_ERROR_CORRUPT, "state import lock is unsafe: %s",
                   strerror(saved_errno));
    return -1;
  }
  while (flock(descriptor, LOCK_EX) != 0) {
    if (errno == EINTR) {
      continue;
    }
    const int saved_errno = errno;
    (void)close(descriptor);
    baca_error_set(error, BACA_ERROR_IO,
                   "could not acquire state import lock: %s",
                   strerror(saved_errno));
    return -1;
  }

  struct stat current = {0};
  if (fstatat(cache_base, BACA_STATE_IMPORT_LOCK, &current,
              AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(current.st_mode) || !baca_state_same_object(&opened, &current)) {
    const int saved_errno = errno == 0 ? EIO : errno;
    (void)close(descriptor);
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "state import lock changed while it was acquired: %s",
                   strerror(saved_errno));
    return -1;
  }
  return descriptor;
}

static bool baca_state_temp_name(char *name, size_t size, BacaError *error) {
  unsigned char random[16] = {0};
  size_t offset = 0U;
  while (offset < sizeof(random)) {
    const ssize_t count =
        getrandom(random + offset, sizeof(random) - offset, 0U);
    if (count > 0) {
      offset += (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    const int saved_errno = count == 0 ? EIO : errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not generate state import temporary name: %s",
                   strerror(saved_errno));
    return false;
  }

  static const char hex[] = "0123456789abcdef";
  static const char prefix[] = ".legacy-state-import-v1.tmp.";
  if (sizeof(prefix) + sizeof(random) * 2U > size) {
    baca_error_set(error, BACA_ERROR_INTERNAL,
                   "could not build state import temporary name");
    return false;
  }
  memcpy(name, prefix, sizeof(prefix) - 1U);
  size_t cursor = sizeof(prefix) - 1U;
  for (size_t index = 0U; index < sizeof(random); ++index) {
    name[cursor++] = hex[random[index] >> 4U];
    name[cursor++] = hex[random[index] & 0x0fU];
  }
  name[cursor] = '\0';
  return true;
}

static void baca_state_discard_temp(int directory, BacaStateTemp *temporary);

static bool baca_state_create_temp_at(int directory, BacaStateTemp *temporary,
                                      BacaError *error) {
  temporary->descriptor = -1;
  temporary->name[0] = '\0';
  for (unsigned int attempt = 0U; attempt < 128U; ++attempt) {
    if (!baca_state_temp_name(temporary->name, sizeof(temporary->name),
                              error)) {
      return false;
    }
    temporary->descriptor =
        openat(directory, temporary->name,
               O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (temporary->descriptor >= 0) {
      if (fchmod(temporary->descriptor, 0600) == 0) {
        return true;
      }
      const int saved_errno = errno;
      baca_state_discard_temp(directory, temporary);
      baca_error_set(error, BACA_ERROR_IO,
                     "could not secure state import temporary file: %s",
                     strerror(saved_errno));
      return false;
    }
    if (errno != EEXIST) {
      const int saved_errno = errno;
      baca_error_set(error, BACA_ERROR_IO,
                     "could not create state import temporary file: %s",
                     strerror(saved_errno));
      return false;
    }
  }
  baca_error_set(error, BACA_ERROR_IO,
                 "could not allocate a state import temporary file");
  return false;
}

static bool baca_state_temp_entry_matches(int directory,
                                          const BacaStateTemp *temporary) {
  if (temporary->descriptor < 0 || temporary->name[0] == '\0') {
    return false;
  }
  struct stat descriptor_status = {0};
  struct stat path_status = {0};
  return fstat(temporary->descriptor, &descriptor_status) == 0 &&
         S_ISREG(descriptor_status.st_mode) &&
         fstatat(directory, temporary->name, &path_status,
                 AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(path_status.st_mode) &&
         baca_state_same_object(&descriptor_status, &path_status);
}

static void baca_state_discard_temp(int directory, BacaStateTemp *temporary) {
  if (baca_state_temp_entry_matches(directory, temporary) &&
      (unlinkat(directory, temporary->name, 0) == 0 || errno == ENOENT)) {
    temporary->name[0] = '\0';
  }
  if (temporary->descriptor >= 0) {
    (void)close(temporary->descriptor);
    temporary->descriptor = -1;
  }
}

static bool baca_state_write_all(int descriptor, const void *data,
                                 size_t length, const char *description,
                                 BacaError *error) {
  const unsigned char *cursor = data;
  size_t remaining = length;
  while (remaining > 0U) {
    const ssize_t count = write(descriptor, cursor, remaining);
    if (count > 0) {
      cursor += (size_t)count;
      remaining -= (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    const int saved_errno = count == 0 ? EIO : errno;
    baca_error_set(error, BACA_ERROR_IO, "could not write %s: %s", description,
                   strerror(saved_errno));
    return false;
  }
  return true;
}

static bool baca_state_copy_file(int source, int destination,
                                 const char *description, BacaError *error) {
  struct stat before = {0};
  if (fstat(source, &before) != 0 || !S_ISREG(before.st_mode)) {
    const int saved_errno = errno == 0 ? EIO : errno;
    baca_error_set(error, BACA_ERROR_IO, "could not inspect %s: %s",
                   description, strerror(saved_errno));
    return false;
  }
  unsigned char buffer[65536];
  for (;;) {
    const ssize_t count = read(source, buffer, sizeof(buffer));
    if (count == 0) {
      struct stat after = {0};
      if (fstat(source, &after) == 0 &&
          baca_state_same_snapshot(&before, &after)) {
        return true;
      }
      const int saved_errno = errno == 0 ? EIO : errno;
      baca_error_set(error, BACA_ERROR_CORRUPT,
                     "%s changed while it was copied: %s", description,
                     strerror(saved_errno));
      return false;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved_errno = errno;
      baca_error_set(error, BACA_ERROR_IO, "could not read %s: %s", description,
                     strerror(saved_errno));
      return false;
    }
    if (!baca_state_write_all(destination, buffer, (size_t)count, description,
                              error)) {
      return false;
    }
  }
}

static bool baca_state_publish_temp(int directory, BacaStateTemp *temporary,
                                    const char *destination,
                                    bool require_regular_collision,
                                    const char *description, BacaError *error) {
  if (!baca_state_sync_directory(directory,
                                 "state import destination directory", error)) {
    baca_state_discard_temp(directory, temporary);
    return false;
  }
  if (renameat2(directory, temporary->name, directory, destination,
                RENAME_NOREPLACE) == 0) {
    temporary->name[0] = '\0';
    if (!baca_state_close(&temporary->descriptor, description, error)) {
      return false;
    }
    return baca_state_sync_directory(
        directory, "state import destination directory", error);
  }

  const int saved_errno = errno;
  if (saved_errno == EEXIST) {
    if (!baca_state_temp_entry_matches(directory, temporary)) {
      (void)baca_state_close(&temporary->descriptor,
                             "state import temporary file", error);
      if (!baca_error_is_set(error)) {
        baca_error_set(error, BACA_ERROR_CORRUPT,
                       "state import temporary file changed before cleanup");
      }
      return false;
    }
    if (unlinkat(directory, temporary->name, 0) != 0 && errno != ENOENT) {
      const int unlink_errno = errno;
      (void)close(temporary->descriptor);
      temporary->descriptor = -1;
      baca_error_set(error, BACA_ERROR_IO,
                     "could not remove state import temporary file: %s",
                     strerror(unlink_errno));
      return false;
    }
    temporary->name[0] = '\0';
    if (!baca_state_close(&temporary->descriptor, "state import temporary file",
                          error)) {
      return false;
    }
    if (!baca_state_sync_directory(
            directory, "state import destination directory", error)) {
      return false;
    }
    if (!require_regular_collision) {
      return true;
    }
    bool exists = false;
    return baca_state_regular_exists_at(directory, destination, &exists,
                                        description, error) &&
           exists;
  }
  baca_state_discard_temp(directory, temporary);
  baca_error_set(error, BACA_ERROR_IO,
                 "could not publish %s without overwrite: %s", description,
                 strerror(saved_errno));
  return false;
}

static bool baca_state_import_config(int config_base, BacaError *error) {
  bool new_root_exists = false;
  int new_root = baca_state_open_current_directory_at(
      config_base, BACA_NAME, false, &new_root_exists, "current config root",
      error);
  if (new_root < 0 && baca_error_is_set(error)) {
    return false;
  }
  if (new_root_exists) {
    bool current_exists = false;
    if (!baca_state_regular_exists_at(new_root, "config.ini", &current_exists,
                                      "current config", error)) {
      (void)close(new_root);
      return false;
    }
    if (current_exists) {
      return baca_state_close(&new_root, "current config root", error);
    }
  }

  bool old_root_exists = false;
  int old_root =
      baca_state_open_directory_at(config_base, "baca", false, &old_root_exists,
                                   "legacy config root", error);
  if (old_root < 0) {
    if (baca_error_is_set(error)) {
      (void)close(new_root);
      return false;
    }
    return baca_state_close(&new_root, "current config root", error);
  }
  bool source_exists = false;
  int source = baca_state_open_regular_at(
      old_root, "config.ini", &source_exists, "legacy config", error);
  if (source < 0) {
    const bool success = !baca_error_is_set(error);
    (void)close(old_root);
    (void)close(new_root);
    return success;
  }

  if (!new_root_exists) {
    new_root = baca_state_open_current_directory_at(
        config_base, BACA_NAME, true, &new_root_exists, "current config root",
        error);
    if (new_root < 0) {
      (void)close(source);
      (void)close(old_root);
      return false;
    }
  }
  bool current_exists = false;
  if (!baca_state_regular_exists_at(new_root, "config.ini", &current_exists,
                                    "current config", error)) {
    (void)close(source);
    (void)close(old_root);
    (void)close(new_root);
    return false;
  }
  if (current_exists) {
    bool success = baca_state_close(&source, "legacy config", error);
    if (success) {
      success = baca_state_close(&old_root, "legacy config root", error);
    } else {
      (void)close(old_root);
    }
    if (success) {
      success = baca_state_close(&new_root, "current config root", error);
    } else {
      (void)close(new_root);
    }
    return success;
  }

  BacaStateTemp temporary = {.descriptor = -1};
  bool imported = baca_state_create_temp_at(new_root, &temporary, error) &&
                  baca_state_copy_file(source, temporary.descriptor,
                                       "legacy config", error) &&
                  fsync(temporary.descriptor) == 0;
  if (!imported && !baca_error_is_set(error)) {
    const int saved_errno = errno == 0 ? EIO : errno;
    baca_error_set(error, BACA_ERROR_IO, "could not sync imported config: %s",
                   strerror(saved_errno));
  }
  if (imported) {
    imported = baca_state_publish_temp(new_root, &temporary, "config.ini", true,
                                       "current config", error);
  } else {
    baca_state_discard_temp(new_root, &temporary);
  }
  if (source >= 0 && close(source) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO, "could not close legacy config: %s",
                   strerror(saved_errno));
    imported = false;
  }
  if (close(old_root) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close legacy config root: %s",
                   strerror(saved_errno));
    imported = false;
  }
  if (close(new_root) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close current config root: %s",
                   strerror(saved_errno));
    imported = false;
  }
  return imported;
}

static bool baca_state_database_temp_sidecar(int directory, const char *name,
                                             bool *exists, BacaError *error) {
  static const char *const suffixes[] = {"-journal", "-wal", "-shm"};
  *exists = false;
  for (size_t index = 0U; index < BACA_ARRAY_LEN(suffixes); ++index) {
    char sidecar[128] = {0};
    const int length =
        snprintf(sidecar, sizeof(sidecar), "%s%s", name, suffixes[index]);
    if (length <= 0 || (size_t)length >= sizeof(sidecar)) {
      baca_error_set(error, BACA_ERROR_INTERNAL,
                     "could not build database sidecar name");
      return false;
    }
    bool sidecar_exists = false;
    if (!baca_state_entry_exists_at(directory, sidecar, &sidecar_exists,
                                    "database sidecar", error)) {
      return false;
    }
    if (sidecar_exists) {
      *exists = true;
      return true;
    }
  }
  return true;
}

static void baca_state_discard_database_temp(int directory,
                                             BacaStateTemp *temporary,
                                             bool remove_sidecars) {
  if (baca_state_temp_entry_matches(directory, temporary)) {
    if (remove_sidecars) {
      static const char *const suffixes[] = {"-journal", "-wal", "-shm"};
      for (size_t index = 0U; index < BACA_ARRAY_LEN(suffixes); ++index) {
        char name[128] = {0};
        const int length = snprintf(name, sizeof(name), "%s%s", temporary->name,
                                    suffixes[index]);
        if (length > 0 && (size_t)length < sizeof(name)) {
          (void)unlinkat(directory, name, 0);
        }
      }
    }
    (void)unlinkat(directory, temporary->name, 0);
    temporary->name[0] = '\0';
  }
  if (temporary->descriptor >= 0) {
    (void)close(temporary->descriptor);
    temporary->descriptor = -1;
  }
}

static bool baca_state_create_database_temp(int directory,
                                            BacaStateTemp *temporary,
                                            BacaError *error) {
  temporary->descriptor = -1;
  temporary->name[0] = '\0';
  for (unsigned int attempt = 0U; attempt < 128U; ++attempt) {
    if (!baca_state_temp_name(temporary->name, sizeof(temporary->name),
                              error)) {
      return false;
    }
    temporary->descriptor =
        openat(directory, temporary->name,
               O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (temporary->descriptor < 0) {
      if (errno == EEXIST) {
        continue;
      }
      const int saved_errno = errno;
      baca_error_set(error, BACA_ERROR_IO,
                     "could not create database import temporary file: %s",
                     strerror(saved_errno));
      return false;
    }
    bool sidecar_exists = false;
    if (!baca_state_database_temp_sidecar(directory, temporary->name,
                                          &sidecar_exists, error)) {
      baca_state_discard_database_temp(directory, temporary, false);
      return false;
    }
    if (!sidecar_exists && fchmod(temporary->descriptor, 0600) == 0) {
      return true;
    }
    const int saved_errno = errno;
    baca_state_discard_database_temp(directory, temporary, false);
    if (!sidecar_exists) {
      baca_error_set(error, BACA_ERROR_IO,
                     "could not secure database import temporary file: %s",
                     strerror(saved_errno));
      return false;
    }
  }
  baca_error_set(error, BACA_ERROR_IO,
                 "could not allocate a database import temporary file");
  return false;
}

static char *baca_state_database_path(int directory, const char *name,
                                      BacaError *error) {
  char path[160] = {0};
  const int length =
      snprintf(path, sizeof(path), "/proc/self/fd/%d/%s", directory, name);
  if (length <= 0 || (size_t)length >= sizeof(path)) {
    baca_error_set(error, BACA_ERROR_INTERNAL,
                   "could not build anchored database import path");
    return nullptr;
  }
  return baca_strdup(path, error);
}

static void baca_state_sqlite_error(BacaError *error, const char *operation,
                                    sqlite3 *database, int status) {
  baca_error_set(error, BACA_ERROR_DATABASE, "%s: %s", operation,
                 database == nullptr ? sqlite3_errstr(status)
                                     : sqlite3_errmsg(database));
}

static bool baca_state_sqlite_matches_descriptor(sqlite3 *database,
                                                 int descriptor,
                                                 const char *description,
                                                 BacaError *error) {
  const char *path = sqlite3_db_filename(database, "main");
  struct stat expected = {0};
  struct stat current = {0};
  int moved = 1;
  const int status =
      sqlite3_file_control(database, "main", SQLITE_FCNTL_HAS_MOVED, &moved);
  if (path != nullptr && status == SQLITE_OK && moved == 0 &&
      fstat(descriptor, &expected) == 0 && S_ISREG(expected.st_mode) &&
      stat(path, &current) == 0 && S_ISREG(current.st_mode) &&
      baca_state_same_object(&expected, &current)) {
    return true;
  }
  baca_error_set(error, BACA_ERROR_CORRUPT, "%s changed while sqlite opened it",
                 description);
  return false;
}

static bool baca_state_backup_database(const char *source_path,
                                       int source_descriptor,
                                       const char *target_path,
                                       int target_descriptor,
                                       BacaError *error) {
  sqlite3 *source = nullptr;
  sqlite3 *target = nullptr;
  sqlite3_backup *backup = nullptr;
  bool copied = false;

  int status =
      sqlite3_open_v2(source_path, &source,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (status != SQLITE_OK) {
    baca_state_sqlite_error(error, "could not open legacy database read-only",
                            source, status);
    goto cleanup;
  }
  if (!baca_state_sqlite_matches_descriptor(source, source_descriptor,
                                            "legacy database", error)) {
    goto cleanup;
  }
  status = sqlite3_busy_timeout(source, BACA_STATE_IMPORT_RETRIES *
                                            BACA_STATE_IMPORT_RETRY_MS);
  if (status != SQLITE_OK) {
    baca_state_sqlite_error(error,
                            "could not configure legacy database busy timeout",
                            source, status);
    goto cleanup;
  }
  status =
      sqlite3_open_v2(target_path, &target,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (status != SQLITE_OK) {
    baca_state_sqlite_error(
        error, "could not open database import temporary file", target, status);
    goto cleanup;
  }
  if (!baca_state_sqlite_matches_descriptor(
          target, target_descriptor, "database import temporary file", error)) {
    goto cleanup;
  }
  status = sqlite3_busy_timeout(target, BACA_STATE_IMPORT_RETRIES *
                                            BACA_STATE_IMPORT_RETRY_MS);
  if (status != SQLITE_OK) {
    baca_state_sqlite_error(error,
                            "could not configure database import busy timeout",
                            target, status);
    goto cleanup;
  }

  backup = sqlite3_backup_init(target, "main", source, "main");
  if (backup == nullptr) {
    baca_state_sqlite_error(error,
                            "could not initialize legacy database snapshot",
                            target, sqlite3_errcode(target));
    goto cleanup;
  }
  int step_status = SQLITE_OK;
  int retries = 0;
  for (;;) {
    step_status = sqlite3_backup_step(backup, -1);
    if (step_status == SQLITE_DONE) {
      break;
    }
    const int primary_status = step_status & 0xff;
    if ((primary_status == SQLITE_BUSY || primary_status == SQLITE_LOCKED) &&
        retries < BACA_STATE_IMPORT_RETRIES) {
      ++retries;
      (void)sqlite3_sleep(BACA_STATE_IMPORT_RETRY_MS);
      continue;
    }
    if (primary_status == SQLITE_BUSY || primary_status == SQLITE_LOCKED) {
      baca_error_set(error, BACA_ERROR_DATABASE,
                     "legacy database remained busy during state import");
    } else {
      baca_state_sqlite_error(error, "could not snapshot legacy database",
                              target, step_status);
    }
    break;
  }
  status = sqlite3_backup_finish(backup);
  backup = nullptr;
  if (step_status == SQLITE_DONE && status == SQLITE_OK) {
    copied = true;
  } else if (!baca_error_is_set(error)) {
    baca_state_sqlite_error(error, "could not finish legacy database snapshot",
                            target, status);
  }

cleanup:
  if (backup != nullptr) {
    (void)sqlite3_backup_finish(backup);
  }
  if (target != nullptr) {
    status = sqlite3_close(target);
    if (status != SQLITE_OK && copied) {
      baca_state_sqlite_error(error,
                              "could not close database import temporary file",
                              nullptr, status);
      copied = false;
    }
  }
  if (source != nullptr) {
    status = sqlite3_close(source);
    if (status != SQLITE_OK && copied) {
      baca_state_sqlite_error(error, "could not close legacy database", nullptr,
                              status);
      copied = false;
    }
  }
  return copied;
}

static bool baca_state_new_database_sidecars_absent(int new_root,
                                                    BacaError *error) {
  static const char *const sidecars[] = {
      BACA_NAME ".db-journal", BACA_NAME ".db-wal", BACA_NAME ".db-shm"};
  for (size_t index = 0U; index < BACA_ARRAY_LEN(sidecars); ++index) {
    bool exists = false;
    if (!baca_state_entry_exists_at(new_root, sidecars[index], &exists,
                                    "current database sidecar", error)) {
      return false;
    }
    if (exists) {
      baca_error_set(error, BACA_ERROR_CORRUPT,
                     "current database sidecar exists without a current "
                     "database main file");
      return false;
    }
  }
  return true;
}

static bool baca_state_import_database(int cache_base, int new_root,
                                       BacaError *error) {
  bool current_exists = false;
  if (!baca_state_regular_exists_at(new_root, BACA_NAME ".db", &current_exists,
                                    "current database", error)) {
    return false;
  }
  if (current_exists) {
    return true;
  }
  if (!baca_state_new_database_sidecars_absent(new_root, error)) {
    return false;
  }

  bool old_root_exists = false;
  int old_root = baca_state_open_directory_at(
      cache_base, "baca", false, &old_root_exists, "legacy cache root", error);
  if (old_root < 0) {
    return !baca_error_is_set(error);
  }
  bool source_exists = false;
  int source_descriptor = baca_state_open_regular_at(
      old_root, "baca.db", &source_exists, "legacy database", error);
  if (source_descriptor < 0) {
    const bool success = !baca_error_is_set(error);
    (void)close(old_root);
    return success;
  }

  BacaStateTemp temporary = {.descriptor = -1};
  char *source_path = nullptr;
  char *target_path = nullptr;
  bool imported =
      baca_state_create_database_temp(new_root, &temporary, error) &&
      baca_state_regular_matches_at(old_root, "baca.db", source_descriptor,
                                    "legacy database", error) &&
      (source_path = baca_state_database_path(old_root, "baca.db", error)) !=
          nullptr &&
      (target_path = baca_state_database_path(new_root, temporary.name,
                                              error)) != nullptr &&
      baca_state_backup_database(source_path, source_descriptor, target_path,
                                 temporary.descriptor, error) &&
      baca_state_regular_matches_at(old_root, "baca.db", source_descriptor,
                                    "legacy database", error);

  bool temporary_sidecar = false;
  if (imported) {
    imported = baca_state_database_temp_sidecar(new_root, temporary.name,
                                                &temporary_sidecar, error) &&
               !temporary_sidecar;
    if (temporary_sidecar && !baca_error_is_set(error)) {
      baca_error_set(error, BACA_ERROR_DATABASE,
                     "database import left an incomplete temporary sidecar");
    }
  }
  struct stat descriptor_status = {0};
  struct stat path_status = {0};
  if (imported) {
    imported = fstat(temporary.descriptor, &descriptor_status) == 0 &&
               S_ISREG(descriptor_status.st_mode) &&
               fstatat(new_root, temporary.name, &path_status,
                       AT_SYMLINK_NOFOLLOW) == 0 &&
               S_ISREG(path_status.st_mode) &&
               baca_state_same_object(&descriptor_status, &path_status) &&
               fchmod(temporary.descriptor, 0600) == 0 &&
               fsync(temporary.descriptor) == 0;
    if (!imported && !baca_error_is_set(error)) {
      const int saved_errno = errno == 0 ? EIO : errno;
      baca_error_set(error, BACA_ERROR_IO,
                     "could not finish database import temporary file: %s",
                     strerror(saved_errno));
    }
  }
  if (imported) {
    imported = baca_state_new_database_sidecars_absent(new_root, error);
  }
  if (imported) {
    imported = baca_state_publish_temp(new_root, &temporary, BACA_NAME ".db",
                                       true, "current database", error);
  } else {
    baca_state_discard_database_temp(new_root, &temporary, true);
  }

  if (close(source_descriptor) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close legacy database file: %s",
                   strerror(saved_errno));
    imported = false;
  }
  if (close(old_root) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close legacy cache root: %s",
                   strerror(saved_errno));
    imported = false;
  }
  free(source_path);
  free(target_path);
  return imported;
}

static int baca_state_open_legacy_downloads(int cache_base,
                                            bool current_downloads_exist,
                                            bool *exists, BacaError *error) {
  *exists = false;
  int old_root = openat(cache_base, "baca",
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (old_root < 0) {
    const int saved_errno = errno;
    if (saved_errno == ENOENT ||
        (current_downloads_exist &&
         (saved_errno == ELOOP || saved_errno == ENOTDIR))) {
      return -1;
    }
    const BacaErrorCode code = saved_errno == ELOOP || saved_errno == ENOTDIR
                                   ? BACA_ERROR_CORRUPT
                                   : BACA_ERROR_IO;
    baca_error_set(error, code, "could not open legacy cache root: %s",
                   strerror(saved_errno));
    return -1;
  }
  int downloads = openat(old_root, "downloads",
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  const int saved_errno = errno;
  (void)close(old_root);
  if (downloads < 0) {
    if (saved_errno == ENOENT ||
        (current_downloads_exist &&
         (saved_errno == ELOOP || saved_errno == ENOTDIR))) {
      return -1;
    }
    const BacaErrorCode code = saved_errno == ELOOP || saved_errno == ENOTDIR
                                   ? BACA_ERROR_CORRUPT
                                   : BACA_ERROR_IO;
    baca_error_set(error, code, "could not open legacy downloads path: %s",
                   strerror(saved_errno));
    return -1;
  }
  struct stat status = {0};
  if (fstat(downloads, &status) != 0) {
    const int status_errno = errno;
    (void)close(downloads);
    baca_error_set(error, BACA_ERROR_IO,
                   "could not inspect legacy downloads path: %s",
                   strerror(status_errno));
    return -1;
  }
  if (!S_ISDIR(status.st_mode)) {
    (void)close(downloads);
    if (current_downloads_exist) {
      return -1;
    }
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "legacy downloads path is not a safe directory");
    return -1;
  }
  *exists = true;
  return downloads;
}

static bool baca_state_copy_downloads(int old_downloads, int new_downloads,
                                      BacaError *error) {
  DIR *directory = fdopendir(old_downloads);
  if (directory == nullptr) {
    const int saved_errno = errno;
    (void)close(old_downloads);
    baca_error_set(error, BACA_ERROR_IO,
                   "could not enumerate legacy downloads: %s",
                   strerror(saved_errno));
    return false;
  }
  bool copied = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(directory);
    if (entry == nullptr) {
      if (errno != 0) {
        const int saved_errno = errno;
        baca_error_set(error, BACA_ERROR_IO,
                       "could not enumerate legacy downloads: %s",
                       strerror(saved_errno));
        copied = false;
      }
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    struct stat inspected = {0};
    if (fstatat(dirfd(directory), entry->d_name, &inspected,
                AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        continue;
      }
      const int saved_errno = errno;
      baca_error_set(error, BACA_ERROR_IO,
                     "could not inspect legacy download '%s': %s",
                     entry->d_name, strerror(saved_errno));
      copied = false;
      break;
    }
    if (!S_ISREG(inspected.st_mode)) {
      continue;
    }
    bool collision = false;
    if (!baca_state_regular_exists_at(new_downloads, entry->d_name, &collision,
                                      "current download", error)) {
      copied = false;
      break;
    }
    if (collision) {
      continue;
    }

    bool source_exists = false;
    int source =
        baca_state_open_regular_at(dirfd(directory), entry->d_name,
                                   &source_exists, "legacy download", error);
    if (source < 0) {
      if (!baca_error_is_set(error)) {
        continue;
      }
      copied = false;
      break;
    }
    BacaStateTemp temporary = {.descriptor = -1};
    copied = baca_state_create_temp_at(new_downloads, &temporary, error) &&
             baca_state_copy_file(source, temporary.descriptor,
                                  "legacy download", error) &&
             fsync(temporary.descriptor) == 0;
    if (!copied && !baca_error_is_set(error)) {
      const int saved_errno = errno == 0 ? EIO : errno;
      baca_error_set(error, BACA_ERROR_IO,
                     "could not sync imported download: %s",
                     strerror(saved_errno));
    }
    if (copied) {
      copied = baca_state_publish_temp(new_downloads, &temporary, entry->d_name,
                                       true, "current download", error);
    } else {
      baca_state_discard_temp(new_downloads, &temporary);
    }
    if (close(source) != 0 && copied) {
      const int saved_errno = errno;
      baca_error_set(error, BACA_ERROR_IO,
                     "could not close legacy download: %s",
                     strerror(saved_errno));
      copied = false;
    }
    if (!copied) {
      break;
    }
  }
  if (closedir(directory) != 0 && copied) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO, "could not close legacy downloads: %s",
                   strerror(saved_errno));
    copied = false;
  }
  return copied;
}

static bool baca_state_import_downloads(int cache_base, int new_root,
                                        BacaError *error) {
  bool current_exists = false;
  int current = baca_state_open_current_directory_at(
      new_root, "downloads", false, &current_exists, "current downloads path",
      error);
  if (current < 0 && baca_error_is_set(error)) {
    return false;
  }

  bool legacy_exists = false;
  int legacy = baca_state_open_legacy_downloads(cache_base, current_exists,
                                                &legacy_exists, error);
  if (legacy < 0) {
    const bool success = !baca_error_is_set(error);
    (void)close(current);
    return success;
  }
  if (!current_exists) {
    current = baca_state_open_current_directory_at(
        new_root, "downloads", true, &current_exists, "current downloads path",
        error);
    if (current < 0) {
      (void)close(legacy);
      return false;
    }
  }
  const bool copied = baca_state_copy_downloads(legacy, current, error);
  if (close(current) != 0 && copied) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close current downloads path: %s",
                   strerror(saved_errno));
    return false;
  }
  return copied;
}

static bool baca_state_marker_exists(int config_base, bool *exists,
                                     BacaError *error);

static bool baca_state_write_marker(int config_base, BacaError *error) {
  BacaStateTemp temporary = {.descriptor = -1};
  bool written = baca_state_create_temp_at(config_base, &temporary, error) &&
                 baca_state_write_all(
                     temporary.descriptor, baca_state_import_marker_contents,
                     sizeof(baca_state_import_marker_contents) - 1U,
                     "state import completion marker", error) &&
                 fsync(temporary.descriptor) == 0;
  if (!written && !baca_error_is_set(error)) {
    const int saved_errno = errno == 0 ? EIO : errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not sync state import completion marker: %s",
                   strerror(saved_errno));
  }
  if (written) {
    written = baca_state_publish_temp(config_base, &temporary,
                                      BACA_STATE_IMPORT_MARKER, true,
                                      "state import completion marker", error);
  } else {
    baca_state_discard_temp(config_base, &temporary);
  }
  if (written) {
    bool marker_exists = false;
    written = baca_state_marker_exists(config_base, &marker_exists, error) &&
              marker_exists;
  }
  return written;
}

static bool baca_state_marker_exists(int config_base, bool *exists,
                                     BacaError *error) {
  bool present = false;
  int descriptor = baca_state_open_regular_at(
      config_base, BACA_STATE_IMPORT_MARKER, &present,
      "state import completion marker", error);
  *exists = false;
  if (descriptor < 0) {
    return !baca_error_is_set(error);
  }

  char contents[sizeof(baca_state_import_marker_contents)] = {0};
  size_t offset = 0U;
  while (offset < sizeof(contents)) {
    const ssize_t count =
        read(descriptor, contents + offset, sizeof(contents) - offset);
    if (count > 0) {
      offset += (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      const int saved_errno = errno;
      (void)close(descriptor);
      baca_error_set(error, BACA_ERROR_IO,
                     "could not read state import completion marker: %s",
                     strerror(saved_errno));
      return false;
    }
    break;
  }
  const bool valid =
      offset == sizeof(baca_state_import_marker_contents) - 1U &&
      memcmp(contents, baca_state_import_marker_contents, offset) == 0;
  if (close(descriptor) != 0) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close state import completion marker: %s",
                   strerror(saved_errno));
    return false;
  }
  if (!valid) {
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "state import completion marker has invalid contents");
    return false;
  }
  *exists = true;
  return true;
}

static bool baca_state_validate_current_layout(int config_base, int cache_base,
                                               BacaError *error) {
  bool config_root_exists = false;
  int config_root = baca_state_open_current_directory_at(
      config_base, BACA_NAME, false, &config_root_exists, "current config root",
      error);
  if (config_root < 0 && baca_error_is_set(error)) {
    return false;
  }
  if (config_root >= 0) {
    bool config_exists = false;
    const bool config_valid = baca_state_regular_exists_at(
        config_root, "config.ini", &config_exists, "current config", error);
    if (!baca_state_close(&config_root, "current config root", error) ||
        !config_valid) {
      return false;
    }
  }

  bool cache_root_exists = false;
  int cache_root = baca_state_open_current_directory_at(
      cache_base, BACA_NAME, false, &cache_root_exists, "current cache root",
      error);
  if (cache_root < 0) {
    return !baca_error_is_set(error);
  }

  bool database_exists = false;
  bool valid = baca_state_regular_exists_at(
      cache_root, BACA_NAME ".db", &database_exists, "current database", error);
  if (valid && !database_exists) {
    valid = baca_state_new_database_sidecars_absent(cache_root, error);
  }
  bool downloads_exists = false;
  int downloads = -1;
  if (valid) {
    downloads = baca_state_open_current_directory_at(
        cache_root, "downloads", false, &downloads_exists,
        "current downloads path", error);
    valid = downloads >= 0 || !baca_error_is_set(error);
  }
  if (downloads >= 0 &&
      !baca_state_close(&downloads, "current downloads path", error)) {
    valid = false;
  }
  if (!baca_state_close(&cache_root, "current cache root", error)) {
    valid = false;
  }
  return valid;
}

bool baca_state_migrate(BacaError *error) {
  if (error == nullptr) {
    return false;
  }
  baca_error_clear(error);
  BacaStatePaths paths = {0};
  int cache_base = -1;
  int config_base = -1;
  int new_cache_root = -1;
  int lock = -1;
  bool imported = false;

  if (!baca_state_paths_init(&paths, error)) {
    return false;
  }
  cache_base = baca_state_open_base(paths.cache_base, error);
  if (cache_base < 0) {
    goto cleanup;
  }
  lock = baca_state_lock(cache_base, error);
  if (lock < 0) {
    goto cleanup;
  }
  config_base = baca_state_open_base(paths.config_base, error);
  if (config_base < 0) {
    goto cleanup;
  }

  bool marker_exists = false;
  if (!baca_state_marker_exists(config_base, &marker_exists, error)) {
    goto cleanup;
  }
  if (marker_exists) {
    imported =
        baca_state_validate_current_layout(config_base, cache_base, error);
    goto cleanup;
  }

  bool new_cache_exists = false;
  new_cache_root = baca_state_open_current_directory_at(
      cache_base, BACA_NAME, true, &new_cache_exists, "current cache root",
      error);
  if (new_cache_root < 0 || !baca_state_import_config(config_base, error) ||
      !baca_state_import_database(cache_base, new_cache_root, error) ||
      !baca_state_import_downloads(cache_base, new_cache_root, error) ||
      !baca_state_write_marker(config_base, error)) {
    goto cleanup;
  }
  imported = true;

cleanup:
  if (config_base >= 0 && close(config_base) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close config state base: %s",
                   strerror(saved_errno));
    imported = false;
  }
  if (new_cache_root >= 0 && close(new_cache_root) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close current cache root: %s",
                   strerror(saved_errno));
    imported = false;
  }
  if (lock >= 0 && close(lock) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO,
                   "could not close state import lock: %s",
                   strerror(saved_errno));
    imported = false;
  }
  if (cache_base >= 0 && close(cache_base) != 0 && imported) {
    const int saved_errno = errno;
    baca_error_set(error, BACA_ERROR_IO, "could not close cache state base: %s",
                   strerror(saved_errno));
    imported = false;
  }
  baca_state_paths_free(&paths);
  return imported;
}
