/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdio.h>
#include <string.h>

#include "rcvrpc.h"

void rcvrpc_options_init(rcvrpc_options_t* options) { memset(options, 0, sizeof(*options)); }
int rcvrpc_parse_option(rcvrpc_options_t* options, int argc, char** argv, int* index) {
    const char* names[] = {"--rpc-port",    "--rpc-bind",   "--rpc-origin",
                           "--rpc-no-auth", "--no-console", "--exit-on-stdin-eof"};
    size_t i;
    (void)options;
    (void)argc;
    for (i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if (!strcmp(argv[*index], names[i])) {
            fprintf(stderr, "error: this build has no RPC support (MRTK_WITH_RPC=OFF)\n");
            return -1;
        }
    }
    return 0;
}
int rcvrpc_options_validate(rcvrpc_options_t* options) {
    (void)options;
    return 1;
}
void rcvrpc_options_free(rcvrpc_options_t* options) { (void)options; }
int rcvrpc_start(rcvrpc_t** rpc, const rcvrpc_options_t* options, rcvcmd_t* cmd) {
    (void)options;
    (void)cmd;
    *rpc = NULL;
    return 1;
}
void rcvrpc_stop(rcvrpc_t* rpc) { (void)rpc; }
