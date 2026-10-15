/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef RCVRPC_H
#define RCVRPC_H

#include "rcvcmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int port, no_console, no_auth, exit_on_stdin_eof;
    const char* bind;
    const char** origins;
    size_t origin_count;
    char token[257];
    int token_invalid;
} rcvrpc_options_t;
typedef struct rcvrpc rcvrpc_t;

/** @brief Capture and remove the token environment variable before threads exist. */
void rcvrpc_options_init(rcvrpc_options_t* options);
/** @brief Parse one RPC option, advancing index over its value.
 * @return 1 if consumed, 0 if unrelated, -1 on an error already printed to stderr.
 */
int rcvrpc_parse_option(rcvrpc_options_t* options, int argc, char** argv, int* index);
/** @brief Validate startup options and obtain a token when the endpoint needs one. */
int rcvrpc_options_validate(rcvrpc_options_t* options);
/** @brief Erase token storage and free the origin list after the endpoint stops. */
void rcvrpc_options_free(rcvrpc_options_t* options);
/** @brief Start the endpoint and optional stdin watcher, then print its ready line.
 * @return 1 on success (also when disabled), 0 on startup failure.
 */
int rcvrpc_start(rcvrpc_t** rpc, const rcvrpc_options_t* options, rcvcmd_t* cmd);
/** @brief Join stdin watcher, close connections with 1001 and stop CivetWeb. */
void rcvrpc_stop(rcvrpc_t* rpc);

#ifdef __cplusplus
}
#endif
#endif
