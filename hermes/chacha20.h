#ifndef CHACHA20_H
#define CHACHA20_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[16];
} chacha20_ctx;

void chacha20_block(uint32_t *state, uint8_t *keystream);
void chacha20_init(chacha20_ctx *ctx, const uint8_t *key, const uint8_t *nonce, uint32_t counter);
void chacha20_encrypt(chacha20_ctx *ctx, const uint8_t *in, uint8_t *out, size_t len);

#endif // CHACHA20_H
