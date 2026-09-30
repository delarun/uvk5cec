/* Poly1305 (RFC 8439) — compact 26-bit-limb implementation (poly1305-donna-32
 * algorithm, loops instead of unrolled code to save flash on the Cortex-M0).
 */
#include "poly1305.h"
#include <string.h>

static uint32_t le32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void st32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

void poly1305_mac(uint8_t mac[16], const uint8_t *m, size_t n, const uint8_t key[32]) {
    uint32_t r[5], h[5] = {0}, g[5], c;
    static const uint8_t sh[5] = { 0, 2, 4, 6, 8 };
    static const uint32_t rmask[5] = { 0x3ffffff, 0x3ffff03, 0x3ffc0ff, 0x3f03fff, 0x00fffff };

    for (int i = 0; i < 5; i++)
        r[i] = (le32(key + 3 * i) >> sh[i]) & rmask[i];

    while (n) {
        uint8_t blk[17];
        const size_t k = n < 16 ? n : 16;
        memset(blk, 0, sizeof(blk));
        memcpy(blk, m, k);
        blk[k] = 1;
        m += k;
        n -= k;

        for (int i = 0; i < 5; i++)
            h[i] += (le32(blk + 3 * i) >> sh[i]) & 0x3ffffff;
        h[4] += (uint32_t)blk[16] << 24;

        uint64_t d[5];
        for (int i = 0; i < 5; i++) {
            d[i] = 0;
            for (int j = 0; j < 5; j++)
                d[i] += (uint64_t)h[j] * (j <= i ? r[i - j] : r[i - j + 5] * 5);
        }
        c = 0;
        for (int i = 0; i < 5; i++) {
            d[i] += c;
            c    = (uint32_t)(d[i] >> 26);
            h[i] = (uint32_t)d[i] & 0x3ffffff;
        }
        h[0] += c * 5;
        h[1] += h[0] >> 26;
        h[0] &= 0x3ffffff;
    }

    // full carry
    c = 0;
    for (int i = 1; i < 5; i++) {
        h[i] += c;
        c = h[i] >> 26;
        h[i] &= 0x3ffffff;
    }
    h[0] += c * 5;
    h[1] += h[0] >> 26;
    h[0] &= 0x3ffffff;

    // g = h - p; select h if g < 0
    c = 5;
    for (int i = 0; i < 4; i++) {
        g[i] = h[i] + c;
        c = g[i] >> 26;
        g[i] &= 0x3ffffff;
    }
    g[4] = h[4] + c - (1u << 26);
    const uint32_t mask = (g[4] >> 31) - 1;
    for (int i = 0; i < 5; i++)
        h[i] = (h[i] & ~mask) | (g[i] & mask);

    // h mod 2^128 + s
    const uint32_t w[4] = {
        h[0] | (h[1] << 26), (h[1] >> 6) | (h[2] << 20),
        (h[2] >> 12) | (h[3] << 14), (h[3] >> 18) | (h[4] << 8)
    };
    uint64_t f = 0;
    for (int i = 0; i < 4; i++) {
        f += (uint64_t)w[i] + le32(key + 16 + 4 * i);
        st32(mac + 4 * i, (uint32_t)f);
        f >>= 32;
    }
}
