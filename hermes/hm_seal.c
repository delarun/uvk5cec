/* Hermes Security — Inner/Outer encryption pipeline (RFC §3, §4)
 *
 * Inner (E2E):  AEAD over Source(6)+Payload(56) → InnerMAC(8)
 *               nonce = PacketID(6) || Dest(6), key = K_scope
 * Outer (hop):  ChaCha20 XOR over bytes 0..87 except HopNonce(20..23),
 *               then Poly1305 OuterMAC over bytes 0..87.
 *               nonce = HopNonce(4) || SyncWord(4, LE) || Freq(4, LE), key = K_net
 *
 * TX: EncryptInner → ObfuscateOuter → CalculateOuterMAC
 * RX: VerifyOuterMAC → DeobfuscateOuter → DecryptInner
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_seal.h"
#include "hermes/hm_crypto_engine.h"
#include "hermes/chacha20.h"
#include <string.h>

#define HM_OUTER_LEN 88

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void outer_nonce(const HermesDataBlock_t *b, uint32_t sync_word, uint32_t freq, uint8_t nonce[12]) {
    memcpy(nonce, b->header.hop_nonce, 4);
    put_le32(nonce + 4, sync_word);
    put_le32(nonce + 8, freq);
}

static void inner_nonce(const HermesDataBlock_t *b, uint8_t nonce[12]) {
    memcpy(nonce, b->header.packet_id, HM_PACKET_ID_SIZE);
    memcpy(nonce + HM_PACKET_ID_SIZE, b->header.dest, HM_NODE_ID_SIZE);
}

// Source and payload are separated by hop_nonce, so gather them first.
static bool inner(HermesDataBlock_t *b, const uint8_t key[32], bool encrypt) {
    uint8_t nonce[12], tmp[62];
    inner_nonce(b, nonce);
    memcpy(tmp, b->header.src, 6);
    memcpy(tmp + 6, b->payload, 56);
    if (encrypt)
        HERMES_Crypto_Encrypt(key, nonce, tmp, sizeof(tmp), b->inner_mac);
    else if (!HERMES_Crypto_Decrypt(key, nonce, tmp, sizeof(tmp), b->inner_mac))
        return false;
    memcpy(b->header.src, tmp, 6);
    memcpy(b->payload, tmp + 6, 56);
    return true;
}

void HERMES_Seal_EncryptInner(HermesDataBlock_t *block, const uint8_t k_scope[32]) {
    inner(block, k_scope, true);
}

bool HERMES_Seal_DecryptInner(HermesDataBlock_t *block, const uint8_t k_scope[32]) {
    return inner(block, k_scope, false);
}

void HERMES_Seal_ObfuscateOuter(HermesDataBlock_t *block, const uint8_t k_net[32],
                                uint32_t sync_word, uint32_t frequency) {
    uint8_t nonce[12], ks[HM_OUTER_LEN];
    outer_nonce(block, sync_word, frequency, nonce);
    memset(ks, 0, sizeof(ks));
    chacha20_ctx ctx;
    chacha20_init(&ctx, k_net, nonce, 0);
    chacha20_encrypt(&ctx, ks, ks, sizeof(ks));
    uint8_t *raw = (uint8_t *)block;
    for (int i = 0; i < HM_OUTER_LEN; i++)
        if (i < 20 || i > 23)
            raw[i] ^= ks[i];
}

void HERMES_Seal_CalculateOuterMAC(HermesDataBlock_t *block, const uint8_t k_net[32],
                                   uint32_t sync_word, uint32_t frequency) {
    uint8_t nonce[12];
    outer_nonce(block, sync_word, frequency, nonce);
    HERMES_Crypto_MAC(k_net, nonce, (const uint8_t *)block, HM_OUTER_LEN, block->outer_mac);
}

bool HERMES_Seal_VerifyOuterMAC(const HermesDataBlock_t *block, const uint8_t k_net[32],
                                uint32_t sync_word, uint32_t frequency) {
    uint8_t nonce[12], mac[8];
    outer_nonce(block, sync_word, frequency, nonce);
    HERMES_Crypto_MAC(k_net, nonce, (const uint8_t *)block, HM_OUTER_LEN, mac);
    return memcmp(mac, block->outer_mac, 8) == 0;
}

#endif // ENABLE_HERMES
