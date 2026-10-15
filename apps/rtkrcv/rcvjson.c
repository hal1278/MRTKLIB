/*------------------------------------------------------------------------------
 * rcvjson.c : shared JSON text helpers for receiver adapters
 *
 * Copyright (C) 2026 H.SHIONO (MRTKLIB Project)
 * SPDX-License-Identifier: BSD-2-Clause
 *-----------------------------------------------------------------------------*/
#include "rcvjson.h"

#include <string.h>

/* Consume one scalar or maximal ill-formed subpart. Keeping a valid prefix
 * together prevents a character cut by a fixed-size core buffer from becoming
 * several replacement characters. Restrictions on the second byte also rule
 * out overlong encodings, UTF-16 surrogates and values above U+10FFFF. */
static size_t utf8_part(const unsigned char* text, size_t length, int* valid) {
    unsigned char first = text[0], low = 0x80, high = 0xBF;
    size_t width, i;
    *valid = 0;
    if (first < 0x80) {
        *valid = 1;
        return 1;
    }
    if (first >= 0xC2 && first <= 0xDF) {
        width = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
        width = 3;
        if (first == 0xE0) low = 0xA0;
        if (first == 0xED) high = 0x9F;
    } else if (first >= 0xF0 && first <= 0xF4) {
        width = 4;
        if (first == 0xF0) low = 0x90;
        if (first == 0xF4) high = 0x8F;
    } else {
        return 1;
    }
    for (i = 1; i < width; i++) {
        if (i == length || text[i] < low || text[i] > high) return i;
        low = 0x80;
        high = 0xBF;
    }
    *valid = 1;
    return width;
}

int rcvjson_utf8_valid(const char* text, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        int valid;
        offset += utf8_part((const unsigned char*)text + offset, length - offset, &valid);
        if (!valid) return 0;
    }
    return 1;
}

size_t rcvjson_sanitize_utf8(const char* text, size_t length, char* out, size_t capacity) {
    static const char replacement[] = "\xEF\xBF\xBD";
    size_t offset = 0, required = 0, written = 0;
    int full = 0;
    while (offset < length) {
        int valid;
        size_t part = utf8_part((const unsigned char*)text + offset, length - offset, &valid);
        size_t width = valid ? part : sizeof(replacement) - 1;
        if (!full && capacity > written && width < capacity - written) {
            memcpy(out + written, valid ? text + offset : replacement, width);
            written += width;
        } else {
            full = 1;
        }
        required += width;
        offset += part;
    }
    if (capacity) out[written] = '\0';
    return required;
}

#ifdef RCVJSON_RPC
#include <math.h>

#include "mrtklib/mrtk_const.h"
#include "mrtklib/mrtk_coords.h"
#include "mrtklib/mrtk_time.h"

const char* rcvjson_state_name(rcvcmd_state_t state) {
    static const char* names[] = {"stopped", "starting", "running", "stopping"};
    return names[state];
}

/* Cross-checked against MODOPT and GEOOPT in mrtk_options.c by utest_rcvjson. */
const char* rcvjson_mode_name(int mode) {
    static const char* names[] = {"single",  "dgps",          "kinematic",  "static",    "movingbase",
                                  "fixed",   "ppp-kine",      "ppp-static", "ppp-fixed", "ppp-rtk",
                                  "ssr2osr", "ssr2osr-fixed", "vrs-rtk"};
    return names[mode];
}

const char* rcvjson_geoid_name(int model) {
    static const char* names[] = {"internal", "egm96", "egm08_2.5", "egm08_1", "gsi2000"};
    return names[model];
}

yyjson_mut_val* rcvjson_lifecycle(yyjson_mut_doc* doc, const rcvcmd_lifecycle_t* value) {
    static const char* messages[] = {"", "The correction source is invalid or not available for the positioning mode.",
                                     "Output files already exist.", "The server could not start.",
                                     "Standard input is reserved for process monitoring."};
    yyjson_mut_val* out = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, out, "state", rcvjson_state_name(value->state));
    if ((value->state == RCVCMD_STOPPED || value->state == RCVCMD_STOPPING) && value->stop_reason != RCVCMD_STOP_NONE)
        yyjson_mut_obj_add_str(doc, out, "stopReason", value->stop_reason == RCVCMD_STOP_ERROR ? "error" : "request");
    if (value->last_error != RCVCMD_START_ERROR_NONE &&
        (value->state == RCVCMD_STARTING ||
         (value->state == RCVCMD_STOPPED && value->stop_reason == RCVCMD_STOP_ERROR))) {
        yyjson_mut_val* error = yyjson_mut_obj(doc);
        int overwrite = value->last_error == RCVCMD_START_ERROR_OVERWRITE;
        yyjson_mut_obj_add_str(doc, error, "reason", overwrite ? "outputExists" : "startFailed");
        yyjson_mut_obj_add_str(doc, error, "message", messages[value->last_error]);
        if (overwrite) {
            yyjson_mut_val* paths = yyjson_mut_arr(doc);
            const char* path;
            for (path = value->paths; *path; path += strlen(path) + 1) yyjson_mut_arr_add_strcpy(doc, paths, path);
            yyjson_mut_obj_add_val(doc, error, "paths", paths);
        }
        yyjson_mut_obj_add_val(doc, out, "lastError", error);
    }
    return out;
}

static yyjson_mut_val* position_json(yyjson_mut_doc* doc, const double* xyz) {
    double pos[3];
    yyjson_mut_val *out = yyjson_mut_obj(doc), *ecef = yyjson_mut_obj(doc), *llh = yyjson_mut_obj(doc);
    ecef2pos(xyz, pos);
    yyjson_mut_obj_add_real(doc, ecef, "x", xyz[0]);
    yyjson_mut_obj_add_real(doc, ecef, "y", xyz[1]);
    yyjson_mut_obj_add_real(doc, ecef, "z", xyz[2]);
    yyjson_mut_obj_add_real(doc, llh, "lat", pos[0] * R2D);
    yyjson_mut_obj_add_real(doc, llh, "lon", pos[1] * R2D);
    yyjson_mut_obj_add_real(doc, llh, "height", pos[2]);
    yyjson_mut_obj_add_val(doc, out, "ecef", ecef);
    yyjson_mut_obj_add_val(doc, out, "llh", llh);
    return out;
}

/* Only the values written below count: an unsent member (velocity, clock bias)
 * must not turn a usable solution into none. */
static int solution_finite(const rcvevent_solution_t* value) {
    const sol_t* sol = &value->sol;
    int i;
    if (!isfinite(sol->time.sec) || !isfinite(value->leap) || !isfinite(sol->age) || !isfinite(sol->ratio)) return 0;
    for (i = 0; i < 3; i++)
        if (!isfinite(sol->rr[i])) return 0;
    for (i = 0; i < 6; i++)
        if (!isfinite(sol->qr[i])) return 0;
    if (value->has_base)
        for (i = 0; i < 3; i++)
            if (!isfinite(value->rb[i])) return 0;
    return !value->has_separation || isfinite(value->separation);
}

yyjson_mut_val* rcvjson_solution(yyjson_mut_doc* doc, const rcvevent_solution_t* value) {
    static const char* names[] = {"none", "fix", "float", "sbas", "dgps", "single", "ppp", "dr"};
    static const char* ecef_keys[] = {"xx", "yy", "zz", "xy", "yz", "zx"};
    static const char* enu_keys[] = {"ee", "nn", "uu", "en", "nu", "ue"};
    static const int indices[] = {0, 4, 8, 1, 5, 2};
    const sol_t* sol = &value->sol;
    double pos[3], P[9], Q[9], tow;
    gtime_t epoch = sol->time;
    int week, i, none = sol->stat == SOLQ_NONE || !solution_finite(value);
    yyjson_mut_val *out = yyjson_mut_obj(doc), *time = yyjson_mut_obj(doc);
    yyjson_mut_val *covariance, *ecef, *enu, *geoid;
    /* Invalid time itself has no JSON representation; keep the integral epoch. */
    if (!isfinite(epoch.sec)) epoch.sec = 0.0;
    tow = round(time2gpst(epoch, &week) * 1e9) / 1e9;
    if (tow >= 604800.0) {
        week++;
        tow = 0.0;
    }
    yyjson_mut_obj_add_int(doc, time, "week", week);
    yyjson_mut_obj_add_real(doc, time, "tow", tow);
    yyjson_mut_obj_add_val(doc, out, "time", time);
    yyjson_mut_obj_add_int(doc, out, "leapSeconds", isfinite(value->leap) ? (int)lround(value->leap) : 0);
    /* Server-produced solutions are ECEF (type 0); type 1 is readsol's ENU input. */
    if (!none) ecef2pos(sol->rr, pos);
    P[0] = sol->qr[0];
    P[4] = sol->qr[1];
    P[8] = sol->qr[2];
    P[1] = P[3] = sol->qr[3];
    P[5] = P[7] = sol->qr[4];
    P[2] = P[6] = sol->qr[5];
    if (!none) {
        covenu(pos, P, Q);
        for (i = 0; i < 3; i++)
            if (!isfinite(pos[i])) none = 1;
        for (i = 0; i < 9; i++)
            if (!isfinite(Q[i])) none = 1;
        if (value->has_base) {
            double base[3];
            ecef2pos(value->rb, base);
            for (i = 0; i < 3; i++)
                if (!isfinite(base[i])) none = 1;
        }
    }
    yyjson_mut_obj_add_str(doc, out, "status", none ? "none" : names[sol->stat]);
    if (none) return out;
    yyjson_mut_obj_add_val(doc, out, "position", position_json(doc, sol->rr));
    covariance = yyjson_mut_obj(doc);
    ecef = yyjson_mut_obj(doc);
    enu = yyjson_mut_obj(doc);
    for (i = 0; i < 6; i++) {
        yyjson_mut_obj_add_val(doc, ecef, ecef_keys[i], yyjson_mut_float(doc, sol->qr[i]));
        yyjson_mut_obj_add_real(doc, enu, enu_keys[i], Q[indices[i]]);
    }
    yyjson_mut_obj_add_val(doc, covariance, "ecef", ecef);
    yyjson_mut_obj_add_val(doc, covariance, "enu", enu);
    yyjson_mut_obj_add_val(doc, out, "covariance", covariance);
    geoid = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, geoid, "model", rcvjson_geoid_name(value->geoid_model));
    if (value->has_separation) yyjson_mut_obj_add_real(doc, geoid, "separation", value->separation);
    yyjson_mut_obj_add_val(doc, out, "geoid", geoid);
    yyjson_mut_obj_add_uint(doc, out, "validSatellites", sol->ns);
    yyjson_mut_obj_add_val(doc, out, "age", yyjson_mut_float(doc, sol->age));
    yyjson_mut_obj_add_val(doc, out, "ratio", yyjson_mut_float(doc, sol->ratio));
    if (value->has_base) yyjson_mut_obj_add_val(doc, out, "base", position_json(doc, value->rb));
    return out;
}

yyjson_mut_val* rcvjson_snapshot(yyjson_mut_doc* doc, const rcvcmd_snapshot_t* value) {
    yyjson_mut_val *out = yyjson_mut_obj(doc), *status = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, out, "seq", value->seq);
    yyjson_mut_obj_add_val(doc, status, "lifecycle", rcvjson_lifecycle(doc, &value->lifecycle));
    if (value->lifecycle.state == RCVCMD_RUNNING) {
        yyjson_mut_obj_add_str(doc, status, "mode", rcvjson_mode_name(value->mode));
        if (value->has_solution)
            yyjson_mut_obj_add_val(doc, status, "solution", rcvjson_solution(doc, &value->solution.data.solution));
    }
    yyjson_mut_obj_add_val(doc, out, "status", status);
    return out;
}

yyjson_mut_val* rcvjson_event(yyjson_mut_doc* doc, const rcvevent_t* value) {
    yyjson_mut_val *out = yyjson_mut_obj(doc), *data;
    const char* kind;
    if (value->kind == RCVEVENT_STATUS) {
        rcvcmd_lifecycle_t lifecycle;
        lifecycle.state = value->data.status.state;
        lifecycle.stop_reason = value->data.status.stop_reason;
        lifecycle.last_error = value->data.status.last_error;
        memcpy(lifecycle.paths, value->data.status.paths, sizeof(lifecycle.paths));
        kind = "status";
        data = rcvjson_lifecycle(doc, &lifecycle);
    } else if (value->kind == RCVEVENT_SOLUTION) {
        kind = "solution";
        data = rcvjson_solution(doc, &value->data.solution);
    } else {
        char text[RCVEVENT_TEXT * 3];
        kind = "logMessage";
        data = yyjson_mut_obj(doc);
        rcvjson_sanitize_utf8(value->data.log.text, strlen(value->data.log.text), text, sizeof(text));
        yyjson_mut_obj_add_int(doc, data, "level", value->data.log.level);
        yyjson_mut_obj_add_strcpy(doc, data, "text", text);
    }
    yyjson_mut_obj_add_uint(doc, out, "seq", value->seq);
    yyjson_mut_obj_add_str(doc, out, "kind", kind);
    yyjson_mut_obj_add_val(doc, out, "data", data);
    return out;
}
#endif
