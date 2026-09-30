/* Hermes Security — AEAD (RFC §11.1): ChaCha20 + Poly1305, MAC truncated to 8 bytes.
 * Poly1305 one-time key = first 32 bytes of the ChaCha20 keystream (counter 0),
 * payload keystream starts at counter 1 (RFC 8439 style, encrypt-then-MAC).
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_crypto_engine.h"
#include "hermes/chacha20.h"
#include "hermes/poly1305.h"
#include <string.h>

void HERMES_Crypto_MAC(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t *data, uint16_t len, uint8_t mac_out[8]) {
    static const uint8_t zeros[32];
    uint8_t poly_key[32];
    chacha20_ctx kctx;
    chacha20_init(&kctx, key, nonce, 0);
    chacha20_encrypt(&kctx, zeros, poly_key, 32);

    uint8_t full[16];
    poly1305_mac(full, data, len, poly_key);
    memcpy(mac_out, full, 8);
}

static void xcrypt(const uint8_t key[32], const uint8_t nonce[12], uint8_t *data, uint16_t len) {
    chacha20_ctx ctx;
    chacha20_init(&ctx, key, nonce, 1);
    chacha20_encrypt(&ctx, data, data, len);
}

void HERMES_Crypto_Encrypt(const uint8_t key[32], const uint8_t nonce[12],
                           uint8_t *data, uint16_t len, uint8_t mac_out[8]) {
    xcrypt(key, nonce, data, len);
    HERMES_Crypto_MAC(key, nonce, data, len, mac_out);
}

bool HERMES_Crypto_Decrypt(const uint8_t key[32], const uint8_t nonce[12],
                           uint8_t *data, uint16_t len, const uint8_t mac[8]) {
    uint8_t calc[8];
    HERMES_Crypto_MAC(key, nonce, data, len, calc);
    if (memcmp(calc, mac, 8) != 0)
        return false;
    xcrypt(key, nonce, data, len);
    return true;
}

#endif // ENABLE_HERMES
