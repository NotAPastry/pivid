#include "phrase_sync.h"
#include "sha256.h"
#include <string.h>

uint32_t phrase_to_sync_word(const char *phrase) {
    if (!phrase || phrase[0] == '\0') phrase = DEFAULT_BINDING_PHRASE;
    uint8_t d[32];
    sha256(phrase, strlen(phrase), d);
    uint32_t w = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                 ((uint32_t)d[2] << 8) | (uint32_t)d[3];
    return sync_word_nonzero(w);
}
