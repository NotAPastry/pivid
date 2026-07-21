/* Host-side tests for common/fec.c -- pure RS codec, no I/O. */
#include "fec.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int g_failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); g_failures++; } \
} while (0)

/* fec_build_cauchy + fec_mat_invert are internal; expose via test hook decls. */
void fec_build_cauchy(uint8_t *mat, int k, int m);   /* m rows x k cols */
int  fec_mat_invert(uint8_t *a, int n);              /* in-place; -1 if singular */

static void test_gf_identities(void) {
    fec_gf_init();
    EXPECT(fec_gf_mul(0, 5) == 0, "0*x == 0");
    EXPECT(fec_gf_mul(7, 0) == 0, "x*0 == 0");
    EXPECT(fec_gf_mul(1, 123) == 123, "1 is multiplicative identity");
    /* a * inv(a) == 1 for all nonzero a */
    for (int a = 1; a < 256; a++)
        EXPECT(fec_gf_mul((uint8_t)a, fec_gf_inv((uint8_t)a)) == 1, "a * inv(a) == 1");
    /* commutativity + a sample associativity check */
    EXPECT(fec_gf_mul(13, 200) == fec_gf_mul(200, 13), "mul commutes");
    uint8_t l = fec_gf_mul(fec_gf_mul(3, 17), 200);
    uint8_t r = fec_gf_mul(3, fec_gf_mul(17, 200));
    EXPECT(l == r, "mul associative");
}

static void test_matrix(void) {
    fec_gf_init();
    /* Invert a known matrix, multiply back, expect identity. Use the top-left
     * 3x3 of a Cauchy-derived square: build C (m=3,k=3), invert, A*Ainv==I. */
    uint8_t c[9], a[9], ainv[9];
    fec_build_cauchy(c, 3, 3);
    memcpy(a, c, sizeof a);
    memcpy(ainv, c, sizeof ainv);
    EXPECT(fec_mat_invert(ainv, 3) == 0, "cauchy 3x3 invertible");
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            uint8_t acc = 0;
            for (int t = 0; t < 3; t++)
                acc ^= fec_gf_mul(a[i*3+t], ainv[t*3+j]);
            EXPECT(acc == (i == j ? 1 : 0), "A * Ainv == I");
        }
}

static void fill_sym(uint8_t *s, uint8_t seed) {
    for (int i = 0; i < FEC_SYMBOL_LEN; i++) s[i] = (uint8_t)(seed * 31 + i);
}

static void test_encode_shape(void) {
    fec_gf_init();
    enum { K = 5, M = 3 };
    uint8_t d[K][FEC_SYMBOL_LEN], p[M][FEC_SYMBOL_LEN];
    const uint8_t *dp[K]; uint8_t *pp[M];
    for (int i = 0; i < K; i++) { fill_sym(d[i], (uint8_t)(i+1)); dp[i] = d[i]; }
    for (int i = 0; i < M; i++) pp[i] = p[i];
    fec_encode(dp, K, M, pp);
    /* m=0 must be a no-op: it must NOT touch parity_out. Snapshot, call, compare. */
    uint8_t snap[M][FEC_SYMBOL_LEN];
    for (int i = 0; i < M; i++) memcpy(snap[i], p[i], FEC_SYMBOL_LEN);
    fec_encode(dp, K, 0, pp);
    int unchanged = 1;
    for (int i = 0; i < M; i++) if (memcmp(snap[i], p[i], FEC_SYMBOL_LEN) != 0) unchanged = 0;
    EXPECT(unchanged, "m=0 is a no-op: parity_out untouched");
    /* Parity must depend on data: flipping a data byte changes parity. */
    uint8_t p2[M][FEC_SYMBOL_LEN]; uint8_t *pp2[M];
    for (int i = 0; i < M; i++) pp2[i] = p2[i];
    d[2][0] ^= 0xFF;
    fec_encode(dp, K, M, pp2);
    EXPECT(memcmp(p[0], p2[0], FEC_SYMBOL_LEN) != 0, "parity changes with data");
}

/* Encode K data syms, erase a chosen set, decode, assert exact recovery. */
static int roundtrip_erase(int K, int M, const int *erase, int n_erase) {
    int N = K + M;
    uint8_t orig[FEC_K_MAX][FEC_SYMBOL_LEN];
    uint8_t sym[FEC_K_MAX + FEC_M_MAX][FEC_SYMBOL_LEN];
    uint8_t present[FEC_K_MAX + FEC_M_MAX];
    const uint8_t *dp[FEC_K_MAX]; uint8_t *pp[FEC_M_MAX]; uint8_t *sp[FEC_K_MAX+FEC_M_MAX];

    for (int i = 0; i < K; i++) { fill_sym(orig[i], (uint8_t)(i*7+1)); dp[i] = orig[i]; }
    for (int i = 0; i < M; i++) pp[i] = sym[K + i];
    fec_encode(dp, K, M, pp);
    for (int i = 0; i < K; i++) memcpy(sym[i], orig[i], FEC_SYMBOL_LEN);
    for (int i = 0; i < N; i++) { present[i] = 1; sp[i] = sym[i]; }
    for (int e = 0; e < n_erase; e++) { present[erase[e]] = 0; memset(sym[erase[e]], 0xAA, FEC_SYMBOL_LEN); }

    int rc = fec_decode(sp, present, K, N);
    if (n_erase > M) return rc == -1;          /* unrecoverable must be flagged */
    if (rc != 0) return 0;
    for (int i = 0; i < K; i++)
        if (memcmp(sym[i], orig[i], FEC_SYMBOL_LEN) != 0) return 0;
    return 1;
}

static void test_decode(void) {
    fec_gf_init();
    /* Erase every single-symbol and every two-symbol combination for K=6,M=3. */
    for (int a = 0; a < 9; a++) {
        int e1[1] = { a };
        EXPECT(roundtrip_erase(6, 3, e1, 1), "recover 1 erasure");
        for (int b = a+1; b < 9; b++) {
            int e2[2] = { a, b };
            EXPECT(roundtrip_erase(6, 3, e2, 2), "recover 2 erasures");
        }
    }
    /* All 3 parity worth of erasures (=m) must recover; m+1 must fail. */
    int e3[3] = {0, 3, 5}; EXPECT(roundtrip_erase(6, 3, e3, 3), "recover m erasures");
    int e4[4] = {0, 1, 2, 3}; EXPECT(roundtrip_erase(6, 3, e4, 4), "m+1 flagged unrecoverable");
    /* Fast path: zero erasures recovers trivially. */
    EXPECT(roundtrip_erase(6, 3, NULL, 0), "no-erasure fast path");
    /* Larger k near the split bound. */
    int e5[2] = {10, 40}; EXPECT(roundtrip_erase(40, 2, e5, 2), "k=40 m=2 recover");
}

/* Deterministic fuzz (no Date/rand-from-clock; seeded LCG). */
static void test_fuzz(void) {
    uint32_t s = 0xC0FFEEu;
    for (int t = 0; t < 400; t++) {
        s = s * 1664525u + 1013904223u;
        int K = 1 + (int)(s % 60);
        s = s * 1664525u + 1013904223u;
        int M = 1 + (int)(s % 8);
        int erased[8]; int ne = 0;
        for (int i = 0; i < M; i++) {            /* erase up to M random distinct syms */
            s = s * 1664525u + 1013904223u;
            int idx = (int)(s % (uint32_t)(K + M));
            int dup = 0; for (int j = 0; j < ne; j++) if (erased[j] == idx) dup = 1;
            if (!dup) erased[ne++] = idx;
        }
        EXPECT(roundtrip_erase(K, M, erased, ne), "fuzz: <=m erasures recover");
    }
}

int main(void) {
    test_gf_identities();
    test_matrix();
    test_encode_shape();
    test_decode();
    test_fuzz();
    if (g_failures) { fprintf(stderr, "test_fec: %d failure(s)\n", g_failures); return 1; }
    printf("test_fec: OK\n");
    return 0;
}
