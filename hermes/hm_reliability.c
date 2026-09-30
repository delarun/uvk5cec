/* Hermes Transport — Stop-and-Wait ARQ (RFC §8.2)
 *
 * One outstanding unicast frame. The frame is re-sent unchanged (same packet
 * ID, same hop nonce) on ACK timeout; after HM_MAX_RETRIES the caller is told
 * that delivery failed.
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_reliability.h"
#include "hermes/hm_csma.h"
#include "hermes/hm_framing.h"
#include <string.h>

extern uint32_t HERMES_NowMs(void);

static struct {
    HermesDataBlock_t block;     // fully sealed block (header is obfuscated)
    uint8_t           packet_id[HM_PACKET_ID_SIZE];
    uint32_t          timeout_at;
    uint8_t           retries;
    bool              active;
} arq;

void HERMES_ARQ_Init(void) {
    memset(&arq, 0, sizeof(arq));
}

static void send(uint32_t sync_word, Hermes_Priority_t prio) {
    HermesFrame_t frame;
    HERMES_Frame_Pack(&arq.block, &frame, sync_word);
    HERMES_CSMA_Transmit(frame.raw, HM_FRAME_SIZE, prio);
}

bool HERMES_ARQ_Send(const HermesDataBlock_t *block, const uint8_t packet_id[6], uint32_t sync_word) {
    if (arq.active) return false;
    memcpy(&arq.block, block, sizeof(HermesDataBlock_t));
    memcpy(arq.packet_id, packet_id, HM_PACKET_ID_SIZE);
    arq.retries = 0;
    arq.active  = true;
    send(sync_word, HM_PRIO_NORMAL);
    arq.timeout_at = HERMES_NowMs() + HM_ACK_TIMEOUT_MS;   // counted from end of TX
    return true;
}

bool HERMES_ARQ_HandleAck(const uint8_t packet_id[6]) {
    if (!arq.active || memcmp(arq.packet_id, packet_id, HM_PACKET_ID_SIZE) != 0)
        return false;
    arq.active = false;
    return true;
}

// returns: 0 nothing, 1 retransmitted, -1 gave up (failed_id filled)
int8_t HERMES_ARQ_Tick(uint32_t now_ms, uint32_t sync_word, uint8_t failed_id[6]) {
    if (!arq.active || (int32_t)(now_ms - arq.timeout_at) < 0)
        return 0;
    if (++arq.retries > HM_MAX_RETRIES) {
        arq.active = false;
        memcpy(failed_id, arq.packet_id, HM_PACKET_ID_SIZE);
        return -1;
    }
    send(sync_word, HM_PRIO_CRITICAL);
    arq.timeout_at = HERMES_NowMs() + HM_ACK_TIMEOUT_MS + (uint32_t)arq.retries * 500;
    return 1;
}

bool HERMES_ARQ_IsPending(void) {
    return arq.active;
}

#endif // ENABLE_HERMES
