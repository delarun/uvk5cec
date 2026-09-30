/* Hermes Security — Key Derivation (RFC §2, §5)
 *
 * K_net   = KDF(passcode, salt)  — 10 000 ChaCha20 iterations
 * K_scope = PRF(K_net, Label || Dest || Secret)
 * Labels: 'U' unicast, 'M' multicast, 'B' broadcast, 'D' discovery.
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_kdf.h"
#include "hermes/chacha20.h"
#include <string.h>

void HERMES_KDF_DeriveNetworkKey(const char *passcode, const uint8_t salt[16], uint8_t out[32]) {
    memset(out, 0, 32);
    if (!passcode || passcode[0] == '\0')
        return;                                 // open network: K_net = 0

    uint8_t work[32];
    memset(work, 0, 32);
    for (int i = 0; i < 16; i++)
        work[i] ^= salt[i];
    const char *p = passcode;
    for (int i = 0; *p && i < 32; i++, p++)
        work[16 + (i % 16)] ^= (uint8_t)*p;

    uint8_t nonce[12];
    memcpy(nonce, salt, 12);

    static const uint8_t zero[32];
    for (uint16_t i = 0; i < 10000; i++) {
        chacha20_ctx ctx;
        chacha20_init(&ctx, work, nonce, 0);
        chacha20_encrypt(&ctx, zero, work, 32);
    }
    memcpy(out, work, 32);
}

void HERMES_KDF_DeriveTrafficKey(const uint8_t k_net[32], uint8_t label,
                                 const uint8_t dest[6], const uint8_t *secret,
                                 uint8_t out[32]) {
    static const uint8_t NULL_SECRET[32] = {0};
    const uint8_t *sec = secret ? secret : NULL_SECRET;

    uint8_t nonce[12];
    nonce[0] = label;
    memcpy(nonce + 1, dest, 6);
    memcpy(nonce + 7, sec, 5);

    uint8_t temp[32] = {0};
    chacha20_ctx ctx;
    chacha20_init(&ctx, k_net, nonce, 0);
    chacha20_encrypt(&ctx, temp, temp, 32);

    for (int i = 0; i < 27; i++)
        temp[i] ^= sec[5 + i];

    memset(out, 0, 32);
    chacha20_init(&ctx, temp, nonce, 0);
    chacha20_encrypt(&ctx, out, out, 32);
}

#endif // ENABLE_HERMES
