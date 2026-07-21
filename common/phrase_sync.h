#ifndef COMMON_PHRASE_SYNC_H
#define COMMON_PHRASE_SYNC_H

#include <stdint.h>

/* Map a raw 32-bit candidate to a usable sync word: any value but 0. We avoid
 * 0 so a derived word is always distinguishable from a zeroed/uninitialized
 * field (the bind path used the same convention). Pure + separately testable
 * (we can never feed phrase_to_sync_word a phrase whose digest starts 0x00000000). */
static inline uint32_t sync_word_nonzero(uint32_t w) { return w ? w : 1u; }

/* ELRS-style derivation: sync_word = nonzero(first 4 bytes, big-endian, of
 * SHA-256(phrase)). Identical phrase on both ends -> identical word.
 * NULL or "" derives from the default phrase so a fresh unit still pairs. */
uint32_t phrase_to_sync_word(const char *phrase);

#define DEFAULT_BINDING_PHRASE "pivid-default"

#endif
