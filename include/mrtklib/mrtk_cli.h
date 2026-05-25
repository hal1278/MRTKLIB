/*------------------------------------------------------------------------------
 * mrtk_cli.h : shared CLI helpers for mrtk subcommands
 *
 * Copyright (C) 2026, MRTKLIB Contributors, All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * description : Small utilities used by every `mrtk <subcommand>` entry point
 *               to keep CLI surface (long-option aliases, help-flag detection)
 *               consistent without rewriting per-subcommand argument parsers.
 *-----------------------------------------------------------------------------*/
#ifndef MRTK_CLI_H
#define MRTK_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Long-option to short-option alias entry.
 *
 * Each subcommand defines a NULL-terminated array of these to declare which
 * `--long` flags translate to which `-short` flag in its existing parser. Used
 * by mrtk_normalize_args() as a pre-pass over argv.
 */
typedef struct {
    const char* long_opt;  /**< long-form flag, e.g. "--config" */
    const char* short_opt; /**< short-form flag the parser already handles, e.g. "-k" */
} mrtk_optmap_t;

/**
 * @brief Translate `--long` flags to their `-short` aliases in argv (in place).
 *
 * Walks argv[1..argc-1] and, for each entry that exactly matches a
 * `map[i].long_opt`, swaps the pointer to point at `map[i].short_opt` instead.
 * Only the argv pointers are reassigned; the underlying argument strings are
 * never mutated. argv[0] is left untouched. Pass NULL or an empty map to no-op.
 *
 * @param[in]      argc  argument count
 * @param[in,out]  argv  argument vector (pointers may be reassigned)
 * @param[in]      map   NULL-terminated alias table (terminator: {NULL, NULL})
 */
void mrtk_normalize_args(int argc, char** argv, const mrtk_optmap_t* map);

/**
 * @brief Returns 1 if `arg` is a generic help flag (`-h` or `--help`), else 0.
 *
 * Subcommands that have repurposed `-h` (e.g. `post` uses it for fix-and-hold
 * AR) must NOT use this helper and should match `--help` (and any historical
 * short flag like `-?`) manually.
 *
 * @param[in]  arg  argument string (must not be NULL)
 */
int mrtk_is_help_flag(const char* arg);

#ifdef __cplusplus
}
#endif

#endif /* MRTK_CLI_H */
