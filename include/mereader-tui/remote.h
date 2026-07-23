#pragma once

#include "mereader-tui/common.h"

#define MEREADER_TUI_REMOTE_MAX_BYTES (256U * 1024U * 1024U)

typedef struct MereaderTuiRemoteFile {
    char *url;
    char *path;
} MereaderTuiRemoteFile;

[[nodiscard]] bool mereader_tui_remote_is_url(const char *value);
[[nodiscard]] bool mereader_tui_remote_validate_url(const char *url, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_remote_fetch(const char *url, MereaderTuiRemoteFile *file, MereaderTuiError *error);
void mereader_tui_remote_file_free(MereaderTuiRemoteFile *file);
