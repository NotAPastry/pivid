#ifndef COMMON_SHA256_H
#define COMMON_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* One-shot SHA-256. Writes the 32-byte big-endian digest to out[32].
 * Public-domain implementation (no external deps); used only to derive a
 * deterministic radio sync word from the binding phrase, so it is not on any
 * hot path. */
void sha256(const void *data, size_t len, uint8_t out[32]);

#endif
