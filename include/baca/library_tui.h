#pragma once

#include "baca/catalog.h"
#include "baca/config.h"
#include "baca/library.h"

typedef enum BacaLibraryCommand : uint8_t {
    BACA_LIBRARY_COMMAND_NONE = 0,
    BACA_LIBRARY_COMMAND_QUIT,
    BACA_LIBRARY_COMMAND_OPEN,
    BACA_LIBRARY_COMMAND_REFRESH,
    BACA_LIBRARY_COMMAND_SET_ROOT,
} BacaLibraryCommand;

typedef struct BacaLibraryAction {
    BacaLibraryCommand command;
    char *path;
    BacaLibrarySort sort;
} BacaLibraryAction;

[[nodiscard]] int baca_library_tui_run(const BacaConfig *config, const BacaHistory *history, BacaCatalog *catalog,
                                        bool setup_required, BacaLibrarySort sort, const char *selected_filepath,
                                        const char *context, BacaLibraryAction *action, BacaError *error);
