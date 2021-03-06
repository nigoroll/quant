// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2020, NetApp, Inc.
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#ifndef NO_ERR_REASONS
#include <stdarg.h>
#endif

#ifndef NO_MIGRATION
#include <sys/socket.h>
#endif

#ifdef __FreeBSD__
#include <netinet/in.h>
#endif

#if !defined(PARTICLE) && !defined(RIOT_VERSION)
#include <netinet/ip.h>
#endif

#include <picotls.h>
#include <quant/quant.h>
#include <timeout.h>

#include "conn.h"
#include "diet.h"
#include "frame.h"
#include "loop.h"
#include "marshall.h"
#include "pkt.h"
#include "pn.h"
#include "qlog.h"
#include "quic.h"
#include "recovery.h"
#include "stream.h"
#include "tls.h"

#ifndef NO_SERVER
#include "kvec.h"
#endif

#ifndef NO_QLOG
#include "bitset.h"
#endif


#undef CONN_STATE
#define CONN_STATE(k, v) [v] = #k

const char * const conn_state_str[] = {CONN_STATES};

struct q_conn_sl c_ready = sl_head_initializer(c_ready);
struct q_conn_sl c_zcid = sl_head_initializer(c_zcid);

#ifndef NO_SERVER
struct q_conn_sl c_embr = sl_head_initializer(c_embr);
#endif


#ifndef NO_SRT_MATCHING
khash_t(conns_by_srt) conns_by_srt = {0};
#endif


static inline __attribute__((const)) bool is_vneg_vers(const uint32_t vers)
{
    return (vers & 0x0f0f0f0f) == 0x0a0a0a0a;
}


static inline __attribute__((const)) bool is_draft_vers(const uint32_t vers)
{
    return (vers & 0xff000000) == 0xff000000;
}


#ifndef NO_MIGRATION
khash_t(conns_by_id) conns_by_id = {0};

SPLAY_GENERATE(cids_by_seq, cid, node_seq, cids_by_seq_cmp)
#endif


static bool __attribute__((const)) vers_supported(const uint32_t v)
{
    if (is_vneg_vers(v))
        return false;

    for (uint8_t i = 0; i < ok_vers_len; i++)
        if (v == ok_vers[i])
            return true;

    // we're out of matching candidates
    warn(INF, "no vers in common");
    return false;
}


static uint32_t __attribute__((nonnull))
clnt_vneg(const uint8_t * const pos, const uint8_t * const end)
{
    for (uint8_t i = 0; i < ok_vers_len; i++) {
        if (is_vneg_vers(ok_vers[i]))
            continue;

        const uint8_t * p = pos;
        while (p + sizeof(ok_vers[0]) <= end) {
            uint32_t vers = 0;
            dec4(&vers, &p, end);
            if (is_vneg_vers(vers))
                continue;
#ifdef DEBUG_EXTRA
            warn(DBG,
                 "serv prio %ld = 0x%0" PRIx32 "; our prio %u = 0x%0" PRIx32,
                 (unsigned long)(p - pos) / sizeof(vers), vers, i, ok_vers[i]);
#endif
            if (ok_vers[i] == vers)
                return vers;
        }
    }

    // we're out of matching candidates
    warn(INF, "no vers in common with serv");
    return 0;
}


#ifndef NO_OOO_0RTT
struct ooo_0rtt_by_cid ooo_0rtt_by_cid = splay_initializer(&ooo_0rtt_by_cid);

SPLAY_GENERATE(ooo_0rtt_by_cid, ooo_0rtt, node, ooo_0rtt_by_cid_cmp)
#endif


static inline epoch_t __attribute__((nonnull))
epoch_in(const struct q_conn * const c)
{
    const size_t epoch = ptls_get_read_epoch(c->tls.t);
    ensure(epoch <= ep_data, "unhandled epoch %lu", (unsigned long)epoch);
    return (epoch_t)epoch;
}


#ifndef NO_SERVER
static struct w_sock * __attribute__((nonnull))
get_local_sock_by_ipnp(struct per_engine_data * const ped,
                       const struct w_sockaddr * const local)
{
    for (size_t i = 0; i < kv_size(ped->serv_socks); i++) {
        struct w_sock * const ws = kv_A(ped->serv_socks, i);
        if (w_sockaddr_cmp(local, &ws->ws_loc))
            return ws;
    }
    return 0;
}
#endif


#ifndef NO_SRT_MATCHING
struct q_conn * get_conn_by_srt(uint8_t * const srt)
{
    const khiter_t k = kh_get(conns_by_srt, &conns_by_srt, srt);
    if (unlikely(k == kh_end(&conns_by_srt)))
        return 0;
    return kh_val(&conns_by_srt, k);
}
#endif


#ifndef NO_MIGRATION
static struct q_conn * __attribute__((nonnull))
get_conn_by_cid(struct cid * const scid)
{
    const khiter_t k = kh_get(conns_by_id, &conns_by_id, scid);
    if (unlikely(k == kh_end(&conns_by_id)))
        return 0;
    return kh_val(&conns_by_id, k);
}


static inline void __attribute__((nonnull))
cids_by_id_ins(khash_t(cids_by_id) * const cbi, struct cid * const id)
{
    int ret;
    const khiter_t k = kh_put(cids_by_id, cbi, id, &ret);
    ensure(ret >= 1, "inserted returned %d", ret);
    kh_val(cbi, k) = id;
}


static inline void __attribute__((nonnull))
cids_by_id_del(khash_t(cids_by_id) * const cbi, struct cid * const id)
{
    const khiter_t k = kh_get(cids_by_id, cbi, id);
    ensure(k != kh_end(cbi), "found");
    kh_del(cids_by_id, cbi, k);
}


static struct cid * __attribute__((nonnull))
get_cid_by_id(const khash_t(cids_by_id) * const cbi, struct cid * const id)
{
    const khiter_t k = kh_get(cids_by_id, cbi, id);
    if (unlikely(k == kh_end(cbi)))
        return 0;
    return kh_val(cbi, k);
}


void use_next_dcid(struct q_conn * const c)
{
    struct cid * const dcid =
        splay_next(cids_by_seq, &c->dcids_by_seq, c->dcid);
    ensure(dcid, "can't switch from dcid %" PRIu, c->dcid->seq);

    mk_cid_str(NTE, dcid, dcid_str_new);
    mk_cid_str(NTE, c->dcid, dcid_str_prev);
    warn(NTE, "migration to dcid %s for %s conn (was %s)", dcid_str_new,
         conn_type(c), dcid_str_prev);

    if (c->spin_enabled)
        c->spin = 0; // need to reset spin value
    c->tx_retire_cid = c->dcid->retired = true;
    c->dcid = dcid;
}
#endif


#ifndef NDEBUG
static void __attribute__((nonnull)) log_sent_pkts(struct q_conn * const c)
{
    for (pn_t t = pn_init; t <= pn_data; t++) {
        struct pn_space * const pn = &c->pns[t];
        if (pn->abandoned)
            continue;

        struct diet unacked = diet_initializer(unacked);
        struct pkt_meta * m;
        kh_foreach_value(&pn->sent_pkts, m,
                         diet_insert(&unacked, m->hdr.nr, 0));

        int pos = 0;
        unpoison_scratch(ped(c->w)->scratch, ped(c->w)->scratch_len);
        const uint32_t tmp_len = ped(c->w)->scratch_len;
        uint8_t * const tmp = ped(c->w)->scratch;
        struct ival * i = 0;
        diet_foreach (i, diet, &unacked) {
            if ((size_t)pos >= tmp_len) {
                tmp[tmp_len - 2] = tmp[tmp_len - 3] = tmp[tmp_len - 4] = '.';
                tmp[tmp_len - 1] = 0;
                break;
            }

            if (i->lo == i->hi)
                pos += snprintf((char *)&tmp[pos], tmp_len - (size_t)pos,
                                FMT_PNR_OUT "%s", i->lo,
                                splay_next(diet, &unacked, i) ? ", " : "");
            else
                pos += snprintf((char *)&tmp[pos], tmp_len - (size_t)pos,
                                FMT_PNR_OUT ".." FMT_PNR_OUT "%s", i->lo, i->hi,
                                splay_next(diet, &unacked, i) ? ", " : "");
        }
        diet_free(&unacked);

        if (pos)
            warn(INF, "%s conn %s, %s unacked: %s", conn_type(c),
                 cid_str(c->scid), pn_type_str(t), tmp);
        poison_scratch(ped(c->w)->scratch, ped(c->w)->scratch_len);
    }
}
#else
#define log_sent_pkts(...)
#endif


static void __attribute__((nonnull))
rtx_pkt(struct w_iov * const v, struct pkt_meta * const m)
{
    struct q_conn * const c = m->pn->c;
#ifndef NO_QINFO
    c->i.pkts_out_rtx++;
#endif

    if (m->lost)
        // we don't need to do the steps below if the pkt is lost already
        return;

    // on RTX, remember orig pkt meta data
    const uint16_t data_start = m->strm_data_pos;
    struct pkt_meta * m_orig;
    struct w_iov * const v_orig =
        alloc_iov(c->w, q_conn_af(c), 0, data_start, &m_orig);
    pm_cpy(m_orig, m, true);
    memcpy(v_orig->buf - data_start, v->buf - data_start, data_start);
    m_orig->has_rtx = true;
    sl_insert_head(&m->rtx, m_orig, rtx_next);
    sl_insert_head(&m_orig->rtx, m, rtx_next);
    pm_by_nr_del(&m->pn->sent_pkts, m);
    // we reinsert m with its new pkt nr in on_pkt_sent()
    pm_by_nr_ins(&m_orig->pn->sent_pkts, m_orig);
}


static void do_w_tx(struct w_sock * const ws, struct w_iov_sq * const q)
{
#ifndef FUZZING
    w_tx(ws, q);
    do
        w_nic_tx(ws->w);
    while (w_tx_pending(q));
#endif
}


static void __attribute__((nonnull)) tx_vneg_resp(struct w_sock * const ws,
                                                  const struct w_iov * const v,
                                                  struct pkt_meta * const m)
{
    struct pkt_meta * mx;
    struct w_iov * const xv = alloc_iov(ws->w, ws->ws_af, 0, 0, &mx);

    struct w_iov_sq q = w_iov_sq_initializer(q);
    sq_insert_head(&q, xv, next);

    warn(INF, "sending vneg serv response");
    mx->txed = 1;
    mx->hdr.flags = HEAD_FORM | (uint8_t)w_rand_uniform32(UINT8_MAX);

    uint8_t * pos = xv->buf;
    const uint8_t * end = xv->buf + xv->len;
    enc1(&pos, end, mx->hdr.flags);
    enc4(&pos, end, mx->hdr.vers);
    enc_lh_cids(&pos, end, mx, &m->hdr.scid, &m->hdr.dcid);

    for (uint8_t j = 0; j < ok_vers_len; j++)
        if (!is_vneg_vers(ok_vers[j]))
            enc4(&pos, end, ok_vers[j]);

    mx->udp_len = xv->len = (uint16_t)(pos - xv->buf);
    xv->saddr = v->saddr;
    xv->flags = v->flags;
    log_pkt("TX", xv, &xv->saddr, 0, 0, 0);
    // qlog_transport(pkt_tx, "default", xv, mx);
    do_w_tx(ws, &q);
    q_free(&q);
}


static void __attribute__((nonnull)) do_tx_txq(struct q_conn * const c,
                                               struct w_iov_sq * const q,
                                               struct w_sock * const ws)
{
#ifndef NO_QINFO
    c->i.pkts_out += w_iov_sq_cnt(q);
#endif

    const uint16_t pmtu =
        MIN(w_max_udp_payload(ws), (uint16_t)c->tp_peer.max_pkt);

    if (w_iov_sq_cnt(q) > 1 && unlikely(is_lh(*sq_first(q)->buf))) {
        const bool do_pmtud =
            c->rec.max_pkt_size == MIN_INI_LEN && pmtu > MIN_INI_LEN;
        c->pmtud_pkt = coalesce(
            q, unlikely(do_pmtud) ? pmtu : c->rec.max_pkt_size, do_pmtud);
    }
    do_w_tx(ws, q);

    // txq was allocated from warpcore, no metadata to be freed
    w_free(q);
}


static void __attribute__((nonnull)) do_tx(struct q_conn * const c)
{
    // do it here instead of in on_pkt_sent()
    set_ld_timer(c);
    log_cc(c);

    c->needs_tx = false;
    c->tx_limit = 0;

    if (likely(sq_empty(&c->txq) == false))
        do_tx_txq(c, &c->txq, c->sock);
#ifndef NO_MIGRATION
    if (likely(sq_empty(&c->migr_txq) == false))
        do_tx_txq(c, &c->migr_txq, c->migr_sock);
#endif
    log_sent_pkts(c);
}


static void __attribute__((nonnull))
restart_key_flip_alarm(struct q_conn * const c)
{
    const timeout_t t = c->tls_key_update_frequency * NS_PER_S;

#ifdef DEBUG_TIMERS
    warn(DBG, "next key flip alarm in %.3f sec", (double)t / NS_PER_S);
#endif

    timeouts_add(ped(c->w)->wheel, &c->key_flip_alarm, t);
}


void do_conn_fc(struct q_conn * const c, const uint16_t len)
{
    if (unlikely(c->state == conn_clsg || c->state == conn_drng))
        return;

    if (len &&
        c->out_data_str + len + c->rec.max_pkt_size > c->tp_peer.max_data)
        c->blocked = true;

    // check if we need to do connection-level flow control
    if (c->in_data_str * 4 > c->tp_mine.max_data) {
        c->tx_max_data = true;
        c->tp_mine.max_data *= 4;
    }
}


static void __attribute__((nonnull)) do_conn_mgmt(struct q_conn * const c)
{
    if (c->state == conn_clsg || c->state == conn_drng)
        return;

    // do we need to make more stream IDs available?
    if (likely(hshk_done(c))) {
        if (!is_clnt(c) && unlikely(c->tx_new_tok && c->tok_len == 0 &&
                                    c->pns[ep_init].abandoned))
            // TODO: find a better way to send NEW_TOKEN
            make_rtry_tok(c);

        do_stream_id_fc(c, c->cnt_uni, false, true);
        do_stream_id_fc(c, c->cnt_bidi, true, true);
    }

#ifndef NO_MIGRATION
    if (likely(c->tp_peer.disable_active_migration == false) &&
        unlikely(c->do_migration == true) && c->scid) {
        if (splay_count(&c->scids_by_seq) >= 2) {
            // the peer has a CID for us that they can switch to
            const struct cid * const dcid =
                splay_max(cids_by_seq, &c->dcids_by_seq);
            // if higher-numbered destination CIDs are available, switch to next
            if (dcid && dcid->seq > c->dcid->seq) {
                use_next_dcid(c);
                // don't migrate again for a while
                c->do_migration = false;
                restart_key_flip_alarm(c);
            }
        }
        // send new CIDs if the peer doesn't have sufficient remaining
        c->tx_ncid = needs_more_ncids(c);
    }
#endif
}


static bool __attribute__((nonnull)) tx_stream(struct q_stream * const s)
{
    struct q_conn * const c = s->c;

    const bool has_data =
        (sq_empty(&s->out) == false && out_fully_acked(s) == false);

#ifdef DEBUG_STREAMS
    warn(ERR,
         "%s strm id=" FMT_SID ", cnt=%" PRIu
         ", has_data=%u, needs_ctrl=%u, blocked=%u, lost_cnt=%" PRIu
         ", fully_acked=%u, "
         "limit=%" PRIu32,
         conn_type(c), s->id, w_iov_sq_cnt(&s->out), has_data, needs_ctrl(s),
         s->blocked, s->lost_cnt, out_fully_acked(s), c->tx_limit);
#endif

    // check if we should skip TX on this stream
    if (has_data == false || (s->blocked && s->lost_cnt == 0) ||
        // unless for 0-RTT, is this a regular stream during conn open?
        unlikely(c->try_0rtt == false && s->id >= 0 && c->state < conn_estb)) {
#ifdef DEBUG_STREAMS
        warn(ERR, "skip " FMT_SID, s->id);
#endif
        return true;
    }

#ifdef DEBUG_STREAMS
    warn(INF, "TX on %s conn %s strm " FMT_SID " w/%" PRIu " pkt%s in queue ",
         conn_type(c), cid_str(c->scid), s->id, w_iov_sq_cnt(&s->out),
         plural(w_iov_sq_cnt(&s->out)));
#endif

    uint32_t encoded = 0;
    struct w_iov * v = s->out_una;
    sq_foreach_from (v, &s->out, next) {
        struct pkt_meta * const m = &meta(v);
        if (unlikely(has_wnd(c, v->len) == false && c->tx_limit == 0)) {
            c->no_wnd = true;
            break;
        }

        if (unlikely(m->acked)) {
#ifdef DEBUG_EXTRA
            warn(INF, "skip ACK'ed pkt " FMT_PNR_OUT, m->hdr.nr);
#endif
            continue;
        }

        if (c->tx_limit == 0 && m->txed && m->lost == false) {
#ifdef DEBUG_EXTRA
            warn(INF, "skip non-lost TX'ed pkt " FMT_PNR_OUT, m->hdr.nr);
#endif
            continue;
        }

        if (likely(hshk_done(c) && s->id >= 0)) {
            do_stream_fc(s, v->len);
            do_conn_fc(c, v->len);
        }

        const bool do_rtx = m->lost || (c->tx_limit && m->txed);
        if (unlikely(do_rtx))
            rtx_pkt(v, m);

        if (unlikely(enc_pkt(s, do_rtx, true, c->tx_limit > 0, false, v, m) ==
                     false))
            continue;
        encoded++;

        if (unlikely(s->blocked || c->blocked))
            break;

        if (unlikely(c->tx_limit && encoded == c->tx_limit)) {
#ifdef DEBUG_STREAMS
            warn(INF, "tx limit %" PRIu32 " reached", c->tx_limit);
#endif
            break;
        }

        if (unlikely(c->state > conn_estb))
            break;
    }

    return (c->tx_limit == 0 || encoded < c->tx_limit) && c->no_wnd == false;
}


static bool __attribute__((nonnull))
tx_ack(struct q_conn * const c, const epoch_t e, const bool tx_ack_eliciting)
{
    do_conn_mgmt(c);
    if (unlikely(c->cstrms[e] == 0))
        return false;

    struct pkt_meta * m;
    struct w_iov * const v = alloc_iov(c->w, q_conn_af(c), 0, 0, &m);
    return enc_pkt(c->cstrms[e], false, false, tx_ack_eliciting, false, v, m);
}


void tx(struct q_conn * const c)
{
#ifdef DEBUG_TIMERS
    warn(DBG, "tx timeout on %s conn %s, lim %u", conn_type(c),
         cid_str(c->scid), c->tx_limit);
#endif

    if (unlikely(c->state == conn_drng))
        return;

    if (unlikely(c->state == conn_qlse)) {
        enter_closing(c);
        tx_ack(c, epoch_in(c), false);
        goto done;
    }

#ifndef NO_SERVER
    if (unlikely(c->tx_rtry)) {
        tx_ack(c, ep_init, false);
        goto done;
    }
#endif

    if (unlikely(c->state == conn_opng) && is_clnt(c) && c->try_0rtt &&
        c->pns[pn_data].data.out_0rtt.aead == 0) {
        // if we have no 0-rtt keys here, the ticket didn't have any - disable
        warn(NTE, "TLS ticket w/o 0-RTT keys, disabling 0-RTT");
        c->try_0rtt = false;
        // TODO: we should also reset tp_peer here
    }

    if (unlikely(c->blocked))
        goto done;

    do_conn_mgmt(c);

    if (likely(c->state != conn_clsg)) {
        for (epoch_t e = ep_init; e <= ep_data; e++) {
            if (c->cstrms[e] == 0)
                continue;
            if (tx_stream(c->cstrms[e]) == false)
                goto done;
        }

        struct q_stream * s;
        kh_foreach_value(&c->strms_by_id, s, {
            if (tx_stream(s) == false)
                break;
        });
    }

done:;
    // make sure we sent enough packets when we have a TX limit
    uint_t sent = w_iov_sq_cnt(&c->txq)
#ifndef NO_MIGRATION
                  + w_iov_sq_cnt(&c->migr_txq)
#endif
        ;
    while ((unlikely(c->tx_limit) && sent < c->tx_limit) ||
           (c->needs_tx && sent == 0)) {
        if (likely(tx_ack(c, epoch_in(c), c->tx_limit && sent < c->tx_limit)))
            sent++;
        else {
            warn(WRN, "no ACK sent");
            break;
        }
    }
    if (likely(sent))
        do_tx(c);
}


#ifndef NO_SRT_MATCHING
void conns_by_srt_ins(struct q_conn * const c, uint8_t * const srt)
{
    int ret;
    const khiter_t k = kh_put(conns_by_srt, &conns_by_srt, srt, &ret);
    if (unlikely(ret == 0)) {
        if (kh_val(&conns_by_srt, k) != c)
            die("srt already in use by different conn ");
        else {
            warn(WRN, "srt %s already used for conn", srt_str(srt));
            return;
        }
    }
    kh_val(&conns_by_srt, k) = c;
}


static inline void __attribute__((nonnull))
conns_by_srt_del(uint8_t * const srt)
{
    const khiter_t k = kh_get(conns_by_srt, &conns_by_srt, srt);
    if (likely(k != kh_end(&conns_by_srt)))
        // if peer is reusing SRTs w/different CIDs, it may already be deleted
        kh_del(conns_by_srt, &conns_by_srt, k);
}
#endif


#ifndef NO_MIGRATION
static inline void __attribute__((nonnull))
conns_by_id_ins(struct q_conn * const c, struct cid * const id)
{
    int ret;
    const khiter_t k = kh_put(conns_by_id, &conns_by_id, id, &ret);
    ensure(ret >= 1, "inserted returned %d", ret);
    kh_val(&conns_by_id, k) = c;
}


static inline void __attribute__((nonnull))
conns_by_id_del(struct cid * const id)
{
    const khiter_t k = kh_get(conns_by_id, &conns_by_id, id);
    ensure(k != kh_end(&conns_by_id), "found");
    kh_del(conns_by_id, &conns_by_id, k);
}
#endif


#ifndef NO_SERVER
static void __attribute__((nonnull)) update_act_scid(struct q_conn * const c)
{
    // server picks a new random cid
    struct cid nscid = {.seq = 0};
    mk_rand_cid(&nscid, ped(c->w)->conf.server_cid_len, true);
    cid_cpy(&c->odcid, c->scid);
    mk_cid_str(NTE, &nscid, scid_str_new);
    mk_cid_str(NTE, c->scid, scid_str_prev);
    warn(NTE, "hshk switch to scid %s for %s %s conn (was %s)", scid_str_new,
         conn_state_str[c->state], conn_type(c), scid_str_prev);
#ifndef NO_MIGRATION
    conns_by_id_del(c->scid);
    cids_by_id_del(&c->scids_by_id, c->scid);
#endif
    cid_cpy(c->scid, &nscid);
#ifndef NO_MIGRATION
    cids_by_id_ins(&c->scids_by_id, c->scid);
    conns_by_id_ins(c, c->scid);
#endif

    // we need to keep accepting the client-chosen odcid for 0-RTT pkts
#ifndef NO_MIGRATION
    cids_by_id_ins(&c->scids_by_id, &c->odcid);
    conns_by_id_ins(c, &c->odcid);
#endif
}
#endif


#ifndef NO_MIGRATION
void add_scid(struct q_conn * const c, struct cid * const id)
{
    struct cid * const scid = calloc(1, sizeof(*scid));
    ensure(scid, "could not calloc");
    cid_cpy(scid, id);
    ensure(splay_insert(cids_by_seq, &c->scids_by_seq, scid) == 0, "inserted");
    cids_by_id_ins(&c->scids_by_id, scid);
    if (c->scid == 0)
        c->scid = scid;
    conns_by_id_ins(c, scid);
}
#endif


void add_dcid(struct q_conn * const c, const struct cid * const id)
{
    struct cid * dcid =
#ifndef NO_MIGRATION
        splay_find(cids_by_seq, &c->dcids_by_seq, id);
#else
        c->dcid;
#endif
    if (dcid == 0) {
        dcid = calloc(1, sizeof(*dcid));
        ensure(dcid, "could not calloc");
        if (c->dcid == 0)
            c->dcid = dcid;
    } else {
        mk_cid_str(NTE, id, dcid_str_new);
        mk_cid_str(NTE, c->dcid, dcid_str_prev);
        warn(NTE, "hshk switch to dcid %s for %s conn (was %s)", dcid_str_new,
             conn_type(c), dcid_str_prev);
#ifndef NO_MIGRATION
        ensure(splay_remove(cids_by_seq, &c->dcids_by_seq, dcid), "removed");
#endif
#ifndef NO_SRT_MATCHING
        if (dcid->has_srt)
            conns_by_srt_del(dcid->srt);
#endif
    }
    cid_cpy(dcid, id);
#ifndef NO_SRT_MATCHING
    if (id->has_srt)
        conns_by_srt_ins(c, dcid->srt);
#endif
#ifndef NO_MIGRATION
    ensure(splay_insert(cids_by_seq, &c->dcids_by_seq, dcid) == 0, "inserted");
#endif
}


static void __attribute__((nonnull))
rx_crypto(struct q_conn * const c, const struct pkt_meta * const m_cur)
{
    struct q_stream * const s = c->cstrms[epoch_in(c)];
    while (unlikely(!sq_empty(&s->in))) {
        // take the data out of the crypto stream
        struct w_iov * const v = sq_first(&s->in);
        sq_remove_head(&s->in, next);
        sq_next(v, next) = 0;

        // ooo crypto pkts have stream cleared by dec_stream_or_crypto_frame()
        struct pkt_meta * const m = &meta(v);
        const bool free_ooo = m->strm == 0;
        // mark this (potential in-order) pkt for freeing in rx_pkts()
        m->strm = 0;

        const int ret = tls_io(s, v);
        if (free_ooo && m != m_cur)
            free_iov(v, m);
        if (ret)
            continue;

        if (c->state == conn_idle || c->state == conn_opng) {
            conn_to_state(c, conn_estb);
            if (is_clnt(c))
                maybe_api_return(q_connect, c, 0);
#ifndef NO_SERVER
            else if (c->needs_accept == false) {
                sl_insert_head(&accept_queue, c, node_aq);
                c->needs_accept = true;
            }

#endif
        }
    }
}


static void __attribute__((nonnull)) free_cids(struct q_conn * const c)
{
#ifndef NO_MIGRATION
    if (is_clnt(c) == false && c->odcid.len) {
        // TODO: we should stop accepting pkts on the client odcid earlier
        cids_by_id_del(&c->scids_by_id, &c->odcid);
        conns_by_id_del(&c->odcid);
    }

    while (!splay_empty(&c->scids_by_seq)) {
        struct cid * const id = splay_min(cids_by_seq, &c->scids_by_seq);
        free_scid(c, id);
    }

    while (!splay_empty(&c->dcids_by_seq)) {
        struct cid * const id = splay_min(cids_by_seq, &c->dcids_by_seq);
        free_dcid(c, id);
    }
#else
    if (c->scid)
        free_scid(c, c->scid);
    if (c->dcid)
        free_dcid(c, c->dcid);
#endif

    c->scid = c->dcid = 0;
}


static void __attribute__((nonnull(1)))
new_initial_cids(struct q_conn * const c,
                 const struct cid * const dcid,
                 const struct cid * const scid)
{
    // init dcid
    if (is_clnt(c)) {
        struct cid ndcid = {.seq = 0};
        mk_rand_cid(&ndcid, CID_LEN_MAX + 1, false); // random len
        cid_cpy(&c->odcid, &ndcid);
        add_dcid(c, &ndcid);
    } else if (dcid)
        // dcid->seq is 0 due to calloc allocation
        add_dcid(c, dcid);

    // init scid and add connection to global data structures
    struct cid nscid = {.seq = 0};
    if (is_clnt(c))
        mk_rand_cid(&nscid, ped(c->w)->conf.client_cid_len, false);
    else if (scid) {
        cid_cpy(&nscid, scid);
        mk_rand_cid(&nscid, 0, true);
    }
#ifndef NO_MIGRATION
    if (nscid.len)
        add_scid(c, &nscid);
    else
#endif
        if (c->in_c_zcid == false) {
        sl_insert_head(&c_zcid, c, node_zcid_int);
        c->in_c_zcid = true;
    }
}


static void __attribute__((nonnull))
vneg_or_rtry_resp(struct q_conn * const c, const bool is_vneg)
{
    // reset FC state
    c->in_data_str = c->out_data_str = 0;

    for (epoch_t e = ep_init; e <= ep_data; e++)
        if (c->cstrms[e])
            reset_stream(c->cstrms[e], true);

    struct q_stream * s;
    kh_foreach_value(&c->strms_by_id, s, reset_stream(s, false));

    // reset packet number spaces
    for (pn_t t = pn_init; t <= pn_data; t++)
        reset_pn(&c->pns[t]);

    if (is_vneg) {
        // reset CIDs
        free_cids(c); // this zeros c->scid
        new_initial_cids(c, 0, 0);
    }

    // reset CC state
    init_rec(c);

    // reset TLS state and create new CH
    const bool should_try_0rtt = c->try_0rtt;
    init_tls(c, 0, (char *)c->tls.alpn.base);
    c->try_0rtt = should_try_0rtt;
    tls_io(c->cstrms[ep_init], 0);

    // switch to new qlog file
    if (ped(c->w)->conf.qlog_dir)
        qlog_init(c);
}


#ifndef NDEBUG
static bool __attribute__((const))
pkt_ok_for_epoch(const uint8_t flags, const epoch_t epoch)
{
    switch (epoch) {
    case ep_init:
        return pkt_type(flags) == LH_INIT || pkt_type(flags) == LH_RTRY;
    case ep_0rtt:
    case ep_hshk:
        return is_lh(flags);
    case ep_data:
        return true;
    default:
        return false;
    }
}
#endif


static bool __attribute__((nonnull)) rx_pkt(const struct w_sock * const ws
#ifdef NO_SERVER
                                            __attribute__((unused))
#endif
                                            ,
                                            struct w_iov * v,
                                            struct pkt_meta * m,
                                            struct w_iov_sq * const x
#if defined(NO_OOO_0RTT) || defined(NO_SERVER)
                                            __attribute__((unused))
#endif
                                            ,
                                            const uint8_t * const tok,
                                            const uint16_t tok_len,
                                            const uint8_t * const rit
#ifdef NDEBUG
                                            __attribute__((unused))
#endif
)
{
    struct q_conn * const c = m->pn->c;
    bool ok = false;

    log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
    c->in_data += m->udp_len;

    if (is_clnt(c) == false && unlikely(c->path_val_win != UINT_T_MAX))
        // server limits response to 3x incoming pkt
        c->path_val_win += 3 * m->udp_len;

    switch (c->state) {
    case conn_idle:
#ifndef NO_SERVER
        // this is a new connection
        c->vers = m->hdr.vers;

        // TODO: remove this interop hack eventually
        if (bswap16(ws->ws_lport) == 4434 || ped(c->w)->conf.force_retry) {
            if (m->hdr.type == LH_INIT && tok_len) {
                if (verify_rtry_tok(c, tok, tok_len) == false) {
                    warn(ERR, "retry token verification failed");
                    enter_closing(c);
                    goto done;
                }
            } else {
                if (c->tx_rtry) {
                    warn(DBG, "already tx'ing retry, ignoring");
                    goto done;
                }
                warn(INF, "sending retry");
                // send a RETRY
                make_rtry_tok(c);
                ok = true;
                c->needs_tx = c->tx_rtry = true;
                update_act_scid(c);
                goto done;
            }
        }

#ifdef DEBUG_EXTRA
        warn(INF, "supporting clnt-requested vers 0x%0" PRIx32, c->vers);
#endif
        if (dec_frames(c, &v, &m) == false)
            goto done;

        // if the CH doesn't include any crypto frames, bail
        if (has_frm(m->frms, FRM_CRY) == false) {
            warn(ERR, "initial pkt w/o crypto frames");
            enter_closing(c);
            goto done;
        }

        init_tp(c);

#ifndef NO_OOO_0RTT
        // check if any reordered 0-RTT packets are cached for this CID
        const struct ooo_0rtt which = {.cid = m->hdr.dcid};
        struct ooo_0rtt * const zo =
            splay_find(ooo_0rtt_by_cid, &ooo_0rtt_by_cid, &which);
        if (zo) {
            warn(INF, "have reordered 0-RTT pkt for %s conn %s", conn_type(c),
                 cid_str(c->scid));
            ensure(splay_remove(ooo_0rtt_by_cid, &ooo_0rtt_by_cid, zo),
                   "removed");
            sq_insert_head(x, zo->v, next);
            free(zo);
        }
#endif

        conn_to_state(c, conn_opng);

        // server picks a new random cid
        update_act_scid(c);

        ok = true;
#endif
        break;

    case conn_opng:
        if (is_clnt(c)) {
            if (m->hdr.vers == 0) {
                // this is a vneg pkt
                m->hdr.nr = UINT_T_MAX;
                if (c->vers != c->vers_initial) {
                    // we must have already reacted to a prior vneg pkt
                    warn(INF, "ignoring spurious vneg response");
                    goto done;
                }

                // check that the rx'ed CIDs match our tx'ed CIDs
                const bool rx_scid_ok = !cid_cmp(&m->hdr.scid, c->dcid);
                const bool rxed_dcid_ok =
                    m->hdr.dcid.len == 0 || !cid_cmp(&m->hdr.dcid, c->scid);
                if (rx_scid_ok == false || rxed_dcid_ok == false) {
                    mk_cid_str(INF, rx_scid_ok ? &m->hdr.dcid : &m->hdr.scid,
                               cid_str_rx);
                    mk_cid_str(INF, rx_scid_ok ? c->scid : c->dcid, cid_str_tx);
                    warn(INF, "vneg %ccid mismatch: rx %s != %s",
                         rx_scid_ok ? 'd' : 's', cid_str_rx, cid_str_tx);
                    enter_closing(c);
                    goto done;
                }

                // only do vneg for draft and vneg versions
                if (is_vneg_vers(c->vers) == false &&
                    is_draft_vers(c->vers) == false) {
                    enter_closing(c);
                    goto done;
                }

                // handle an incoming vneg packet
                const uint32_t try_vers =
                    clnt_vneg(v->buf + m->hdr.hdr_len, v->buf + v->len);
                if (try_vers == 0) {
                    // no version in common with serv
                    enter_closing(c);
                    goto done;
                }

                vneg_or_rtry_resp(c, true);
                c->vers = try_vers;
                warn(INF,
                     "serv didn't like vers 0x%0" PRIx32
                     ", retrying with 0x%0" PRIx32 "",
                     c->vers_initial, c->vers);
                ok = true;
                goto done;
            }

            if (unlikely(m->hdr.vers != c->vers)) {
                warn(ERR,
                     "serv response w/vers 0x%0" PRIx32
                     " to CI w/vers 0x%0" PRIx32 ", ignoring",
                     m->hdr.vers, c->vers);
                goto done;
            }

            if (m->hdr.type == LH_RTRY) {
                m->hdr.nr = UINT_T_MAX;
                if (c->tok_len) {
                    // we already had an earlier RETRY on this connection
                    warn(INF, "already handled a retry, ignoring");
                    goto done;
                }

                // handle an incoming retry packet
                c->tok_len = tok_len;
                memcpy(c->tok, tok, c->tok_len);
                vneg_or_rtry_resp(c, false);
                warn(INF, "handling serv retry w/tok %s",
                     tok_str(c->tok, c->tok_len));
                ok = true;
                goto done;
            }
            // server accepted version -
            // if we get here, this should be a regular server-hello
        }

        ok = dec_frames(c, &v, &m);
        break;

    case conn_estb:
    case conn_qlse:
    case conn_clsg:
    case conn_drng:
        if (is_lh(m->hdr.flags) && m->hdr.vers == 0) {
            // we shouldn't get another vneg packet here, ignore
            warn(NTE, "ignoring spurious vneg response");
            goto done;
        }

        // ignore 0-RTT packets if we're not doing 0-RTT
        if (c->did_0rtt == false && m->hdr.type == LH_0RTT) {
            warn(NTE, "ignoring 0-RTT pkt");
            goto done;
        }

        if (dec_frames(c, &v, &m) == false)
            goto done;

        ok = true;
        break;

    case conn_clsd:
        warn(NTE, "ignoring pkt for closed %s conn", conn_type(c));
        break;
    }

done:
    if (unlikely(ok == false))
        return false;

    if (likely(m->hdr.nr != UINT_T_MAX)) {
        struct pn_space * const pn = pn_for_pkt_type(c, m->hdr.type);
        // update ECN info
        switch (v->flags & IPTOS_ECN_MASK) {
        case IPTOS_ECN_ECT1:
            pn->ect1_cnt++;
            break;
        case IPTOS_ECN_ECT0:
            pn->ect0_cnt++;
            break;
        case IPTOS_ECN_CE:
            pn->ce_cnt++;
            break;
        }
        pn->pkts_rxed_since_last_ack_tx++;

        // if (pn == &c->pns[pn_data] && pn->pkts_rxed_since_last_ack_tx >= 16)
        //     tx_ack(c, ep_data, false);
    }

#ifndef NO_QLOG
    // if pkt has STREAM or CRYPTO frame but no strm pointer, it's a dup
    static const struct frames qlog_dup_chk =
        bitset_t_initializer(1 << FRM_CRY | 1 << FRM_STR);
    const bool dup_strm =
        bit_overlap(FRM_MAX, &m->frms, &qlog_dup_chk) && m->strm == 0;
    qlog_transport(dup_strm ? pkt_dp : pkt_rx, "default", v, m);
#endif
    return true;
}


#ifdef FUZZING
void
#else
static void __attribute__((nonnull))
#endif
    rx_pkts(struct w_iov_sq * const x, struct q_conn_sl * const crx,
            struct w_sock * const ws)
{
    struct cid outer_dcid = {0};
    while (!sq_empty(x)) {
        struct w_iov * const xv = sq_first(x);
        sq_remove_head(x, next);
        sq_next(xv, next) = 0;

#if !defined(NDEBUG) && !defined(FUZZING) && defined(FUZZER_CORPUS_COLLECTION)
        // when called from the fuzzer, xv->wv_af is zero
        if (xv->wv_af)
            write_to_corpus(corpus_pkt_dir, xv->buf, xv->len);
#endif

        // allocate new w_iov for the (eventual) unencrypted data and meta-data
        struct pkt_meta * m;
        struct w_iov * const v = alloc_iov(ws->w, ws->ws_af, 0, 0, &m);
        v->saddr = xv->saddr;
        v->flags = xv->flags;
        v->ttl = xv->ttl;
        v->len = xv->len; // this is just so that log_pkt can show the rx len
        m->t = loop_now();

        bool pkt_valid = false;
        const bool is_clnt = w_connected(ws);
        struct q_conn * c = 0;
        uint8_t tok[MAX_TOK_LEN];
        uint16_t tok_len = 0;
        uint8_t rit[RIT_LEN];
        if (unlikely(!dec_pkt_hdr_beginning(
                xv, v, m, is_clnt, tok, &tok_len, rit,
                is_clnt ? (ws->data ? 0 : ped(ws->w)->conf.client_cid_len)
                        : ped(ws->w)->conf.server_cid_len))) {
            // we might still need to send a vneg packet
            if (w_connected(ws) == false) {
                if (m->hdr.scid.len == 0 || m->hdr.scid.len >= 4) {
                    warn(ERR, "received invalid %u-byte %s pkt, sending vneg",
                         v->len, pkt_type_str(m->hdr.flags, &m->hdr.vers));
                    tx_vneg_resp(ws, v, m);
                } else {
                    log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                    warn(ERR,
                         "received invalid %u-byte %s pkt w/invalid scid len "
                         "%u, ignoring",
                         v->len, pkt_type_str(m->hdr.flags, &m->hdr.vers),
                         m->hdr.scid.len);
                    goto drop;
                }
            } else
                warn(ERR, "received invalid %u-byte %s pkt, ignoring", v->len,
                     pkt_type_str(m->hdr.flags, &m->hdr.vers));
            // can't log packet, because it may be too short for log_pkt()
            goto drop;
        }

#ifndef NO_MIGRATION
        c = get_conn_by_cid(&m->hdr.dcid);
        if (c == 0 && m->hdr.dcid.len == 0)
#endif
            c = (struct q_conn *)ws->data;
        if (likely(is_lh(m->hdr.flags)) && !is_clnt) {
            if (c && m->hdr.type == LH_0RTT) {
                mk_cid_str(INF, &m->hdr.dcid, dcid_str_prev);
                mk_cid_str(INF, c->scid, dcid_str_cur);
                if (c->did_0rtt)
                    warn(INF,
                         "got 0-RTT pkt for orig cid %s, new is %s, "
                         "accepting",
                         dcid_str_prev, dcid_str_cur);
                else {
                    log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                    warn(WRN,
                         "got 0-RTT pkt for orig cid %s, new is %s, "
                         "but rejected 0-RTT, ignoring",
                         dcid_str_prev, dcid_str_cur);
                    goto drop;
                }
            } else if (m->hdr.type == LH_INIT && c == 0) {
                // validate minimum packet size
                if (xv->len < MIN_INI_LEN) {
                    log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                    warn(ERR, "%u-byte Initial pkt too short (< %u)", xv->len,
                         MIN_INI_LEN);
                    goto drop;
                }

                if (vers_supported(m->hdr.vers) == false ||
                    is_vneg_vers(m->hdr.vers)) {
                    log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                    warn(WRN,
                         "clnt-requested vers 0x%0" PRIx32 " not supported",
                         m->hdr.vers);
                    if (m->hdr.vers != 0)
                        // only reply to non-vneg packets
                        tx_vneg_resp(ws, v, m);
                    goto drop;
                }

                warn(NTE, "new serv conn on port %u from %s%s%s:%u w/cid=%s",
                     bswap16(ws->ws_lport), v->wv_af == AF_INET6 ? "[" : "",
                     w_ntop(&v->wv_addr, ip_tmp),
                     v->wv_af == AF_INET6 ? "]" : "", v->saddr.port,
                     cid_str(&m->hdr.dcid));

                c = new_conn(w_engine(ws), UINT16_MAX, &m->hdr.scid,
                             &m->hdr.dcid, &v->saddr, 0, ws->ws_lport,
                             &(struct q_conn_conf){.version = m->hdr.vers});
                if (likely(c))
                    init_tls(c, 0, 0);
            }
        }

        if (likely(c)) {
            // FIXME: validate cid len of non-first Initial packets to >= 8

            if (m->hdr.scid.len && cid_cmp(&m->hdr.scid, c->dcid) != 0) {
                if (m->hdr.vers && m->hdr.type == LH_RTRY) {
                    uint8_t computed_rit[RIT_LEN];
                    make_rit(c, m->hdr.flags, &m->hdr.dcid, &m->hdr.scid, tok,
                             tok_len, computed_rit);
                    if (memcmp(rit, computed_rit, RIT_LEN) != 0) {
                        log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                        warn(ERR, "rit mismatch, computed %s",
                             rit_str(computed_rit));
                        goto drop;
                    }
                }
                if (c->state == conn_opng &&
                    (m->hdr.type != LH_RTRY || c->tok_len == 0))
                    add_dcid(c, &m->hdr.scid);
            }

        } else {
#if !defined(FUZZING) && !defined(NO_OOO_0RTT)
            // if this is a 0-RTT pkt, track it (may be reordered)
            if (m->hdr.type == LH_0RTT && m->hdr.vers) {
                struct ooo_0rtt * const zo = calloc(1, sizeof(*zo));
                ensure(zo, "could not calloc");
                cid_cpy(&zo->cid, &m->hdr.dcid);
                zo->v = v;
                ensure(splay_insert(ooo_0rtt_by_cid, &ooo_0rtt_by_cid, zo) == 0,
                       "inserted");
                log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                warn(INF, "caching 0-RTT pkt for unknown conn %s",
                     cid_str(&m->hdr.dcid));
                goto next;
            }
#endif
            log_pkt("RX", v, &v->saddr, tok, tok_len, rit);

            if (is_srt(xv, m)) {
                warn(INF, BLU BLD "STATELESS RESET" NRM " token=%s",
                     srt_str(&xv->buf[xv->len - SRT_LEN]));
                goto next;
            }

            warn(INF, "cannot find conn %s for %u-byte %s pkt, ignoring",
                 cid_str(&m->hdr.dcid), v->len,
                 pkt_type_str(m->hdr.flags, &m->hdr.vers));
            goto drop;
        }

        if (likely(has_pkt_nr(m->hdr.flags, m->hdr.vers))) {
            bool decoal;
            if (unlikely(m->hdr.type == LH_INIT && c->cstrms[ep_init] == 0)) {
                // we already abandoned Initial pkt processing, ignore
                log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                warn(INF, "ignoring %u-byte %s pkt due to abandoned processing",
                     v->len, pkt_type_str(m->hdr.flags, &m->hdr.vers));
                goto drop;
            } else if (unlikely(dec_pkt_hdr_remainder(xv, v, m, c, x,
                                                      &decoal) == false)) {
                v->len = xv->len;
                log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                if (m->is_reset)
                    warn(INF, BLU BLD "STATELESS RESET" NRM " token=%s",
                         srt_str(&xv->buf[xv->len - SRT_LEN]));
                else
                    warn(ERR, "%s %u-byte %s pkt, ignoring",
                         pkt_ok_for_epoch(m->hdr.flags, epoch_in(c))
                             ? "crypto fail on"
                             : "rx invalid",
                         v->len, pkt_type_str(m->hdr.flags, &m->hdr.vers));
                goto drop;
            }

            if (m->hdr.dcid.len && cid_cmp(&m->hdr.dcid, c->scid) != 0) {
                struct cid * const scid =
#ifndef NO_MIGRATION
                    get_cid_by_id(&c->scids_by_id, &m->hdr.dcid);
#else
                    c->scid;
#endif
                if (unlikely(scid == 0)) {
                    log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                    warn(ERR, "unknown scid %s, ignoring pkt",
                         cid_str(&m->hdr.dcid));
                    goto drop;
                }

                mk_cid_str(NTE, scid, scid_str);
                mk_cid_str(NTE, c->scid, scid_str_prev);
                if (scid->seq <= c->scid->seq)
                    warn(DBG, "pkt has prev scid %s (expected %s), accepting",
                         scid_str, scid_str_prev);
                else {
                    warn(NTE, "migration to scid %s for %s conn (was %s)",
                         scid_str, conn_type(c), scid_str_prev);
                    c->scid = scid;
                }
            }

            // that dcid in split-out coalesced pkt matches outer pkt
            if (unlikely(decoal) && outer_dcid.len == 0) {
                // save outer dcid for checking
                cid_cpy(&outer_dcid, &m->hdr.dcid);
                goto decoal_done;
            }

            if (unlikely(outer_dcid.len) &&
                cid_cmp(&outer_dcid, &m->hdr.dcid) != 0) {
                log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                mk_cid_str(ERR, &outer_dcid, outer_dcid_str);
                mk_cid_str(ERR, &m->hdr.dcid, dcid_str);
                warn(ERR,
                     "outer dcid %s != inner dcid %s during decoalescing, "
                     "ignoring %s pkt",
                     outer_dcid_str, dcid_str,
                     pkt_type_str(m->hdr.flags, &m->hdr.vers));
                goto drop;
            }

            if (likely(decoal == false))
                // forget outer dcid
                outer_dcid.len = 0;

            // check if this pkt came from a new source IP and/or port
            if (w_sockaddr_cmp(&c->peer, &v->saddr) == false
#ifndef NO_MIGRATION
                && (c->tx_path_chlg == false ||
                    w_sockaddr_cmp(&c->migr_peer, &v->saddr) == false)
#endif
            ) {
#if !defined(NO_MIGRATION) || !defined(NDEBUG)
                const uint_t max_recv_all = diet_max(&c->pns[pn_data].recv_all);
#endif
#ifndef NO_MIGRATION
                if (m->hdr.nr <= max_recv_all) {
#endif
                    log_pkt("RX", v, &v->saddr, tok, tok_len, rit);
                    warn(NTE,
                         "pkt from new peer %s%s%s:%u, nr " FMT_PNR_IN
                         " <= max " FMT_PNR_IN ", ignoring",
                         v->wv_af == AF_INET6 ? "[" : "",
                         w_ntop(&v->wv_addr, ip_tmp),
                         v->wv_af == AF_INET6 ? "]" : "",
                         bswap16(v->saddr.port), m->hdr.nr, max_recv_all);
                    goto drop;
#ifndef NO_MIGRATION
                }

                warn(NTE,
                     "pkt from new peer %s%s%s:%u, nr " FMT_PNR_IN
                     " > max " FMT_PNR_IN ", probing",
                     v->wv_af == AF_INET6 ? "[" : "",
                     w_ntop(&v->wv_addr, ip_tmp),
                     v->wv_af == AF_INET6 ? "]" : "", bswap16(v->saddr.port),
                     m->hdr.nr, max_recv_all);

                rand_bytes(&c->path_chlg_out, sizeof(c->path_chlg_out));
                c->migr_peer = v->saddr;
                c->migr_sock = ws;
                c->needs_tx = c->tx_path_chlg = true;
                c->tx_limit = 1;
#endif
            }
        } else
            // this is a vneg or rtry pkt, dec_pkt_hdr_remainder not called
            m->pn = &c->pns[pn_init];

    decoal_done:
        if (likely(rx_pkt(ws, v, m, x, tok, tok_len, rit))) {
            rx_crypto(c, m);
            c->min_rx_epoch = c->had_rx ? MIN(c->min_rx_epoch,
                                              epoch_for_pkt_type(m->hdr.type))
                                        : epoch_for_pkt_type(m->hdr.type);

            if (likely(has_pkt_nr(m->hdr.flags, m->hdr.vers))) {
                struct pn_space * const pn = pn_for_pkt_type(c, m->hdr.type);
#ifdef NO_OOO_DATA
                if (m->strm_off == UINT_T_MAX)
                    // don't ACK this ooo packet
                    goto drop;
#endif
                diet_insert(&pn->recv, m->hdr.nr, m->t);
                diet_insert(&pn->recv_all, m->hdr.nr, 0);
            }
            pkt_valid = true;

            // remember that we had a RX event on this connection
            if (unlikely(!c->had_rx)) {
                c->had_rx = true;
                sl_insert_head(crx, c, node_rx_int);
            }
        }

        if (m->strm == 0)
            // we didn't place this pkt in any stream - bye!
            goto drop;
        else if (unlikely(m->strm->state == strm_clsd &&
                          sq_empty(&m->strm->in)))
            free_stream(m->strm);
        goto next;

    drop:
        if (likely(c) && !is_clnt(c) &&
            unlikely(c->state == conn_idle && c->tx_rtry == false)) {
            // drop server connection on invalid clnt Initial
            warn(DBG, "dropping idle %s conn %s", conn_type(c),
                 cid_str(c->scid));
            if (c->had_rx)
                // we inserted at head above, so can simply take it out again
                sl_remove_head(crx, node_rx_int);
            free_conn(c);
            c = 0;
        } else if (pkt_valid == false)
            qlog_transport(pkt_dp, "default", v, m);
        free_iov(v, m);
    next:
#ifndef NO_QINFO
        if (likely(c)) {
            if (likely(pkt_valid))
                c->i.pkts_in_valid++;
            else
                c->i.pkts_in_invalid++;
        }
#endif
        w_free_iov(xv);
    }
}


void restart_idle_alarm(struct q_conn * const c)
{
    if (c->tp_mine.max_idle_to || c->tp_peer.max_idle_to) {
        const timeout_t min_of_max_idle_to =
            MIN(c->tp_mine.max_idle_to ? c->tp_mine.max_idle_to : UINT64_MAX,
                c->tp_peer.max_idle_to ? c->tp_peer.max_idle_to : UINT64_MAX);
        const timeout_t t =
            MAX(min_of_max_idle_to * NS_PER_MS, 3 * c->rec.ld_alarm_val);
#ifdef DEBUG_TIMERS
        warn(DBG, "next idle alarm on %s conn %s in %.3f sec", conn_type(c),
             cid_str(c->scid), (double)t / NS_PER_S);
#endif
        timeouts_add(ped(c->w)->wheel, &c->idle_alarm, t);
    }
#ifdef DEBUG_TIMERS
    else
        warn(DBG, "stopping idle alarm on %s conn %s", conn_type(c),
             cid_str(c->scid));
#endif
}


static void __attribute__((nonnull)) restart_ack_alarm(struct q_conn * const c)
{
    const timeout_t t = c->tp_mine.max_ack_del * NS_PER_MS;

#ifdef DEBUG_TIMERS
    warn(DBG, "next ACK alarm in %.3f sec", (double)t / NS_PER_S);
#endif

    timeouts_add(ped(c->w)->wheel, &c->ack_alarm, t);
}


void rx(struct w_sock * const ws)
{
    struct w_iov_sq x = w_iov_sq_initializer(x);
    struct q_conn_sl crx = sl_head_initializer(crx);
    w_rx(ws, &x);
    rx_pkts(&x, &crx, ws);

    // for all connections that had RX events
    while (!sl_empty(&crx)) {
        struct q_conn * const c = sl_first(&crx);
        sl_remove_head(&crx, node_rx_int);

        // clear the helper flags set above
        c->had_rx = false;

        if (unlikely(c->state == conn_drng))
            continue;

        // reset idle timeout
        if (likely(c->pns[pn_data].data.out_kyph ==
                   c->pns[pn_data].data.in_kyph))
            restart_idle_alarm(c);

        // is a TX needed for this connection?
        if (c->needs_tx)
            tx(c); // clears c->needs_tx if we TX'ed

        for (epoch_t e = c->min_rx_epoch; e <= ep_data; e++) {
            if (c->cstrms[e] == 0 || e == ep_0rtt)
                // don't ACK abandoned and 0rtt pn spaces
                continue;
            struct pn_space * const pn = pn_for_epoch(c, e);
            switch (needs_ack(pn)) {
            case imm_ack:
                c->needs_tx = true;
                timeouts_add(ped(c->w)->wheel, &c->tx_w, 0);
                break;
            case del_ack:
                if (likely(c->state != conn_clsg))
                    restart_ack_alarm(c);
                break;
            case no_ack:
            case grat_ack:
                break;
            }
        }

#ifndef NO_SERVER
        if (unlikely(c->tx_rtry))
            // if we sent a retry, forget the entire connection existed
            free_conn(c);
        else
#endif
            if (c->have_new_data && !c->in_c_ready) {
            sl_insert_head(&c_ready, c, node_rx_ext);
            c->in_c_ready = true;
            maybe_api_return(q_ready, 0, 0);
        }
    }
}


void
#ifndef NO_ERR_REASONS
    err_close
#else
    err_close_noreason
#endif
        (struct q_conn * const c, const uint_t code, const uint8_t frm
#ifndef NO_ERR_REASONS
         ,
         const char * const fmt, ...
#endif
        )
{
#ifndef FUZZING
    if (unlikely(c->err_code)) {
#ifndef NO_ERR_REASONS
        warn(WRN,
             "ignoring new err 0x%" PRIx "; existing err is 0x%" PRIx " (%s) ",
             code, c->err_code, c->err_reason);
#endif
        return;
    }
#endif

#ifndef NO_ERR_REASONS
    va_list ap;
    va_start(ap, fmt);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    const int ret = vsnprintf(c->err_reason, sizeof(c->err_reason), fmt, ap);
    ensure(ret >= 0, "vsnprintf() failed");
    va_end(ap);

    warn(ERR, "%s", c->err_reason);
    c->err_reason_len =
        (uint8_t)MIN((unsigned long)ret + 1, sizeof(c->err_reason));
#endif
    conn_to_state(c, conn_qlse);
    c->err_code = code;
    c->err_frm = frm;
    c->needs_tx = true;
    enter_closing(c);
}


static void __attribute__((nonnull)) key_flip_alarm(struct q_conn * const c)
{
#ifdef DEBUG_TIMERS
    warn(DBG, "key flip timer fired on %s conn %s", conn_type(c),
         cid_str(c->scid));
#endif

    if (hshk_done(c)) {
        c->do_key_flip = c->key_flips_enabled;
#ifndef NO_MIGRATION
        // XXX we borrow the key flip timer for this
        c->do_migration = !c->tp_peer.disable_active_migration;
#endif
    }
}


static void __attribute__((nonnull)) stop_all_alarms(struct q_conn * const c)
{
    timeout_del(&c->rec.ld_alarm);
    timeout_del(&c->idle_alarm);
    timeout_del(&c->key_flip_alarm);
    timeout_del(&c->ack_alarm);
    timeout_del(&c->closing_alarm);
}


static void __attribute__((nonnull)) enter_closed(struct q_conn * const c)
{
#ifdef DEBUG_TIMERS
    warn(DBG, "closing timeout on %s conn %s", conn_type(c), cid_str(c->scid));
#endif

    conn_to_state(c, conn_clsd);
    stop_all_alarms(c);

    if (!c->in_c_ready) {
        sl_insert_head(&c_ready, c, node_rx_ext);
        c->in_c_ready = true;
    }

    // terminate whatever API call is currently active
    maybe_api_return(c, 0);
    maybe_api_return(q_ready, 0, 0);
}


void enter_closing(struct q_conn * const c)
{
    stop_all_alarms(c);

#ifndef FUZZING
    if ((c->state == conn_idle || c->state == conn_opng) && c->err_code == 0) {
#endif
        // no need to go closing->draining in these cases
        enter_closed(c);
        return;
#ifndef FUZZING
    }
#endif

#ifndef FUZZING
    // start closing/draining alarm (3 * RTO)
    const timeout_t dur =
        3 * (c->rec.cur.srtt == 0 ? kInitialRtt : c->rec.cur.srtt * NS_PER_US) +
        4 * c->rec.cur.rttvar * NS_PER_US;
    timeouts_add(ped(c->w)->wheel, &c->closing_alarm, dur);
#ifdef DEBUG_TIMERS
    warn(DBG, "closing/draining alarm in %.3f sec on %s conn %s",
         (double)dur / NS_PER_S, conn_type(c), cid_str(c->scid));
#endif
#endif

    if (c->state != conn_clsg) {
        c->needs_tx = true;
        conn_to_state(c, conn_clsg);
    }
}

static void __attribute__((nonnull)) idle_alarm(struct q_conn * const c)
{
#ifdef DEBUG_TIMERS
    warn(DBG, "idle timeout on %s conn %s", conn_type(c), cid_str(c->scid));
#endif
    enter_closing(c);
    enter_closed(c);
}


static void __attribute__((nonnull)) ack_alarm(struct q_conn * const c)
{
#ifdef DEBUG_TIMERS
    warn(DBG, "ACK timer fired on %s conn %s", conn_type(c), cid_str(c->scid));
#endif
    if (needs_ack(&c->pns[pn_data]) != no_ack)
        if (tx_ack(c, ep_data, false))
            do_tx(c);
}


void update_conf(struct q_conn * const c, const struct q_conn_conf * const conf)
{
    c->spin_enabled = get_conf_uncond(c->w, conf, enable_spinbit);
    c->do_qr_test = get_conf_uncond(c->w, conf, enable_quantum_readiness_test);

    // (re)set idle alarm
    c->tp_mine.max_idle_to =
        get_conf_uncond(c->w, conf, idle_timeout) * MS_PER_S;
    restart_idle_alarm(c);

    c->tp_mine.disable_active_migration =
#ifndef NO_MIGRATION
        get_conf_uncond(c->w, conf, disable_active_migration);
#else
        true;
#endif
    c->key_flips_enabled = get_conf_uncond(c->w, conf, enable_tls_key_updates);

    if (c->tp_peer.disable_active_migration == false || c->key_flips_enabled) {
        c->tls_key_update_frequency =
            get_conf(c->w, conf, tls_key_update_frequency);
        restart_key_flip_alarm(c);
    }

    c->sockopt.enable_udp_zero_checksums =
        get_conf_uncond(c->w, conf, enable_udp_zero_checksums);
    w_set_sockopt(c->sock, &c->sockopt);

#ifndef NDEBUG
    // XXX for testing, do a key flip and a migration ASAP (if enabled)
    c->do_key_flip = c->key_flips_enabled;
#ifndef NO_MIGRATION
    c->do_migration = !c->tp_peer.disable_active_migration;
#endif
#endif
}


struct q_conn * new_conn(struct w_engine * const w,
                         const uint16_t addr_idx,
                         const struct cid * const dcid,
                         const struct cid * const scid,
                         const struct w_sockaddr * const peer,
                         const char * const peer_name
#ifdef NO_SERVER
                         __attribute__((unused))
#endif
                         ,
                         const uint16_t port,
                         const struct q_conn_conf * const conf)
{
    struct q_conn * const c = calloc(1, sizeof(*c));
    ensure(c, "could not calloc");

    c->pmtud_pkt = UINT16_MAX;
    c->w = w;
#ifndef NO_SERVER
    c->is_clnt = peer_name != 0;
#endif

    uint16_t idx = addr_idx;
    if (peer) {
        c->peer = *peer;
        if (addr_idx == UINT16_MAX) {
            // find a src address of the same family as the peer address
            for (idx = 0; idx < w->addr_cnt; idx++)
                if (w->ifaddr[idx].addr.af == peer->addr.af)
                    break;
            if (idx == w->addr_cnt) {
                warn(CRT, "peer address family not available locally");
                goto fail;
            }
        }
    }

    c->sockopt.enable_ecn = true;
    c->sockopt.enable_udp_zero_checksums =
        get_conf_uncond(c->w, conf, enable_udp_zero_checksums);

    if (is_clnt(c) || peer == 0) {
        c->sock = w_bind(w, idx, port, &c->sockopt);
        if (unlikely(c->sock == 0))
            goto fail;
        c->holds_sock = true;
#ifndef NO_SERVER
        if (peer == 0)
            // remember server socket
            kv_push(struct w_sock *, ped(w)->serv_socks, c->sock);
    } else {
        // find existing server socket
        c->sock = get_local_sock_by_ipnp(
            ped(w),
            &(struct w_sockaddr){.addr = w->ifaddr[idx].addr, .port = port});
        ensure(c->sock, "got serv conn");
#endif
    }

    // init CIDs
    c->next_sid_bidi = is_clnt(c) ? 0 : STRM_FL_SRV;
    c->next_sid_uni = is_clnt(c) ? STRM_FL_UNI : STRM_FL_UNI | STRM_FL_SRV;
    sq_init(&c->txq);
#ifndef NO_MIGRATION
    sq_init(&c->migr_txq);
    splay_init(&c->dcids_by_seq);
    splay_init(&c->scids_by_seq);
#endif

    new_initial_cids(c, dcid, scid);
    if (c->scid == 0)
        c->sock->data = c;

    c->vers = c->vers_initial = get_conf(c->w, conf, version);
    diet_init(&c->clsd_strms);

    // initialize idle timeout
    timeout_setcb(&c->idle_alarm, idle_alarm, c);

    // initialize closing alarm
    timeout_setcb(&c->closing_alarm, enter_closed, c);

    // initialize key flip alarm (XXX also abused for migration)
    timeout_setcb(&c->key_flip_alarm, key_flip_alarm, c);

    // initialize ACK timeout
    timeout_setcb(&c->ack_alarm, ack_alarm, c);

    // initialize recovery state
    init_rec(c);
    if (is_clnt(c))
        c->path_val_win = UINT_T_MAX;
    else
        c->tx_new_tok = true;

    // start a TX watcher
    timeout_init(&c->tx_w, TIMEOUT_ABS);
    timeout_setcb(&c->tx_w, tx, c);

    if (likely(is_clnt(c) || c->holds_sock == false))
        update_conf(c, conf);

    // TODO most of these should become configurable via q_conn_conf
    c->tp_mine.max_pkt = w_max_udp_payload(c->sock);
    c->tp_mine.ack_del_exp = c->tp_peer.ack_del_exp = DEF_ACK_DEL_EXP;
    c->tp_mine.max_ack_del = c->tp_peer.max_ack_del = DEF_MAX_ACK_DEL;
    c->tp_mine.max_strm_data_uni = is_clnt(c) ? INIT_STRM_DATA_UNI : 0;
    c->tp_mine.max_strms_uni = is_clnt(c) ? INIT_MAX_UNI_STREAMS : 0;
    c->tp_mine.max_strms_bidi = INIT_MAX_BIDI_STREAMS;
    c->tp_mine.max_strm_data_bidi_local = c->tp_mine.max_strm_data_bidi_remote =
        is_clnt(c) ? INIT_STRM_DATA_BIDI : INIT_STRM_DATA_BIDI / 2;
    c->tp_mine.max_data =
        c->tp_mine.max_strms_bidi * c->tp_mine.max_strm_data_bidi_local;
    c->tp_mine.act_cid_lim =
        c->tp_mine.disable_active_migration ? 0 : (is_clnt(c) ? 4 : 2);

#if !defined(NO_MIGRATION) && !defined(NO_SERVER)
    // TODO: avoid encoding RFC1918 addresses
    if (!is_clnt(c) && peer && w->have_ip4 && w->have_ip6) {
        // populate tp_mine.pref_addr
        const uint16_t other_af_idx =
            w->ifaddr[idx].addr.af == AF_INET ? 0 : w->addr4_pos;
        struct w_sock * const ws = get_local_sock_by_ipnp(
            ped(w), &(struct w_sockaddr){.addr = w->ifaddr[other_af_idx].addr,
                                         .port = port});

        if (ws) {
            warn(DBG, "other socket is %s%s%s:%u",
                 w->ifaddr[other_af_idx].addr.af == AF_INET6 ? "[" : "",
                 w_ntop(&w->ifaddr[other_af_idx].addr, ip_tmp),
                 w->ifaddr[other_af_idx].addr.af == AF_INET6 ? "]" : "",
                 bswap16(port));

            memcpy(&c->tp_mine.pref_addr.addr4,
                   w->ifaddr[idx].addr.af == AF_INET ? &c->sock->ws_loc
                                                     : &ws->ws_loc,
                   sizeof(c->tp_mine.pref_addr.addr4));
            memcpy(&c->tp_mine.pref_addr.addr6,
                   w->ifaddr[idx].addr.af == AF_INET6 ? &c->sock->ws_loc
                                                      : &ws->ws_loc,
                   sizeof(c->tp_mine.pref_addr.addr6));

            c->max_cid_seq_out = c->tp_mine.pref_addr.cid.seq = 1;
            mk_rand_cid(&c->tp_mine.pref_addr.cid,
                        ped(c->w)->conf.server_cid_len, true);
            add_scid(c, &c->tp_mine.pref_addr.cid);
        }
    }
#endif

    // initialize packet number spaces
    for (pn_t t = pn_init; t <= pn_data; t++)
        init_pn(&c->pns[t], c, t);

    // create crypto streams
    for (epoch_t e = ep_init; e <= ep_data; e++)
        if (e != ep_0rtt)
            new_stream(c, crpt_strm_id(e));

    if (c->scid) {
        // FIXME: first connection sets the type for all future connections
        warn(DBG, "%s conn %s on port %u created", conn_type(c),
             cid_str(c->scid), bswap16(c->sock->ws_lport));
        if (ped(w)->conf.qlog_dir)
            qlog_init(c);
    }

    conn_to_state(c, conn_idle);
    return c;

fail:
    free(c);
    return 0;
}


void free_scid(struct q_conn * const c
#ifdef NO_MIGRATION
               __attribute__((unused))
#endif
               ,
               struct cid * const id)
{
#ifndef NO_MIGRATION
    ensure(splay_remove(cids_by_seq, &c->scids_by_seq, id), "removed");
    cids_by_id_del(&c->scids_by_id, id);
    conns_by_id_del(id);
#endif
    free(id);
}


void free_dcid(struct q_conn * const c
#ifdef NO_MIGRATION
               __attribute__((unused))
#endif
               ,
               struct cid * const id)
{
#ifndef NO_SRT_MATCHING
    if (id->has_srt)
        conns_by_srt_del(id->srt);
#endif
#ifndef NO_MIGRATION
    ensure(splay_remove(cids_by_seq, &c->dcids_by_seq, id), "removed");
#endif
    free(id);
}


void free_conn(struct q_conn * const c)
{
    // exit any active API call on the connection
    maybe_api_return(c, 0);

    stop_all_alarms(c);

    struct q_stream * s;
    kh_foreach_value(&c->strms_by_id, s, { free_stream(s); });
    kh_release(strms_by_id, &c->strms_by_id);

    // free crypto streams
    for (epoch_t e = ep_init; e <= ep_data; e++)
        if (c->cstrms[e])
            free_stream(c->cstrms[e]);

    free_tls(c, false);

    // free packet number spaces
    for (pn_t t = pn_init; t <= pn_data; t++)
        free_pn(&c->pns[t]);

    timeout_del(&c->tx_w);

    diet_free(&c->clsd_strms);

    // remove connection from global lists and free CIDs
    free_cids(c);
#ifndef NO_MIGRATION
    kh_release(cids_by_id, &c->scids_by_id);
#endif

    if (c->holds_sock)
        // only close the socket for the final server connection
        w_close(c->sock);

    if (c->in_c_ready)
        sl_remove(&c_ready, c, q_conn, node_rx_ext);

#ifndef NO_SERVER
    if (c->needs_accept)
        sl_remove(&accept_queue, c, q_conn, node_aq);
#endif

    qlog_close(c);
    free(c);
}


#ifndef NO_QINFO
void conn_info_populate(struct q_conn * const c)
{
    // fill some q_conn_info fields based on other conn fields
    c->i.cwnd = c->rec.cur.cwnd;
    c->i.ssthresh = c->rec.cur.ssthresh;
    c->i.rtt = (float)c->rec.cur.srtt / US_PER_S;
    c->i.rttvar = (float)c->rec.cur.rttvar / US_PER_S;
}
#endif
