/*------------------------------------------------------------------------------
 * mrtk_l6.c : QZSS L6 frame header parsing and service classification
 *
 * Copyright (C) 2026 H.SHIONO (MRTKLIB Project)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *----------------------------------------------------------------------------*/
#include "mrtklib/mrtk_l6.h"

#include <stddef.h>

/* nominal L6 PRN assignments (see l6_prn_info_t). MADOCA iono regions:
 * PRN 200 Philippines/Indonesia/western Australia, PRN 201 Japan/eastern
 * Australia. */
static const l6_prn_info_t l6_prn_table[] = {
    {193, "QZS1", 0, L6_SVC_CLAS, -1, 0},         {194, "QZS2", 0, L6_SVC_CLAS, -1, 204},
    {195, "QZS4", 0, L6_SVC_CLAS, -1, 205},       {196, "QZS1R", 0, L6_SVC_CLAS, -1, 206},
    {197, "QZS5", 0, L6_SVC_CLAS, -1, 207},       {199, "QZS3", 0, L6_SVC_CLAS, -1, 209},
    {200, "QZS6", 0, L6_SVC_MADOCA_IONO, 0, 210}, {201, "QZS7", 0, L6_SVC_MADOCA_IONO, 1, 211},
    {204, "QZS2", 1, L6_SVC_MADOCA, -1, 194},     {205, "QZS4", 1, L6_SVC_MADOCA, -1, 195},
    {206, "QZS1R", 1, L6_SVC_MADOCA, -1, 196},    {207, "QZS5", 1, L6_SVC_MADOCA, -1, 197},
    {209, "QZS3", 1, L6_SVC_MADOCA, -1, 199},     {210, "QZS6", 1, L6_SVC_MADOCA, -1, 200},
    {211, "QZS7", 1, L6_SVC_MADOCA, -1, 201},
};

extern int l6_is_null(const uint8_t* frame) {
    int i;

    if (frame[5] != 0 || !(frame[6] & 0x80)) {
        return 0;
    }
    /* byte 6 = alert bit + first 7 data bits "0101010"b = 0xAA */
    for (i = 6; i < L6_FRAME_LEN; i++) {
        if (frame[i] != 0xAA) {
            return 0;
        }
    }
    return 1;
}

extern int l6_parse_header(const uint8_t* frame, l6_header_t* h) {
    h->preamble = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16) | ((uint32_t)frame[2] << 8) | frame[3];
    h->prn = frame[4];
    h->msgid = frame[5];
    h->vid = (h->msgid >> 5) & 0x07;
    h->fac = (h->msgid >> 3) & 0x03;
    h->ptn = (h->msgid >> 1) & 0x03;
    h->csid = (h->msgid >> 2) & 0x01;
    h->anme = (h->msgid >> 1) & 0x01;
    h->sf_ind = h->msgid & 0x01;
    h->alert = (frame[6] >> 7) & 0x01;

    if (h->preamble != L6_PREAMBLE) {
        h->svc = L6_SVC_UNKNOWN;
        return 0;
    }
    if (l6_is_null(frame)) {
        h->svc = L6_SVC_NULL;
    } else if (h->vid == L6_VID_CLAS) {
        h->svc = L6_SVC_CLAS;
    } else if (h->vid == L6_VID_MADOCA) {
        h->svc = h->csid ? L6_SVC_MADOCA_IONO : L6_SVC_MADOCA;
    } else if (h->vid == L6_VID_QZNMA) {
        h->svc = L6_SVC_QZNMA;
    } else {
        h->svc = L6_SVC_UNKNOWN;
    }
    return 1;
}

extern const l6_prn_info_t* l6_prn_info(int prn) {
    size_t i;

    for (i = 0; i < sizeof(l6_prn_table) / sizeof(l6_prn_table[0]); i++) {
        if (l6_prn_table[i].prn == prn) {
            return l6_prn_table + i;
        }
    }
    return NULL;
}

extern int l6_service_expected(const l6_header_t* h) {
    const l6_prn_info_t* info = l6_prn_info(h->prn);

    if (h->svc == L6_SVC_NULL) {
        return 1;
    }
    if (!info) {
        return 0;
    }
    if (info->l6e) {
        return h->svc == L6_SVC_MADOCA || h->svc == L6_SVC_QZNMA;
    }
    return h->svc == info->svc;
}

extern const char* l6_service_name(l6_service_t svc) {
    switch (svc) {
        case L6_SVC_NULL:
            return "NULL";
        case L6_SVC_CLAS:
            return "CLAS";
        case L6_SVC_MADOCA:
            return "MADOCA";
        case L6_SVC_MADOCA_IONO:
            return "MADOCA-IONO";
        case L6_SVC_QZNMA:
            return "QZNMA";
        default:
            return "UNKNOWN";
    }
}
