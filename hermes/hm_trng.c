/* Hermes — random source for packet IDs / hop nonces / CSMA backoff.
 * DP32G030 has no TRNG: a ChaCha20-keyed state is continuously re-mixed with
 * BK4819 noise/RSSI/glitch readings and SysTick jitter. Uniqueness is what the
 * protocol needs (nonces); the stream is also unpredictable enough for backoff.
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_trng.h"
#include "hermes/chacha20.h"
#include "driver/bk4819.h"
#include "ARMCM0.h"
#include <string.h>

static uint8_t  rng_key[32];
static uint32_t rng_ctr;

static uint32_t entropy(void) {
    return ((uint32_t)BK4819_ReadRegister(BK4819_REG_65) << 16)
         ^ ((uint32_t)BK4819_ReadRegister(BK4819_REG_67) << 7)
         ^ BK4819_ReadRegister(BK4819_REG_63)
         ^ SysTick->VAL;
}

void TRNG_Fill(uint8_t *out, uint8_t len) {
    uint8_t nonce[12] = {0};
    uint8_t ks[64];
    uint32_t e = entropy();
    memcpy(nonce, &e, 4);
    memcpy(nonce + 4, &rng_ctr, 4);
    rng_ctr++;

    chacha20_ctx ctx;
    chacha20_init(&ctx, rng_key, nonce, 0);
    memset(ks, 0, sizeof(ks));
    chacha20_encrypt(&ctx, ks, ks, sizeof(ks));
    memcpy(rng_key, ks, 32);            // fast key erasure / ratchet
    if (len > 32) len = 32;
    memcpy(out, ks + 32, len);
}

uint32_t TRNG_GetU32(void) {
    uint32_t v;
    TRNG_Fill((uint8_t *)&v, 4);
    return v;
}

#endif // ENABLE_HERMES
