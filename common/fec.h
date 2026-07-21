#ifndef COMMON_FEC_H
#define COMMON_FEC_H

#include <stdint.h>
#include <stddef.h>

/* Fixed RS symbol length, sized so a count=2 pair exactly fills the 511 B FLRC
 * ceiling: outer FIXED hdr(3) + FEC pair sub-header(4) + 2*S = 3+4+2*252 = 511.
 * A count=1 tail packet is then 3+4+252 = 259 B. */
#define FEC_SYMBOL_LEN  252
/* TX caps data symbols per RS block here; larger frames split (n=k+m<=255). */
#define FEC_K_MAX       200
/* Largest parity count we ever configure. n=k+m<=255 still bounds runtime. */
#define FEC_M_MAX       32
/* On-wire FEC pair sub-header lives in protocol.h (FEC_HDR_BYTES, the
 * proto_fec_pair_hdr_* packers). fec.h owns only the RS algorithm + sizes. */

/* --- GF(256) arithmetic (primitive poly 0x11d). Tables built once. ------ */
void    fec_gf_init(void);          /* idempotent; safe to call repeatedly */
uint8_t fec_gf_mul(uint8_t a, uint8_t b);
uint8_t fec_gf_inv(uint8_t a);      /* a != 0 */

/* Systematic RS encode. data[0..k-1] and parity_out[0..m-1] each point to
 * FEC_SYMBOL_LEN bytes. Data symbols are the plaintext (idx 0..k-1); this
 * fills the m parity symbols. Requires k>=1, m>=0, k+m<=255. */
void fec_encode(const uint8_t *const data[], int k, int m, uint8_t *parity_out[]);

/* Erasure decode. symbols[0..n-1] point to FEC_SYMBOL_LEN bytes (idx 0..k-1
 * are data, k..n-1 are parity); present[i]!=0 means symbol i arrived intact.
 * Missing data symbols (present[i]==0, i<k) are reconstructed in place.
 * Returns 0 if at least k symbols are present (data recoverable), else -1.
 * Buffers for missing symbols may hold garbage on entry; they are overwritten. */
int fec_decode(uint8_t *symbols[], const uint8_t present[], int k, int n);

#endif
