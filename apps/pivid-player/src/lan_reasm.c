/* apps/pivid-player/src/lan_reasm.c -- see lan_reasm.h. */
#include "lan_reasm.h"

#include <string.h>

void lan_reasm_init(LanReasm *r, LanReasm_EmitCb emit, void *user) {
    memset(r, 0, sizeof *r);
    r->emit = emit;
    r->user = user;
}

/* true if id a is strictly older than id b in the 16-bit modular id space. */
static int id_older(uint16_t a, uint16_t b) {
    uint16_t d = (uint16_t)(b - a);
    return d != 0 && d < 0x8000u;
}

static size_t slot_nchunks(const LanSlot *s) {
    return ((size_t)s->total_len + LANFRAG_CHUNK_PAYLOAD - 1) / LANFRAG_CHUNK_PAYLOAD;
}

static int slot_complete(const LanSlot *s) {
    size_t nc = slot_nchunks(s);
    uint64_t full = (nc >= 64) ? ~0ull : ((1ull << nc) - 1u);
    return (s->recv_mask & full) == full;
}

/* Contiguous received bytes from offset 0 (the prefix safe to forward). */
static size_t slot_prefix_bytes(const LanSlot *s) {
    size_t nc = slot_nchunks(s);
    size_t pc = 0;
    while (pc < nc && (s->recv_mask & (1ull << pc))) pc++;
    if (pc == nc) return s->total_len;          /* whole NAL contiguous */
    return pc * LANFRAG_CHUNK_PAYLOAD;          /* up to the first gap  */
}

static void flush_one(LanReasm *r, LanSlot *s) {
    size_t pb = slot_prefix_bytes(s);
    int complete = slot_complete(s);
    if (pb > 0) r->emit(s->buf, pb, r->user);   /* pb==0 => first chunk lost: drop */
    if (complete) r->nals_complete++;
    else          r->nals_partial++;
    /* Whole-NAL gap detection: NALs are emitted in ascending nal_id order, so a
     * forward step > 1 means the caster numbered ids we never received at all (a
     * slice NAL lost upstream / on the wire). Ignore a large backward-or-forward
     * jump (caster restart / id rebase). */
    if (r->have_emitted) {
        uint16_t step = (uint16_t)(s->nal_id - r->last_emitted);
        if (step > 1 && step < 0x4000u) r->nal_gaps += (uint64_t)(step - 1);
    }
    r->last_emitted = s->nal_id;
    r->have_emitted = 1;
    s->used = 0;
    s->recv_mask = 0;
}

/* Flush every open slot whose nal_id is older-or-equal to target, ascending. */
static void flush_through(LanReasm *r, uint16_t target) {
    for (;;) {
        LanSlot *oldest = NULL;
        for (int i = 0; i < LAN_REASM_SLOTS; i++) {
            LanSlot *s = &r->slots[i];
            if (!s->used) continue;
            int le = (s->nal_id == target) || id_older(s->nal_id, target);
            if (!le) continue;
            if (!oldest || id_older(s->nal_id, oldest->nal_id)) oldest = s;
        }
        if (!oldest) return;
        flush_one(r, oldest);
    }
}

static LanSlot *find_slot(LanReasm *r, uint16_t nal_id) {
    for (int i = 0; i < LAN_REASM_SLOTS; i++)
        if (r->slots[i].used && r->slots[i].nal_id == nal_id) return &r->slots[i];
    return NULL;
}

/* Get a free slot, evicting (flushing as partial) the oldest open slot if full. */
static LanSlot *open_slot(LanReasm *r, uint16_t nal_id, uint16_t total_len,
                          uint64_t now_ns) {
    LanSlot *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < LAN_REASM_SLOTS; i++) {
        LanSlot *s = &r->slots[i];
        if (!s->used) { free_slot = s; continue; }
        if (!oldest || id_older(s->nal_id, oldest->nal_id)) oldest = s;
    }
    if (!free_slot) {                      /* full -> evict oldest as partial */
        flush_one(r, oldest);
        free_slot = oldest;
    }
    free_slot->used = 1;
    free_slot->nal_id = nal_id;
    free_slot->total_len = total_len;
    free_slot->recv_mask = 0;
    free_slot->last_ns = now_ns;
    return free_slot;
}

void lan_reasm_feed(LanReasm *r, const uint8_t *dg, size_t dg_len, uint64_t now_ns) {
    LanfragHdr h;
    if (lanfrag_parse_hdr(dg, dg_len, &h) != 0) { r->bad_magic++; return; }
    if (h.total_len == 0) { r->nal_degenerate++; return; }
    /* geometry: offsets are multiples of the chunk size; the last chunk is the
     * remainder. Reject anything else (spoofed/corrupt) to keep the bitmap valid. */
    if (h.frag_off % LANFRAG_CHUNK_PAYLOAD != 0) { r->chunks_dropped++; return; }
    if ((size_t)h.frag_off >= h.total_len)       { r->chunks_dropped++; return; }
    size_t expect = (size_t)h.total_len - h.frag_off;
    if (expect > LANFRAG_CHUNK_PAYLOAD) expect = LANFRAG_CHUNK_PAYLOAD;
    if (h.payload_len != expect)                 { r->chunks_dropped++; return; }
    /* Drop a chunk for a RECENTLY-emitted NAL (a late straggler/duplicate within
     * LAN_REASM_STALE_WINDOW ids behind last_emitted). A larger backward jump is a
     * caster restart (nal_id reset to 0), not a straggler -- accept it so the
     * player re-bases on the new stream instead of blacking out. */
    if (r->have_emitted) {
        uint16_t back = (uint16_t)(r->last_emitted - h.nal_id);
        if (back < LAN_REASM_STALE_WINDOW) { r->chunks_dropped++; return; }
    }

    LanSlot *s = find_slot(r, h.nal_id);
    if (s && s->total_len != h.total_len) {       /* same id, conflicting size */
        /* Treat as fresh: clearing recv_mask is enough -- buf is mask-gated, so
         * stale bytes left in buf from the old NAL can never surface (only
         * contiguously-received, mask-set chunks are ever emitted). */
        s->recv_mask = 0; s->total_len = h.total_len;
    }
    if (!s) s = open_slot(r, h.nal_id, h.total_len, now_ns);

    size_t ci = (size_t)h.frag_off / LANFRAG_CHUNK_PAYLOAD;
    if (ci < 64 && !(s->recv_mask & (1ull << ci))) {
        memcpy(s->buf + h.frag_off, h.payload, h.payload_len);
        s->recv_mask |= (1ull << ci);
    }
    s->last_ns = now_ns;
    if (slot_complete(s)) flush_through(r, h.nal_id);   /* emit older + this, asc */
}

void lan_reasm_tick(LanReasm *r, uint64_t now_ns) {
    for (;;) {
        LanSlot *oldest = NULL;
        for (int i = 0; i < LAN_REASM_SLOTS; i++) {
            LanSlot *s = &r->slots[i];
            if (!s->used) continue;
            if (!oldest || id_older(s->nal_id, oldest->nal_id)) oldest = s;
        }
        if (!oldest) return;
        if (now_ns - oldest->last_ns <= LAN_REASM_TIMEOUT_NS) return;  /* newest not stale */
        flush_through(r, oldest->nal_id);
    }
}

void lan_reasm_flush_all(LanReasm *r) {
    for (;;) {
        LanSlot *oldest = NULL;
        for (int i = 0; i < LAN_REASM_SLOTS; i++) {
            LanSlot *s = &r->slots[i];
            if (!s->used) continue;
            if (!oldest || id_older(s->nal_id, oldest->nal_id)) oldest = s;
        }
        if (!oldest) return;
        flush_one(r, oldest);
    }
}
