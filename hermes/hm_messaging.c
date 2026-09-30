/* Hermes App — GSM-7 text packing (RFC §12.3)
 *
 * 7-bit GSM 03.38 septets, LSB-first bitstream: up to 64 characters in the
 * 56-byte payload. Only the basic table is supported (the keypad cannot enter
 * extension-table characters); received escapes are shown as '?'.
 * (deltafw used an 8-bit bit counter here, which wrapped after 36 characters.)
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_messaging.h"
#include <string.h>
#include <stdbool.h>

static const char gsm7_basic[128] =
    "@\xa3$\xa5\xe8\xe9\xf9\xec\xf2\xc7\n\xd8\xf8\r\xc5\xe5"
    "\x80_\x81\x82\x83\x84\x85\x86\x87\x88\x89\x1b\xc6\xe6\xdf\xc9"
    " !\"#\xa4%&'()*+,-./"
    "0123456789:;<=>?"
    "\xa1" "ABCDEFGHIJKLMNO"
    "PQRSTUVWXYZ\xc4\xd6\xd1\xdc\xa7"
    "\xbf" "abcdefghijklmno"
    "pqrstuvwxyz\xe4\xf6\xf1\xfc\xe0";

static uint8_t FindGSM7(char c) {
    for (uint8_t i = 0; i < 128; i++)
        if (gsm7_basic[i] == c) return i;
    return 0x20;                                        // unknown → space
}

uint8_t HERMES_MSG_PackGSM7(const char *text, uint8_t text_len, uint8_t *out) {
    uint16_t bit = 0;
    memset(out, 0, HM_PAYLOAD_SIZE);
    for (uint8_t i = 0; i < text_len && text[i] && bit + 7 <= HM_PAYLOAD_SIZE * 8; i++) {
        const uint8_t code = FindGSM7(text[i]);
        for (uint8_t b = 0; b < 7; b++, bit++)
            if (code & (1u << b))
                out[bit / 8] |= 1u << (bit % 8);
    }
    return (bit + 7) / 8;
}

uint8_t HERMES_MSG_UnpackGSM7(const uint8_t *packed, uint8_t packed_len, char *out, uint8_t max_out) {
    uint8_t  chars = 0;
    uint16_t bit   = 0;
    bool     esc   = false;
    while (chars + 1 < max_out && bit + 7 <= (uint16_t)packed_len * 8) {
        uint8_t code = 0;
        for (uint8_t b = 0; b < 7; b++, bit++)
            if (packed[bit / 8] & (1u << (bit % 8)))
                code |= 1u << b;
        if (code == 0x1B && !esc) { esc = true; continue; }
        out[chars++] = esc ? '?' : gsm7_basic[code];
        esc = false;
    }
    while (chars && out[chars - 1] == '@')              // 7-in-8 padding septets
        chars--;
    out[chars] = '\0';
    return chars;
}

#endif // ENABLE_HERMES
