/* Hermes Network — Base40 addressing (RFC §6.1)
 *
 * 9-character callsigns packed into 48-bit (6-byte, big-endian) node IDs:
 *   val = Σ char[i] * 40^(8-i), alphabet ' ' A-Z 0-9 - / .
 * Byte-wise big-number arithmetic keeps the 64-bit libgcc division out of
 * the Cortex-M0 image.
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_addressing.h"
#include <string.h>

static const char B40_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
#define B40_MAX_CHARS 9

static uint8_t char_to_b40(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    for (uint8_t i = 1; i < 40; i++)
        if (B40_CHARS[i] == c) return i;
    return 0;                                   // unknown → space
}

void HERMES_Addr_Encode(const char *callsign, uint8_t out[6]) {
    memset(out, 0, 6);
    uint8_t len = strlen(callsign);
    for (uint8_t i = 0; i < B40_MAX_CHARS; i++) {
        uint16_t carry = i < len ? char_to_b40(callsign[i]) : 0;   // out = out*40 + digit
        for (int8_t k = 5; k >= 0; k--) {
            carry += (uint16_t)out[k] * 40;
            out[k] = (uint8_t)carry;
            carry >>= 8;
        }
    }
}

void HERMES_Addr_Decode(const uint8_t addr[6], char *out, uint8_t max_len) {
    uint8_t v[6];
    char    buf[B40_MAX_CHARS + 1];
    memcpy(v, addr, 6);
    for (int8_t i = B40_MAX_CHARS - 1; i >= 0; i--) {                // v /= 40, digit = v % 40
        uint16_t rem = 0;
        for (uint8_t k = 0; k < 6; k++) {
            rem   = (rem << 8) | v[k];
            v[k]  = rem / 40;
            rem  %= 40;
        }
        buf[i] = B40_CHARS[rem];
    }
    buf[B40_MAX_CHARS] = '\0';
    for (int8_t i = B40_MAX_CHARS - 1; i >= 0 && buf[i] == ' '; i--)
        buf[i] = '\0';
    strncpy(out, buf, max_len - 1);
    out[max_len - 1] = '\0';
}

#endif // ENABLE_HERMES
