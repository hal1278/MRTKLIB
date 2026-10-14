/*------------------------------------------------------------------------------
 * mrtk_l6.c : QZSS L6 frame header parsing and service classification
 *
 * Copyright (C) 2026 H.SHIONO (MRTKLIB Project)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *----------------------------------------------------------------------------*/
#include "mrtklib/mrtk_l6.h"

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
