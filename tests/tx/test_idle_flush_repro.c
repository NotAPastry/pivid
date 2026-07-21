/* tests/tx/test_idle_flush_repro.c -- IMP-1 TX idle-flush corruption repro.
 *
 * Replays a captured rpicam read-chunk stream (with per-read timing, from
 * tests/tx/fixtures/) through the REAL encoder_nal_parser_feed, twice:
 *   (1) baseline: no idle-flush.
 *   (2) idle-flush: before feeding a chunk whose gap since the previous chunk
 *       exceeds the threshold, call encoder_nal_parser_finish() while a NAL is
 *       pending -- exactly what the reverted encoder_run idle branch did.
 * Then diffs the emitted NAL sequences. If they differ, the idle-flush emits a
 * NAL the no-flush path never would (a mid-slice split) -- the corruption. */
#define _POSIX_C_SOURCE 200809L
#include "encoder.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- captured NAL sink -------------------------------------------------- */
typedef struct { uint8_t *buf; size_t len; } Nal;
typedef struct { Nal *v; size_t n, cap; } NalList;
static void nl_push(NalList *l, const uint8_t *b, size_t len) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap*2 : 1024; l->v = realloc(l->v, l->cap*sizeof(Nal)); }
    l->v[l->n].buf = malloc(len); memcpy(l->v[l->n].buf, b, len); l->v[l->n].len = len; l->n++;
}
static void sink(const uint8_t *nal, size_t len, void *user) { nl_push((NalList*)user, nal, len); }

/* ---- capture-file reader ------------------------------------------------ */
typedef struct { uint64_t ns; uint32_t len; const uint8_t *data; } Chunk;
static uint64_t rd_u64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }
static uint32_t rd_u32(const uint8_t *p){ uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v; }

int main(int argc, char **argv) {
    const char *path = argc>1 ? argv[1] : "tx/fixtures/pipe.cap";
    uint64_t thresh_ns = (argc>2 ? (uint64_t)atoll(argv[2]) : 8) * 1000000ull;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *raw = malloc(sz); if (fread(raw,1,sz,f)!=(size_t)sz){return 2;} fclose(f);

    Chunk *ch=NULL; size_t nch=0, cap=0; long off=0;
    while (off+12 <= sz) {
        uint64_t ns=rd_u64(raw+off); uint32_t ln=rd_u32(raw+off+8); off+=12;
        if (off+ln > sz) break;
        if (nch==cap){cap=cap?cap*2:256; ch=realloc(ch,cap*sizeof(Chunk));}
        ch[nch].ns=ns; ch[nch].len=ln; ch[nch].data=raw+off; nch++; off+=ln;
    }
    printf("repro: %zu chunks, threshold=%llums\n", nch, (unsigned long long)(thresh_ns/1000000));

    /* (1) baseline: no flush */
    NalList base={0}; Encoder_NalParser p; encoder_nal_parser_init(&p);
    for (size_t i=0;i<nch;i++) encoder_nal_parser_feed(&p, ch[i].data, ch[i].len, sink, &base);
    encoder_nal_parser_finish(&p, sink, &base);

    /* (2) idle-flush replay */
    NalList fl={0}; encoder_nal_parser_init(&p);
    int flushes=0;
    for (size_t i=0;i<nch;i++) {
        if (i>0 && (ch[i].ns - ch[i-1].ns) >= thresh_ns && p.nal_len > 0) {
            encoder_nal_parser_finish(&p, sink, &fl);   /* the reverted idle branch */
            flushes++;
        }
        encoder_nal_parser_feed(&p, ch[i].data, ch[i].len, sink, &fl);
    }
    encoder_nal_parser_finish(&p, sink, &fl);
    printf("repro: idle-flush fired %d times; baseline NALs=%zu flush NALs=%zu\n",
           flushes, base.n, fl.n);

    /* diff */
    int diffs=0; size_t m = base.n < fl.n ? base.n : fl.n;
    for (size_t i=0;i<m;i++) {
        if (base.v[i].len != fl.v[i].len || memcmp(base.v[i].buf, fl.v[i].buf, base.v[i].len)) {
            if (diffs<6)
                printf("  DIFF nal[%zu]: baseline len=%zu (hdr 0x%02x) vs flush len=%zu (hdr 0x%02x)\n",
                       i, base.v[i].len, base.v[i].len>4?base.v[i].buf[4]:0,
                       fl.v[i].len, fl.v[i].len>4?fl.v[i].buf[4]:0);
            diffs++;
        }
    }
    if (base.n != fl.n) printf("  COUNT MISMATCH: %zu vs %zu (extra NALs from splits)\n", base.n, fl.n);
    printf("repro: %d differing NALs%s\n", diffs + (int)(base.n!=fl.n?1:0),
           (diffs==0 && base.n==fl.n) ? " -> IDENTICAL (flush is byte-safe)" :
                                        " -> DIVERGENT (idle-flush corrupts the NAL stream)");
    return (diffs==0 && base.n==fl.n) ? 0 : 1;
}
