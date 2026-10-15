/* SPDX-License-Identifier: BSD-2-Clause */
/** @file rcvrpc.c @brief POSIX WebSocket transport and JSON-RPC envelope. */
#include "rcvrpc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#include <civetweb.h>
#include <yyjson.h>

#if CIVETWEB_VERSION_MAJOR < 1 || (CIVETWEB_VERSION_MAJOR == 1 && CIVETWEB_VERSION_MINOR < 16)
#error "MRTK RPC requires CivetWeb 1.16 or newer"
#endif
#if !defined(YYJSON_VERSION_HEX) || YYJSON_VERSION_HEX < 0x000A00
#error "MRTK RPC requires yyjson 0.10 or newer"
#endif

#include "mrtklib/mrtk_trace.h"
#include "mrtklib/mrtk_version.h"
#include "rcvjson.h"

#define RPC_MESSAGE_MAX 65536

typedef struct rpc_client {
    pthread_mutex_t lock;
    struct mg_connection* connection;
    struct rpc_client* next;
    size_t length;
    int fragmented, close_sent;
    char message[RPC_MESSAGE_MAX];
} rpc_client_t;

typedef struct {
    char** paths;
    size_t count;
} rpc_overwrite_t;

struct rcvrpc {
    struct mg_context* context;
    const rcvrpc_options_t* options;
    rcvcmd_t* cmd;
    pthread_mutex_t lock;
    pthread_mutex_t log_lock;
    pthread_mutex_t lifecycle_lock;
    pthread_cond_t lifecycle_cond;
    pthread_t lifecycle_thread;
    int lifecycle_started, lifecycle_exit;
    rcvcmd_claim_t pending_claim;
    rpc_overwrite_t pending_overwrite;
    rpc_client_t* clients;
    int stopping, watcher_started;
    pthread_t watcher;
    char listen_error[512];
};

static int startup_error(const char* text) {
    fprintf(stderr, "error: %s\n", text);
    return 0;
}

static int terminal_stderr(void) { return isatty(STDERR_FILENO); }
static int remove_token_environment(void) { return unsetenv("MRTK_RPC_TOKEN") == 0; }

static int random_bytes(unsigned char* bytes, size_t size) {
    size_t offset = 0;
#ifndef __linux__
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
#endif
    while (offset < size) {
#ifdef __linux__
        ssize_t n = getrandom(bytes + offset, size - offset, 0);
#else
        ssize_t n = read(fd, bytes + offset, size - offset);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += (size_t)n;
    }
#ifndef __linux__
    close(fd);
#endif
    return offset == size;
}

void rcvrpc_options_init(rcvrpc_options_t* options) {
    const char* token = getenv("MRTK_RPC_TOKEN");
    size_t n = token ? strlen(token) : 0;
    memset(options, 0, sizeof(*options));
    options->port = -1;
    options->bind = "127.0.0.1";
    if (token) {
        options->token_invalid = n < 1 || n > 256;
        if (!options->token_invalid) memcpy(options->token, token, n + 1);
    }
    if (!remove_token_environment()) options->token_invalid = 1;
}

int rcvrpc_parse_option(rcvrpc_options_t* options, int argc, char** argv, int* index) {
    const char* name = argv[*index];
    if (!strcmp(name, "--no-console"))
        options->no_console = 1;
    else if (!strcmp(name, "--rpc-no-auth"))
        options->no_auth = 1;
    else if (!strcmp(name, "--exit-on-stdin-eof"))
        options->exit_on_stdin_eof = 1;
    else if (!strcmp(name, "--rpc-port") || !strcmp(name, "--rpc-bind") || !strcmp(name, "--rpc-origin")) {
        const char* value;
        if (*index + 1 >= argc) {
            startup_error("RPC option requires a value");
            return -1;
        }
        value = argv[++*index];
        if (!strcmp(name, "--rpc-port")) {
            char* end;
            long port;
            errno = 0;
            port = strtol(value, &end, 10);
            if (!*value || *end || errno || port < 0 || port > 65535) {
                startup_error("--rpc-port requires an integer from 0 to 65535");
                return -1;
            }
            options->port = (int)port;
        } else if (!strcmp(name, "--rpc-bind"))
            options->bind = value;
        else {
            const char** origins = realloc(options->origins, (options->origin_count + 1) * sizeof(*origins));
            if (!origins) {
                startup_error("RPC origin allocation failed");
                return -1;
            }
            options->origins = origins;
            options->origins[options->origin_count++] = value;
        }
    } else
        return 0;
    return 1;
}

static int loopback_address(const char* address) {
    struct in_addr v4;
    struct in6_addr v6;
    return (inet_pton(AF_INET, address, &v4) == 1 && (ntohl(v4.s_addr) >> 24) == 127) ||
           (inet_pton(AF_INET6, address, &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6));
}

int rcvrpc_options_validate(rcvrpc_options_t* options) {
    struct in_addr v4;
    struct in6_addr v6;
    /* A bind address is one IP literal, never CivetWeb's multi-listener syntax. */
    if (inet_pton(AF_INET, options->bind, &v4) != 1 && inet_pton(AF_INET6, options->bind, &v6) != 1)
        return startup_error("--rpc-bind requires an IPv4 or IPv6 address");
    if (options->port >= 0) {
        size_t i;
        if (options->token_invalid)
            return startup_error("invalid MRTK_RPC_TOKEN or unable to remove it from the environment");
        for (i = 0; options->token[i]; i++) {
            unsigned char c = (unsigned char)options->token[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return startup_error("invalid MRTK_RPC_TOKEN");
        }
    }
    if (options->no_console && options->port < 0) return startup_error("--no-console requires --rpc-port");
    if (options->exit_on_stdin_eof && !options->no_console)
        return startup_error("--exit-on-stdin-eof requires --no-console");
    if (options->no_auth && !loopback_address(options->bind))
        return startup_error("--rpc-no-auth requires a loopback bind address");
    if (options->no_auth) fprintf(stderr, "warning: --rpc-no-auth disables RPC authentication\n");
    if (options->port < 0 || options->no_auth || options->token[0]) return 1;
    if (!terminal_stderr()) return startup_error("MRTK_RPC_TOKEN must be set when stderr is not a terminal");
    {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        unsigned char bytes[32];
        unsigned int accumulator = 0;
        int bits = 0;
        size_t i, n = 0;
        if (!random_bytes(bytes, sizeof(bytes))) return startup_error("could not generate MRTK_RPC_TOKEN");
        for (i = 0; i < sizeof(bytes); i++) {
            accumulator = (accumulator << 8) | bytes[i];
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                options->token[n++] = alphabet[(accumulator >> bits) & 63];
            }
        }
        if (bits) options->token[n++] = alphabet[(accumulator << (6 - bits)) & 63];
        options->token[n] = 0;
        fprintf(stderr, "MRTK RPC token: %s\n", options->token);
    }
    return 1;
}

void rcvrpc_options_free(rcvrpc_options_t* options) {
    volatile char* token = options->token;
    size_t i;
    for (i = 0; i < sizeof(options->token); i++) token[i] = 0;
    free(options->origins);
}

static unsigned char ascii_lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c; }
static int ascii_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a++) != ascii_lower((unsigned char)*b++)) return 0;
    }
    return *a == *b;
}

/* Always visit the entire supplied entry and the entire configured token. */
static int token_equal(const char* entry, size_t length, const char* token) {
    size_t i, expected = strlen(token), n = length > expected ? length : expected;
    volatile unsigned int difference = (unsigned int)(length ^ expected);
    for (i = 0; i < n; i++)
        difference |= (i < length ? (unsigned char)entry[i] : 0) ^ (i < expected ? (unsigned char)token[i] : 0);
    return difference == 0;
}

static int reject_http(struct mg_connection* connection, int status, const char* reason) {
    mg_printf(connection, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n%s\r\n", status, reason,
              status == 426 ? "Upgrade: websocket\r\n" : "");
    return 1;
}

static int begin_request(struct mg_connection* connection) {
    const struct mg_request_info* request = mg_get_request_info(connection);
    /* After a handled begin_request CivetWeb drains the request body unless the
     * connection must close, so a body trickled below the read timeout could hold
     * a worker. Mark the connection closing before answering. */
    if (!strcmp(request->local_uri, "/rpc") && !mg_get_header(connection, "Upgrade")) {
        mg_disable_connection_keep_alive(connection);
        reject_http(connection, 426, "Upgrade Required");
        return 426;
    }
    if (strcmp(request->local_uri, "/rpc") && mg_get_header(connection, "Upgrade")) {
        mg_disable_connection_keep_alive(connection);
        reject_http(connection, 404, "Not Found");
        return 404;
    }
    return 0;
}

static int connect_ws(const struct mg_connection* connection, void* arg) {
    rcvrpc_t* rpc = arg;
    const struct mg_request_info* request = mg_get_request_info(connection);
    int i, authenticated = 0, origin_allowed = 1;
    /* CivetWeb selects before this callback and does not trim entry endings.
     * Requiring its selection prevents upgrades without the protocol header. */
    int protocol = request->acceptedWebSocketSubprotocol && !strcmp(request->acceptedWebSocketSubprotocol, "mrtk-rpc");
    if (strcmp(request->local_uri, "/rpc")) return reject_http((struct mg_connection*)connection, 404, "Not Found");
    for (i = 0; i < request->num_headers; i++) {
        const char* value = request->http_headers[i].value;
        if (ascii_equal(request->http_headers[i].name, "Sec-WebSocket-Protocol")) {
            while (*value) {
                const char* end = strchr(value, ',');
                const char* trimmed;
                size_t length;
                if (!end) end = value + strlen(value);
                while (value < end && (*value == ' ' || *value == '\t')) value++;
                trimmed = end;
                while (trimmed > value && (trimmed[-1] == ' ' || trimmed[-1] == '\t')) trimmed--;
                length = (size_t)(trimmed - value);
                if (length >= 11 && !memcmp(value, "mrtk-token.", 11))
                    authenticated |= token_equal(value + 11, length - 11, rpc->options->token);
                value = *end ? end + 1 : end;
            }
        } else if (ascii_equal(request->http_headers[i].name, "Origin")) {
            size_t j;
            int matched = 0;
            for (j = 0; j < rpc->options->origin_count; j++) matched |= ascii_equal(value, rpc->options->origins[j]);
            origin_allowed &= matched;
        }
    }
    if (!protocol) return reject_http((struct mg_connection*)connection, 400, "Bad Request");
    if (!origin_allowed) return reject_http((struct mg_connection*)connection, 403, "Forbidden");
    if (!rpc->options->no_auth && !authenticated)
        return reject_http((struct mg_connection*)connection, 401, "Unauthorized");
    return 0;
}

static void ready_ws(struct mg_connection* connection, void* arg) {
    rcvrpc_t* rpc = arg;
    rpc_client_t* client = calloc(1, sizeof(*client));
    if (!client) {
        const char code[] = {3, (char)243};
        mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, code, 2);
        return;
    }
    if (pthread_mutex_init(&client->lock, NULL)) {
        const char code[] = {3, (char)243};
        mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, code, 2);
        free(client);
        return;
    }
    client->connection = connection;
    mg_set_user_connection_data(connection, client);
    pthread_mutex_lock(&rpc->lock);
    if (rpc->stopping) {
        const char code[] = {3, (char)233};
        mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, code, 2);
        mg_set_user_connection_data(connection, NULL);
        pthread_mutex_destroy(&client->lock);
        free(client);
        pthread_mutex_unlock(&rpc->lock);
        return;
    }
    client->next = rpc->clients;
    rpc->clients = client;
    pthread_mutex_unlock(&rpc->lock);
}

static void close_ws(const struct mg_connection* connection, void* arg) {
    rcvrpc_t* rpc = arg;
    rpc_client_t** cursor;
    pthread_mutex_lock(&rpc->lock);
    for (cursor = &rpc->clients; *cursor; cursor = &(*cursor)->next) {
        if ((*cursor)->connection == connection) {
            rpc_client_t* client = *cursor;
            pthread_mutex_lock(&client->lock);
            *cursor = client->next;
            mg_set_user_connection_data(connection, NULL);
            pthread_mutex_unlock(&client->lock);
            pthread_mutex_destroy(&client->lock);
            free(client);
            break;
        }
    }
    pthread_mutex_unlock(&rpc->lock);
}

/* Caller holds the connection mutex, including for the final shutdown Close. */
static void close_frame(struct mg_connection* connection, const char* data, size_t length) {
    rpc_client_t* client = mg_get_user_connection_data(connection);
    if (client->close_sent) return;
    client->close_sent = 1;
    mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, data, length);
}

static void close_code(struct mg_connection* connection, int code) {
    char bytes[] = {(char)(code >> 8), (char)code};
    close_frame(connection, bytes, 2);
}

static yyjson_mut_val* rpc_error(yyjson_mut_doc* doc, int code, const char* message, const char* reason) {
    yyjson_mut_val* error = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, error, "code", code);
    yyjson_mut_obj_add_str(doc, error, "message", message);
    if (reason) {
        yyjson_mut_val* data = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, data, "reason", reason);
        yyjson_mut_obj_add_val(doc, error, "data", data);
    }
    return error;
}

static void free_overwrite(rpc_overwrite_t* overwrite) {
    size_t i;
    for (i = 0; i < overwrite->count; i++) free(overwrite->paths[i]);
    free(overwrite->paths);
    memset(overwrite, 0, sizeof(*overwrite));
}

static void rpc_notice(void* arg, const rcvcmd_notice_t* notice) {
    (void)arg;
    (void)notice; /* The command layer already traces each notice into logMessage. */
}

static int rpc_confirm_overwrite(void* arg, const char* path) {
    rpc_overwrite_t* overwrite = arg;
    FILE* file = fopen(path, "r");
    size_t i;
    if (!file) return RCVCMD_OVERWRITE_ALLOW;
    fclose(file);
    for (i = 0; i < overwrite->count; i++)
        if (!strcmp(path, overwrite->paths[i])) return RCVCMD_OVERWRITE_ALLOW;
    return RCVCMD_OVERWRITE_REFUSE_CONTINUE;
}

static void* execute_lifecycle(void* arg) {
    rcvrpc_t* rpc = arg;
    pthread_mutex_lock(&rpc->lifecycle_lock);
    for (;;) {
        rcvcmd_claim_t claim;
        rpc_overwrite_t overwrite;
        rcvcmd_io_t io = {NULL, rpc_notice, rpc_confirm_overwrite};
        while (!rpc->pending_claim.seq && !rpc->lifecycle_exit)
            pthread_cond_wait(&rpc->lifecycle_cond, &rpc->lifecycle_lock);
        if (!rpc->pending_claim.seq && rpc->lifecycle_exit) break;
        claim = rpc->pending_claim;
        overwrite = rpc->pending_overwrite;
        rpc->pending_claim.seq = 0;
        memset(&rpc->pending_overwrite, 0, sizeof(rpc->pending_overwrite));
        pthread_mutex_unlock(&rpc->lifecycle_lock);
        io.arg = &overwrite;
        rcvcmd_execute(rpc->cmd, &claim, &io);
        free_overwrite(&overwrite);
        pthread_mutex_lock(&rpc->lifecycle_lock);
    }
    pthread_mutex_unlock(&rpc->lifecycle_lock);
    return NULL;
}

/* Requests retain raw numbers to preserve IDs. Parse integer-valued JSON
 * numbers exactly, including exponent notation, saturating at cap. This avoids
 * accepting a rounded fractional cursor near 2^53. JSON syntax is already valid. */
static int integer_param(yyjson_val* value, uint64_t cap, uint64_t* out) {
    const char* raw = yyjson_get_raw(value);
    size_t n, i = 0, digits = 0, fraction = 0, trailing = 0, significant;
    int negative = 0, dot = 0, exponent_sign = 1, exponent = 0;
    long shift;
    uint64_t number = 0;
    if (!raw) return 0;
    n = yyjson_get_len(value);
    if (raw[i] == '-') {
        negative = 1;
        i++;
    }
    for (; i < n && raw[i] != 'e' && raw[i] != 'E'; i++) {
        if (raw[i] == '.') {
            dot = 1;
            continue;
        }
        digits++;
        fraction += dot;
        trailing = raw[i] == '0' ? trailing + 1 : 0;
    }
    if (i < n) {
        i++;
        if (raw[i] == '-' || raw[i] == '+') {
            exponent_sign = raw[i] == '-' ? -1 : 1;
            i++;
        }
        for (; i < n; i++)
            if (exponent < 100000) exponent = exponent * 10 + raw[i] - '0';
    }
    if (trailing == digits) {
        *out = 0;
        return 1;
    }
    if (negative) return 0;
    shift = (long)exponent_sign * exponent - (long)fraction + (long)trailing;
    if (shift < 0) return 0;
    significant = digits - trailing;
    for (i = 0; significant; i++) {
        unsigned int digit;
        if (raw[i] == '.') continue;
        digit = (unsigned int)(raw[i] - '0');
        number = number > (cap - digit) / 10 ? cap : number * 10 + digit;
        significant--;
    }
    while (shift-- > 0 && number < cap) number = number > cap / 10 ? cap : number * 10;
    *out = number;
    return 1;
}

/* Method-specific validation stays separate from U14's envelope handling. */
static yyjson_mut_val* dispatch(rcvrpc_t* rpc, yyjson_mut_doc* doc, yyjson_val* method, yyjson_val* params,
                                yyjson_mut_val** error) {
    const char* name = yyjson_get_str(method);
    int start = !strcmp(name, "start"), restart = !strcmp(name, "restart"), stop = !strcmp(name, "stop");
    int events = !strcmp(name, "getEvents");
    yyjson_mut_val* result;
    yyjson_val *key, *value;
    yyjson_obj_iter iter;
    if (!start && !restart && !stop && !events && strcmp(name, "getStatus") && strcmp(name, "sync") &&
        strcmp(name, "getVersion") && strcmp(name, "shutdown")) {
        *error = rpc_error(doc, -32601, "Method not found", NULL);
        return NULL;
    }
    yyjson_obj_iter_init(params, &iter);
    while ((key = yyjson_obj_iter_next(&iter))) {
        if ((start || restart) && yyjson_equals_str(key, "overwrite")) continue;
        if (events && (yyjson_equals_str(key, "after") || yyjson_equals_str(key, "limit"))) continue;
        goto invalid;
    }
    if (!strcmp(name, "getStatus") || !strcmp(name, "sync")) {
        rcvcmd_snapshot_t snapshot;
        rcvcmd_snapshot(rpc->cmd, &snapshot);
        return rcvjson_snapshot(doc, &snapshot);
    }
    if (events) {
        uint64_t after, limit = 1000;
        rcvevent_t* copy;
        rcvevent_read_t read;
        yyjson_mut_val* array;
        int i;
        if (!integer_param(yyjson_obj_get(params, "after"), UINT64_C(9007199254740992), &after) ||
            after > UINT64_C(9007199254740991))
            goto invalid;
        value = yyjson_obj_get(params, "limit");
        if (value && (!integer_param(value, 1000, &limit) || !limit)) goto invalid;
        copy = malloc((size_t)limit * sizeof(*copy));
        if (!copy) goto internal;
        if (rcvcmd_read_events(rpc->cmd, after, copy, (int)limit, &read) < 0) {
            free(copy);
            goto invalid;
        }
        result = yyjson_mut_obj(doc);
        array = yyjson_mut_arr(doc);
        for (i = 0; i < read.n; i++) yyjson_mut_arr_append(array, rcvjson_event(doc, &copy[i]));
        free(copy);
        yyjson_mut_obj_add_val(doc, result, "events", array);
        yyjson_mut_obj_add_uint(doc, result, "next", read.next);
        yyjson_mut_obj_add_bool(doc, result, "more", read.more);
        yyjson_mut_obj_add_bool(doc, result, "lost", read.lost);
        if (read.lost) yyjson_mut_obj_add_uint(doc, result, "oldestSeq", read.oldest);
        return result;
    }
    if (start || stop || restart) {
        rcvcmd_claim_t claim;
        rcvcmd_result_t accepted;
        rpc_overwrite_t overwrite = {0};
        size_t i, max;
        value = yyjson_obj_get(params, "overwrite");
        if (value) {
            yyjson_val* path;
            if (!yyjson_is_arr(value)) goto invalid;
            yyjson_arr_foreach(value, i, max, path) if (!yyjson_is_str(path)) goto invalid;
            overwrite.count = yyjson_arr_size(value);
            if (overwrite.count) {
                overwrite.paths = calloc(overwrite.count, sizeof(*overwrite.paths));
                if (!overwrite.paths) goto internal;
            }
            yyjson_arr_foreach(value, i, max, path) {
                size_t length = yyjson_get_len(path);
                overwrite.paths[i] = malloc(length + 1);
                if (!overwrite.paths[i]) {
                    free_overwrite(&overwrite);
                    goto internal;
                }
                memcpy(overwrite.paths[i], yyjson_get_str(path), length + 1);
                /* Embedded NUL cannot agree to a shorter filesystem path. */
                if (strlen(overwrite.paths[i]) != length) overwrite.paths[i][0] = 0;
            }
        }
        accepted = rcvcmd_claim(rpc->cmd,
                                start     ? RCVCMD_OP_START
                                : restart ? RCVCMD_OP_RESTART
                                          : RCVCMD_OP_STOP,
                                &claim);
        if (claim.seq) {
            pthread_mutex_lock(&rpc->lifecycle_lock);
            rpc->pending_claim = claim;
            rpc->pending_overwrite = overwrite;
            pthread_cond_signal(&rpc->lifecycle_cond);
            pthread_mutex_unlock(&rpc->lifecycle_lock);
        } else
            free_overwrite(&overwrite);
        if (accepted.kind != RCVCMD_SUCCESS) {
            int busy = accepted.kind == RCVCMD_BUSY;
            *error = rpc_error(doc,
                               busy                                      ? 1001
                               : accepted.kind == RCVCMD_ALREADY_RUNNING ? 1002
                                                                         : 1003,
                               busy                                      ? "Server is busy"
                               : accepted.kind == RCVCMD_ALREADY_RUNNING ? "Already running"
                                                                         : "Shutting down",
                               busy                                      ? "busy"
                               : accepted.kind == RCVCMD_ALREADY_RUNNING ? "alreadyRunning"
                                                                         : "shuttingDown");
            if (busy)
                yyjson_mut_obj_add_str(doc, yyjson_mut_obj_get(*error, "data"), "state",
                                       rcvjson_state_name(accepted.state));
            return NULL;
        }
        result = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, result, "seq", accepted.seq);
        return result;
    }
    result = yyjson_mut_obj(doc);
    if (!strcmp(name, "getVersion")) {
        yyjson_mut_obj_add_str(doc, result, "protocolVersion", "0.1");
        yyjson_mut_obj_add_str(doc, result, "mrtklibVersion", MRTKLIB_VERSION_STRING);
        yyjson_mut_obj_add_str(doc, result, "authentication", rpc->options->no_auth ? "none" : "token");
    } else {
        rcvcmd_request_shutdown(rpc->cmd);
        yyjson_mut_obj_add_uint(doc, result, "seq", rcvcmd_last_seq(rpc->cmd));
    }
    return result;
invalid:
    *error = rpc_error(doc, -32602, "Invalid parameters", "invalidParams");
    return NULL;
internal:
    *error = rpc_error(doc, -32603, "Internal error", NULL);
    return NULL;
}

static void handle_message(rcvrpc_t* rpc, struct mg_connection* connection, const char* message, size_t length) {
    /* Raw numeric values preserve every ID digit, including numbers beyond uint64. */
    yyjson_doc* request_doc = yyjson_read(message, length, YYJSON_READ_NUMBER_AS_RAW);
    yyjson_val* request = yyjson_doc_get_root(request_doc);
    yyjson_val *id = yyjson_obj_get(request, "id"), *version = yyjson_obj_get(request, "jsonrpc"),
               *method = yyjson_obj_get(request, "method"), *params = yyjson_obj_get(request, "params");
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *response, *error = NULL, *result = NULL;
    char* encoded;
    size_t encoded_length;
    if (!doc) {
        yyjson_doc_free(request_doc);
        close_code(connection, 1011);
        return;
    }
    if (!request_doc)
        error = rpc_error(doc, -32700, "Parse error", NULL);
    else if (!yyjson_is_obj(request) || !yyjson_equals_str(version, "2.0") || !yyjson_is_str(method) ||
             (id && !yyjson_is_str(id) && !yyjson_is_raw(id)))
        error = rpc_error(doc, -32600, "Invalid request", NULL);
    else if (!id) {
        yyjson_doc_free(request_doc);
        yyjson_mut_doc_free(doc);
        return;
    } else if (params && !yyjson_is_obj(params))
        error = rpc_error(doc, -32602, "Invalid parameters", "invalidParams");
    else
        result = dispatch(rpc, doc, method, params, &error);
    response = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, response);
    yyjson_mut_obj_add_str(doc, response, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(
        doc, response, "id",
        id && (yyjson_is_str(id) || yyjson_is_raw(id)) ? yyjson_val_mut_copy(doc, id) : yyjson_mut_null(doc));
    yyjson_mut_obj_add_val(doc, response, error ? "error" : "result", error ? error : result);
    encoded = yyjson_mut_write(doc, 0, &encoded_length);
    if (encoded) {
        mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_TEXT, encoded, encoded_length);
        free(encoded);
    } else
        close_code(connection, 1011);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(request_doc);
}

static int data_ws(struct mg_connection* connection, int bits, char* data, size_t length, void* arg) {
    rcvrpc_t* rpc = arg;
    rpc_client_t* client;
    int opcode = bits & 15, keep = 1;
    pthread_mutex_lock(&rpc->lock);
    client = mg_get_user_connection_data(connection);
    if (!client) {
        pthread_mutex_unlock(&rpc->lock);
        return 0;
    }
    pthread_mutex_lock(&client->lock);
    pthread_mutex_unlock(&rpc->lock);
    /* Per-connection assembly and writes preserve receive order without making
     * another client's progress depend on this client's socket backpressure. */
    if (client->close_sent)
        keep = 0;
    else if ((bits & 0x70) || ((opcode & 8) && (!(bits & 0x80) || length > 125))) {
        close_code(connection, 1002);
        keep = 0;
    } else if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) {
        close_frame(connection, data, length >= 2 ? 2 : 0);
        keep = 0;
    } else if (opcode == MG_WEBSOCKET_OPCODE_BINARY) {
        close_code(connection, 1003);
        keep = 0;
    } else if (opcode == MG_WEBSOCKET_OPCODE_PONG)
        keep = 1;
    else if (opcode == MG_WEBSOCKET_OPCODE_PING) {
        /* With automatic ping disabled, CivetWeb forwards control frames here. */
        mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_PONG, data, length);
    } else if ((opcode != MG_WEBSOCKET_OPCODE_TEXT && opcode != MG_WEBSOCKET_OPCODE_CONTINUATION) ||
               (opcode == MG_WEBSOCKET_OPCODE_CONTINUATION && !client->fragmented) ||
               (opcode == MG_WEBSOCKET_OPCODE_TEXT && client->fragmented)) {
        close_code(connection, 1002);
        keep = 0;
    } else if (length > RPC_MESSAGE_MAX - client->length) {
        close_code(connection, 1009);
        keep = 0;
    } else {
        memcpy(client->message + client->length, data, length);
        client->length += length;
        client->fragmented = !(bits & 128);
        if (!client->fragmented) {
            if (!rcvjson_utf8_valid(client->message, client->length)) {
                close_code(connection, 1007);
                keep = 0;
            } else
                handle_message(rpc, connection, client->message, client->length);
            client->length = 0;
        }
    }
    pthread_mutex_unlock(&client->lock);
    return keep;
}

/* CivetWeb access text includes Referer/User-Agent and diagnostics can include
 * request fragments. Redact every header value and the configured token before
 * forwarding either callback, including during mg_start failure reporting. */
static void safe_log_text(const struct mg_connection* connection, const char* message, char* out, size_t capacity) {
    const struct mg_request_info* request = mg_get_request_info(connection);
    rcvrpc_t* rpc = mg_get_user_data(mg_get_context(connection));
    size_t offset = 0;
    while (*message && offset + 1 < capacity) {
        size_t skip = 0, n = strlen(rpc->options->token);
        int i;
        if (n && !strncmp(message, rpc->options->token, n)) skip = n;
        for (i = 0; request && i < request->num_headers; i++) {
            const char* value = request->http_headers[i].value;
            n = strlen(value);
            if (n > skip && !strncmp(message, value, n)) skip = n;
        }
        if (skip) {
            const char replacement[] = "[redacted]";
            size_t count = sizeof(replacement) - 1;
            if (count >= capacity - offset) break;
            memcpy(out + offset, replacement, count);
            offset += count;
            message += skip;
        } else
            out[offset++] = *message++;
    }
    out[offset] = 0;
}

static int log_message(const struct mg_connection* connection, const char* message) {
    rcvrpc_t* rpc = mg_get_user_data(mg_get_context(connection));
    char safe[8192];
    safe_log_text(connection, message, safe, sizeof(safe));
    trace(NULL, 3, "CivetWeb: %s\n", safe);
    pthread_mutex_lock(&rpc->log_lock);
    /* Keep the original OS error instead of CivetWeb's later summary. */
    if (!rpc->listen_error[0])
        snprintf(rpc->listen_error, sizeof(rpc->listen_error), "%.*s", (int)sizeof(rpc->listen_error) - 1, safe);
    pthread_mutex_unlock(&rpc->log_lock);
    return 1;
}
static int log_access(const struct mg_connection* connection, const char* message) {
    char safe[8192];
    safe_log_text(connection, message, safe, sizeof(safe));
    trace(NULL, 3, "CivetWeb access: %s\n", safe);
    return 1;
}

static void* watch_stdin(void* arg) {
    rcvrpc_t* rpc = arg;
    struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
    for (;;) {
        int stop, status;
        char discarded[1024];
        pthread_mutex_lock(&rpc->lock);
        stop = rpc->stopping;
        pthread_mutex_unlock(&rpc->lock);
        if (stop) break;
        status = poll(&fd, 1, 100);
        if (status < 0 && errno == EINTR) continue;
        if (status < 0 || (status > 0 && (fd.revents & (POLLERR | POLLNVAL)))) {
            rcvcmd_request_shutdown(rpc->cmd);
            break;
        }
        if (status > 0 && (fd.revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(STDIN_FILENO, discarded, sizeof(discarded));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                rcvcmd_request_shutdown(rpc->cmd);
                break;
            }
        }
    }
    return NULL;
}

int rcvrpc_start(rcvrpc_t** out, const rcvrpc_options_t* options, rcvcmd_t* cmd) {
    static const char* protocols[] = {"mrtk-rpc"};
    static struct mg_websocket_subprotocols subprotocols = {1, protocols};
    struct mg_callbacks callbacks = {0};
    struct mg_server_port port;
    struct in6_addr ipv6;
    char listening[256], url[300];
    const char* host = options->bind;
    /* Eight workers serve the expected GUI/browser connections; extras queue.
     * Slow incomplete handshakes hold a worker for at most five seconds.
     * With automatic ping off, the one-second WebSocket timeout bounds mg_stop's
     * idle wait; the data callback answers client pings. No document_root:
     * file GETs stay 404 and file mutations stay 405. */
    const char* configuration[] = {"listening_ports",
                                   listening,
                                   "num_threads",
                                   "8",
                                   "request_timeout_ms",
                                   "5000",
                                   "websocket_timeout_ms",
                                   "1000",
                                   "enable_websocket_ping_pong",
                                   "no",
                                   NULL};
    rcvrpc_t* rpc;
    yyjson_mut_doc* ready;
    yyjson_mut_val* object;
    char* encoded;
    *out = NULL;
    if (options->port < 0) return 1;
    rpc = calloc(1, sizeof(*rpc));
    if (!rpc) return startup_error("RPC allocation failed");
    rpc->options = options;
    rpc->cmd = cmd;
    pthread_mutex_init(&rpc->lock, NULL);
    pthread_mutex_init(&rpc->log_lock, NULL);
    pthread_mutex_init(&rpc->lifecycle_lock, NULL);
    pthread_cond_init(&rpc->lifecycle_cond, NULL);
    snprintf(listening, sizeof(listening), strchr(host, ':') ? "[%s]:%d" : "%s:%d", host, options->port);
    callbacks.begin_request = begin_request;
    callbacks.log_message = log_message;
    callbacks.log_access = log_access;
    mg_init_library(0);
    if (pthread_create(&rpc->lifecycle_thread, NULL, execute_lifecycle, rpc)) {
        startup_error("RPC lifecycle thread could not start");
        rcvrpc_stop(rpc);
        return 0;
    }
    rpc->lifecycle_started = 1;
    rpc->context = mg_start(&callbacks, rpc, configuration);
    if (!rpc->context) {
        fprintf(stderr, "error: RPC listen failed: %s\n",
                rpc->listen_error[0] ? rpc->listen_error : "unable to bind address/port or initialize CivetWeb");
        rcvrpc_stop(rpc);
        return 0;
    }
    mg_set_websocket_handler_with_subprotocols(rpc->context, "/rpc", &subprotocols, connect_ws, ready_ws, data_ws,
                                               close_ws, rpc);
    if (mg_get_server_ports(rpc->context, 1, &port) != 1) {
        startup_error("RPC bound port unavailable");
        rcvrpc_stop(rpc);
        return 0;
    }
    if (options->exit_on_stdin_eof) {
        if (pthread_create(&rpc->watcher, NULL, watch_stdin, rpc)) {
            startup_error("RPC stdin watcher could not start");
            rcvrpc_stop(rpc);
            return 0;
        }
        rpc->watcher_started = 1;
    }
    if (!strcmp(host, "0.0.0.0"))
        host = "127.0.0.1";
    else if (inet_pton(AF_INET6, host, &ipv6) == 1 && IN6_IS_ADDR_UNSPECIFIED(&ipv6))
        host = "::1";
    snprintf(url, sizeof(url), strchr(host, ':') ? "ws://[%s]:%d/rpc" : "ws://%s:%d/rpc", host, port.port);
    ready = yyjson_mut_doc_new(NULL);
    if (!ready) {
        startup_error("RPC ready JSON allocation failed");
        rcvrpc_stop(rpc);
        return 0;
    }
    object = yyjson_mut_obj(ready);
    yyjson_mut_doc_set_root(ready, object);
    yyjson_mut_obj_add_str(ready, object, "url", url);
    encoded = yyjson_mut_write(ready, 0, NULL);
    if (!encoded) {
        yyjson_mut_doc_free(ready);
        startup_error("RPC ready JSON encoding failed");
        rcvrpc_stop(rpc);
        return 0;
    }
    fprintf(stderr, "MRTK-RPC-READY %s\n", encoded);
    fflush(stderr);
    free(encoded);
    yyjson_mut_doc_free(ready);
    *out = rpc;
    return 1;
}

void rcvrpc_stop(rcvrpc_t* rpc) {
    rpc_client_t* client;
    if (!rpc) return;
    pthread_mutex_lock(&rpc->lock);
    rpc->stopping = 1;
    for (client = rpc->clients; client; client = client->next) {
        pthread_mutex_lock(&client->lock);
        close_code(client->connection, 1001);
        pthread_mutex_unlock(&client->lock);
    }
    pthread_mutex_unlock(&rpc->lock);
    pthread_mutex_lock(&rpc->lifecycle_lock);
    rpc->lifecycle_exit = 1;
    pthread_cond_signal(&rpc->lifecycle_cond);
    pthread_mutex_unlock(&rpc->lifecycle_lock);
    if (rpc->lifecycle_started) pthread_join(rpc->lifecycle_thread, NULL);
    if (rpc->watcher_started) pthread_join(rpc->watcher, NULL);
    if (rpc->context) mg_stop(rpc->context);
    mg_exit_library();
    pthread_mutex_destroy(&rpc->lock);
    pthread_mutex_destroy(&rpc->log_lock);
    pthread_mutex_destroy(&rpc->lifecycle_lock);
    pthread_cond_destroy(&rpc->lifecycle_cond);
    free(rpc);
}
