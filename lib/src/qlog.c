// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2019, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <warpcore/warpcore.h>

// IWYU pragma: no_include "../deps/libev/ev.h"

#include "bitset.h"
#include "event.h" // IWYU pragma: keep
#include "frame.h"
#include "marshall.h"
#include "pkt.h"
#include "qlog.h"
#include "quic.h"
#include "recovery.h"
#include "stream.h"


static bool prev_event = false;


static inline const char * __attribute__((const, nonnull))
qlog_pkt_type_str(const uint8_t flags, const void * const vers)
{
    if (is_lh(flags)) {
        if (((const uint8_t * const)vers)[0] == 0 &&
            ((const uint8_t * const)vers)[1] == 0 &&
            ((const uint8_t * const)vers)[2] == 0 &&
            ((const uint8_t * const)vers)[3] == 0)
            return "VERSION_NEGOTIATION";
        switch (pkt_type(flags)) {
        case LH_INIT:
            return "INITIAL";
        case LH_RTRY:
            return "RETRY";
        case LH_HSHK:
            return "HANDSHAKE";
        case LH_0RTT:
            return "ZERORTT";
        }
    } else if (pkt_type(flags) == SH)
        return "ONERTT";
    return "UNKOWN";
}


uint64_t to_usec(const ev_tstamp t)
{
    return (uint64_t)llround(t * USECS_PER_SEC);
}


void qlog_transport(const char * const evt,
                    const char * const trg,
                    struct w_iov * const v,
                    const struct pkt_meta * const m)
{
    fprintf(qlog,
            "%s[%" PRIu64 ",\"TRANSPORT\",\"%s\",\"%s\",{\"packet_type\":\"%"
            "s\",\"header\":{\"packet_size\":%u",
            likely(prev_event) ? "," : "", to_usec(m->t - qlog_ref_t), evt, trg,
            qlog_pkt_type_str(m->hdr.flags, &m->hdr.vers), m->udp_len);
    if (is_lh(m->hdr.flags) == false || (m->hdr.vers && m->hdr.type != LH_RTRY))
        fprintf(qlog, ",\"packet_number\":%" PRIu64, m->hdr.nr);
    fputs("}", qlog);

    static const struct frames qlog_frm =
        bitset_t_initializer(1 << FRM_ACK | 1 << FRM_STR);
    if (bit_overlap(FRM_MAX, &m->frms, &qlog_frm) == false)
        goto done;

    fputs(",\"frames\":[", qlog);
    int prev_frame = 0;
    if (has_frm(m->frms, FRM_STR)) {
        prev_frame = fprintf(qlog,
                             "%s{\"frame_type\": \"STREAM\",\"id\": %" PRId64
                             ",\"length\": %u,\"offset\": %" PRIu64,
                             prev_frame ? "," : "", m->strm->id,
                             m->strm_data_len, m->strm_off);
        if (m->is_fin)
            fputs(",\"fin\":true", qlog);
        fputs("}", qlog);
    }

    if (has_frm(m->frms, FRM_ACK)) {
        adj_iov_to_start(v, m);
        const uint8_t * pos = v->buf + m->ack_frm_pos;
        const uint8_t * const end = v->buf + v->len;

        uint64_t lg_ack = 0;
        decv(&lg_ack, &pos, end);
        uint64_t ack_delay = 0;
        decv(&ack_delay, &pos, end);
        uint64_t ack_rng_cnt = 0;
        decv(&ack_rng_cnt, &pos, end);

        // prev_frame =
        fprintf(qlog,
                "%s{\"frame_type\": \"ACK\",\"ack_delay\": %" PRId64
                ",\"acked_ranges\":[",
                prev_frame ? "," : "", ack_delay);

        // this is a similar loop as in dec_ack_frame() - keep changes in sync
        for (uint64_t n = ack_rng_cnt + 1; n > 0; n--) {
            uint64_t ack_rng = 0;
            decv(&ack_rng, &pos, end);
            fprintf(qlog, "%s[%" PRIu64 ",%" PRIu64 "]", n > 1 ? "," : "",
                    lg_ack - ack_rng, lg_ack);
            if (n > 1) {
                uint64_t gap = 0;
                decv(&gap, &pos, end);
                lg_ack -= ack_rng + gap + 2;
            }
        }

        adj_iov_to_data(v, m);
        fputs("]}", qlog);
    }
    fputs("]", qlog);

done:
    fputs("}]", qlog);

    prev_event = true;
}


void qlog_recovery(const char * const evt,
                   const char * const trg,
                   const struct q_conn * const c)
{
    const ev_tstamp t = ev_now();
    fprintf(qlog, "%s[%" PRIu64 ",\"RECOVERY\",\"%s\",\"%s\",{",
            likely(prev_event) ? "," : "", to_usec(t - qlog_ref_t), evt, trg);
    int prev_metric = 0;
    if (c->rec.cur.in_flight != c->rec.prev.in_flight)
        prev_metric = fprintf(qlog, "%s\"bytes_in_flight\":%" PRIu64,
                              prev_metric ? "," : "", c->rec.cur.in_flight);
    if (c->rec.cur.cwnd != c->rec.prev.cwnd)
        prev_metric = fprintf(qlog, "%s\"cwnd\":%" PRIu64,
                              prev_metric ? "," : "", c->rec.cur.cwnd);
    if (c->rec.cur.ssthresh != c->rec.prev.ssthresh &&
        c->rec.cur.ssthresh != UINT64_MAX)
        prev_metric = fprintf(qlog, "%s\"ssthresh\":%" PRIu64,
                              prev_metric ? "," : "", c->rec.cur.ssthresh);
    if (to_usec(c->rec.cur.srtt) != to_usec(c->rec.prev.srtt))
        prev_metric = fprintf(qlog, "%s\"smoothed_rtt\":%" PRIu64,
                              prev_metric ? "," : "", to_usec(c->rec.cur.srtt));
    if (c->rec.cur.min_rtt != HUGE_VAL &&
        to_usec(c->rec.cur.min_rtt) != to_usec(c->rec.prev.min_rtt))
        prev_metric =
            fprintf(qlog, "%s\"min_rtt\":%" PRIu64, prev_metric ? "," : "",
                    to_usec(c->rec.cur.min_rtt));
    if (to_usec(c->rec.cur.latest_rtt) != to_usec(c->rec.prev.latest_rtt))
        prev_metric =
            fprintf(qlog, "%s\"latest_rtt\":%" PRIu64, prev_metric ? "," : "",
                    to_usec(c->rec.cur.latest_rtt));
    if (to_usec(c->rec.cur.rttvar) != to_usec(c->rec.prev.rttvar))
        // prev_metric =
        fprintf(qlog, "%s\"rtt_variance\":%" PRIu64, prev_metric ? "," : "",
                to_usec(c->rec.cur.rttvar));
    fputs("}]", qlog);

    prev_event = true;
}