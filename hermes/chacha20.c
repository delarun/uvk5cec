/* ChaCha20 (RFC 8439) — taken from qshosfw/deltafw-k1-k5v3 src/helper (GPL-3.0) */
#include "chacha20.h"
#include <string.h>

// Inline rotation (efficient on ARM)
static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static inline void store32_le(uint8_t *d, uint32_t v) {
    d[0] = v; d[1] = v >> 8; d[2] = v >> 16; d[3] = v >> 24;
}

static inline uint32_t load32_le(const uint8_t *s) {
    return s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24);
}

// Helper: Quarter Round
static void chacha20_quarter_round(uint32_t *x, int a, int b, int c, int d) {
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 16);
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 12);
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 8);
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 7);
}

void chacha20_block(uint32_t *state, uint8_t *keystream) {
    uint32_t x[16];
    memcpy(x, state, 64);

    for (int i = 0; i < 10; i++) {
        // Column rounds
        chacha20_quarter_round(x, 0, 4, 8, 12);
        chacha20_quarter_round(x, 1, 5, 9, 13);
        chacha20_quarter_round(x, 2, 6, 10, 14);
        chacha20_quarter_round(x, 3, 7, 11, 15);
        // Diagonal rounds
        chacha20_quarter_round(x, 0, 5, 10, 15);
        chacha20_quarter_round(x, 1, 6, 11, 12);
        chacha20_quarter_round(x, 2, 7, 8, 13);
        chacha20_quarter_round(x, 3, 4, 9, 14);
    }

    for (int i = 0; i < 16; i++) {
        uint32_t result = x[i] + state[i];
        store32_le(keystream + (i * 4), result);
    }
}

void chacha20_init(chacha20_ctx *ctx, const uint8_t *key, const uint8_t *nonce, uint32_t counter) {
    // RFC 8439 Constants "expand 32-byte k"
    ctx->state[0] = 0x61707865;
    ctx->state[1] = 0x3320646e;
    ctx->state[2] = 0x79622d32;
    ctx->state[3] = 0x6b206574;
    
    for (int i = 0; i < 8; i++) {
        ctx->state[4 + i] = load32_le(key + (i * 4));
    }
    
    ctx->state[12] = counter;
    
    for (int i = 0; i < 3; i++) {
        ctx->state[13 + i] = load32_le(nonce + (i * 4));
    }
}

static void secure_memzero(void *v, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)v;
    while (n--) *p++ = 0;
}

void chacha20_encrypt(chacha20_ctx *ctx, const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t block[64];
    
    while (len > 0) {
        chacha20_block(ctx->state, block);
        ctx->state[12]++; // Increment counter

        size_t bytes = (len < 64) ? len : 64;
        
        for (size_t i = 0; i < bytes; i++) {
            out[i] = in[i] ^ block[i];
        }
        
        len -= bytes;
        in  += bytes;
        out += bytes;
    }
    secure_memzero(block, 64);
}
