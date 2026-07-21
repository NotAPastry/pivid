#include "fec.h"
#include <string.h>

/* GF(256), primitive polynomial 0x11d (x^8+x^4+x^3+x^2+1). */
static uint8_t gf_exp[512];   /* doubled so log[a]+log[b] (<=508) never wraps */
static uint8_t gf_log[256];
static int     gf_ready = 0;

void fec_gf_init(void) {
    if (gf_ready) return;
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[(uint8_t)x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_log[0] = 0;            /* log(0) undefined; guarded in callers */
    gf_ready = 1;
}

uint8_t fec_gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp[(int)gf_log[a] + (int)gf_log[b]];   /* index <= 508 */
}

uint8_t fec_gf_inv(uint8_t a) {
    /* a != 0 by contract. inv(a) = g^(255 - log a). */
    return gf_exp[255 - (int)gf_log[a]];
}

/* Cauchy matrix C[i][j] = 1 / (x_i ^ y_j), with x_i = i (0..m-1),
 * y_j = m + j (0..k-1). The two index ranges are disjoint, so x_i ^ y_j != 0
 * and every entry is well-defined; m+k <= n <= 255 keeps all elements < 256. */
void fec_build_cauchy(uint8_t *mat, int k, int m) {
    fec_gf_init();
    for (int i = 0; i < m; i++)
        for (int j = 0; j < k; j++) {
            uint8_t xy = (uint8_t)((uint8_t)i ^ (uint8_t)(m + j));
            mat[i * k + j] = fec_gf_inv(xy);
        }
}

/* In-place Gauss-Jordan inversion of an n x n matrix over GF(256).
 * Returns 0 on success, -1 if singular (never happens for Cauchy submatrices). */
int fec_mat_invert(uint8_t *a, int n) {
    fec_gf_init();
    uint8_t inv[FEC_K_MAX * FEC_K_MAX];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            inv[i*n+j] = (uint8_t)(i == j ? 1 : 0);

    for (int col = 0; col < n; col++) {
        if (a[col*n+col] == 0) {                 /* need a nonzero pivot */
            int sw = -1;
            for (int r = col+1; r < n; r++) if (a[r*n+col]) { sw = r; break; }
            if (sw < 0) return -1;
            for (int j = 0; j < n; j++) {
                uint8_t t = a[col*n+j]; a[col*n+j] = a[sw*n+j]; a[sw*n+j] = t;
                t = inv[col*n+j]; inv[col*n+j] = inv[sw*n+j]; inv[sw*n+j] = t;
            }
        }
        uint8_t pinv = fec_gf_inv(a[col*n+col]);
        for (int j = 0; j < n; j++) {
            a[col*n+j]   = fec_gf_mul(a[col*n+j],   pinv);
            inv[col*n+j] = fec_gf_mul(inv[col*n+j], pinv);
        }
        for (int r = 0; r < n; r++) {
            if (r == col) continue;
            uint8_t f = a[r*n+col];
            if (!f) continue;
            for (int j = 0; j < n; j++) {
                a[r*n+j]   ^= fec_gf_mul(f, a[col*n+j]);
                inv[r*n+j] ^= fec_gf_mul(f, inv[col*n+j]);
            }
        }
    }
    memcpy(a, inv, (size_t)n * (size_t)n);
    return 0;
}

int fec_decode(uint8_t *symbols[], const uint8_t present[], int k, int n) {
    /* Defensive bounds: k/n may be wire-supplied. Never index past the fixed
     * work buffers (A[], cauchy[] are FEC_K_MAX-sized). */
    if (k < 1 || k > FEC_K_MAX || n < k || n > FEC_K_MAX + FEC_M_MAX) return -1;
    fec_gf_init();
    int m = n - k;

    /* Count survivors; bail if fewer than k. */
    int avail = 0;
    for (int i = 0; i < n; i++) if (present[i]) avail++;
    if (avail < k) return -1;

    /* Fast path: all k data symbols present -> nothing to reconstruct. */
    int all_data = 1;
    for (int i = 0; i < k; i++) if (!present[i]) { all_data = 0; break; }
    if (all_data) return 0;

    /* Pick the first k present symbols as the decode basis. Build the k x k
     * matrix A from their encoding-matrix rows: identity row e_i for a data
     * symbol i (<k); Cauchy row (i-k) for a parity symbol. */
    uint8_t cauchy[FEC_M_MAX * FEC_K_MAX];
    if (m > 0) fec_build_cauchy(cauchy, k, m);

    int rows[FEC_K_MAX];
    int nr = 0;
    for (int i = 0; i < n && nr < k; i++) if (present[i]) rows[nr++] = i;

    uint8_t A[FEC_K_MAX * FEC_K_MAX];
    for (int r = 0; r < k; r++) {
        int src = rows[r];
        if (src < k) {                              /* identity row e_src */
            for (int j = 0; j < k; j++) A[r*k + j] = (uint8_t)(j == src ? 1 : 0);
        } else {                                    /* parity row */
            int pr = src - k;
            for (int j = 0; j < k; j++) A[r*k + j] = cauchy[pr*k + j];
        }
    }
    if (fec_mat_invert(A, k) != 0) return -1;       /* Cauchy guarantees success */

    /* For each MISSING data symbol di, D[di] = sum_r Ainv[di][r] * R[r],
     * where R[r] is the survivor symbol at rows[r]. Compute byte-wise. */
    for (int di = 0; di < k; di++) {
        if (present[di]) continue;
        uint8_t *out = symbols[di];
        for (int b = 0; b < FEC_SYMBOL_LEN; b++) {
            uint8_t acc = 0;
            for (int r = 0; r < k; r++)
                acc ^= fec_gf_mul(A[di*k + r], symbols[rows[r]][b]);
            out[b] = acc;
        }
    }
    return 0;
}

void fec_encode(const uint8_t *const data[], int k, int m, uint8_t *parity_out[]) {
    if (m <= 0) return;                       /* m=0: no parity, pure passthrough */
    /* Defensive: the Cauchy build assumes these bounds; never index past the
     * fixed work buffers. Out-of-range params are a caller bug -> no parity. */
    if (k < 1 || k > FEC_K_MAX || m > FEC_M_MAX || k + m > 255) return;
    fec_gf_init();
    uint8_t c[FEC_M_MAX * FEC_K_MAX];
    fec_build_cauchy(c, k, m);
    for (int i = 0; i < m; i++) {
        uint8_t *out = parity_out[i];
        for (int b = 0; b < FEC_SYMBOL_LEN; b++) {
            uint8_t acc = 0;
            for (int j = 0; j < k; j++)
                acc ^= fec_gf_mul(c[i*k + j], data[j][b]);
            out[b] = acc;
        }
    }
}
