/*------------------------------------------------------------------------------
 * mrtk_cli.c : shared CLI helpers for mrtk subcommands
 *
 * Copyright (C) 2026, MRTKLIB Contributors, All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *-----------------------------------------------------------------------------*/
#include "mrtklib/mrtk_cli.h"

#include <string.h>

void mrtk_normalize_args(int argc, char** argv, const mrtk_optmap_t* map) {
    int i, j;
    if (!map || !argv) {
        return;
    }
    for (i = 1; i < argc; i++) {
        if (!argv[i]) {
            continue;
        }
        for (j = 0; map[j].long_opt; j++) {
            if (strcmp(argv[i], map[j].long_opt) == 0) {
                argv[i] = (char*)map[j].short_opt;
                break;
            }
        }
    }
}

int mrtk_is_help_flag(const char* arg) {
    if (!arg) {
        return 0;
    }
    return strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0;
}
