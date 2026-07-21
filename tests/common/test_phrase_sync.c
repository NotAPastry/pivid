#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../common/sha256.h"
#include "../common/phrase_sync.h"

/* FIPS 180-4 known-answer: SHA-256("abc") =
 * ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f2001543 ... */
static void test_sha256_kat_abc(void) {
    uint8_t d[32];
    sha256("abc", 3, d);
    assert(d[0]==0xba && d[1]==0x78 && d[2]==0x16 && d[3]==0xbf);
    assert(d[31]==0xad);
    printf("test_sha256_kat_abc: OK\n");
}

/* Empty-string KAT exercises the padding-only block. SHA-256("") starts e3b0c442. */
static void test_sha256_kat_empty(void) {
    uint8_t d[32];
    sha256("", 0, d);
    assert(d[0]==0xe3 && d[1]==0xb0 && d[2]==0xc4 && d[3]==0x42);
    printf("test_sha256_kat_empty: OK\n");
}

/* FIPS 180-4 56-byte vector exercises the two-block padding boundary (after
 * the 0x80 byte, len > 56, so the length spills into a second block).
 * SHA-256("abcdbcde...nopq") = 248d6a61 ... */
static void test_sha256_kat_two_block(void) {
    uint8_t d[32];
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(msg, 56, d);
    assert(d[0]==0x24 && d[1]==0x8d && d[2]==0x6a && d[3]==0x61);
    printf("test_sha256_kat_two_block: OK\n");
}

/* Derivation = nonzero(first 4 bytes BE of the digest). For "abc" that is
 * 0xba7816bf (already nonzero, no remap). */
static void test_phrase_derivation_known(void) {
    assert(phrase_to_sync_word("abc") == 0xba7816bfu);
    printf("test_phrase_derivation_known: OK\n");
}

/* Determinism + sensitivity: same phrase same word; different phrase differs;
 * NULL/"" fall back to the default phrase (and match each other). */
static void test_phrase_derivation_properties(void) {
    uint32_t a = phrase_to_sync_word("tall-orange-falcon");
    uint32_t b = phrase_to_sync_word("tall-orange-falcon");
    uint32_t c = phrase_to_sync_word("tall-orange-eagle");
    assert(a == b);
    assert(a != c);
    assert(phrase_to_sync_word(NULL) == phrase_to_sync_word(""));
    assert(phrase_to_sync_word("") == phrase_to_sync_word(DEFAULT_BINDING_PHRASE));
    printf("test_phrase_derivation_properties: OK\n");
}

/* The away-from-0 remap is its own pure function because no phrase yields a
 * 0x00000000 prefix in practice. */
static void test_sync_word_nonzero_remap(void) {
    assert(sync_word_nonzero(0u) == 1u);
    assert(sync_word_nonzero(0xDEADBEEFu) == 0xDEADBEEFu);
    assert(sync_word_nonzero(1u) == 1u);
    assert(phrase_to_sync_word("anything") != 0u);  /* invariant: never 0 */
    printf("test_sync_word_nonzero_remap: OK\n");
}

int main(void) {
    test_sha256_kat_abc();
    test_sha256_kat_empty();
    test_sha256_kat_two_block();
    test_phrase_derivation_known();
    test_phrase_derivation_properties();
    test_sync_word_nonzero_remap();
    printf("All phrase_sync tests passed!\n");
    return 0;
}
