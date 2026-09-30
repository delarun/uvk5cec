/* Hermes Network — controlled flooding (RFC §6.2)
 *
 * 16-slot ring of seen packet IDs for de-duplication. Forwarded frames are
 * delayed by an RSSI-weighted backoff: nodes that heard the packet strongly
 * are close to the sender and wait longest, far nodes relay first.
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_routing.h"
#include "hermes/hm_trng.h"
#include <string.h>

#define HM_FWD_QUEUE_SIZE 2

static struct {
    HermesDataBlock_t block;     // already re-sealed for the next hop
    uint8_t           packet_id[HM_PACKET_ID_SIZE];   // plain ID (header is obfuscated)
    uint32_t          due_ms;
    bool              active;
} fwd_queue[HM_FWD_QUEUE_SIZE];

static uint8_t dedup_cache[HM_DEDUP_SLOTS][HM_PACKET_ID_SIZE];
static uint8_t dedup_head;

void HERMES_Route_Init(void) {
    memset(dedup_cache, 0, sizeof(dedup_cache));
    memset(fwd_queue, 0, sizeof(fwd_queue));
    dedup_head = 0;
}

bool HERMES_Route_IsDuplicate(const uint8_t packet_id[6]) {
    for (uint8_t i = 0; i < HM_DEDUP_SLOTS; i++)
        if (memcmp(dedup_cache[i], packet_id, HM_PACKET_ID_SIZE) == 0)
            return true;
    return false;
}

void HERMES_Route_Record(const uint8_t packet_id[6]) {
    memcpy(dedup_cache[dedup_head], packet_id, HM_PACKET_ID_SIZE);
    dedup_head = (dedup_head + 1) % HM_DEDUP_SLOTS;
}

bool HERMES_Route_ShouldForward(const HermesHeader_t *hdr, const uint8_t our_id[6]) {
    if (HM_HDR_TTL(hdr) <= 1)                          return false;  // would arrive with TTL 0
    if (HM_HDR_Type(hdr) == HM_TYPE_DISCOVERY)         return false;  // single hop
    if (HM_HDR_AddrMode(hdr) == HM_ADDR_UNICAST &&
        HM_IsOurAddress(hdr->dest, our_id))            return false;  // consumed here
    return true;
}

uint16_t HERMES_Route_CalcBackoff(int16_t rssi_dbm) {
    if (rssi_dbm < -130) rssi_dbm = -130;
    if (rssi_dbm > -50)  rssi_dbm = -50;
    // strong signal (near the sender) → long delay; weak (far) → short delay
    uint16_t delay = 300 + (uint16_t)(rssi_dbm + 130) * 20;          // 300..1900 ms
    return delay + (TRNG_GetU32() & 0xFF);                            // + 0..255 ms jitter
}

bool HERMES_Route_QueueForward(const HermesDataBlock_t *block, const uint8_t packet_id[6], uint32_t due_ms) {
    for (uint8_t i = 0; i < HM_FWD_QUEUE_SIZE; i++) {
        if (!fwd_queue[i].active) {
            memcpy(&fwd_queue[i].block, block, sizeof(HermesDataBlock_t));
            memcpy(fwd_queue[i].packet_id, packet_id, HM_PACKET_ID_SIZE);
            fwd_queue[i].due_ms = due_ms;
            fwd_queue[i].active = true;
            return true;
        }
    }
    return false;
}

// Cancel a pending forward when somebody else already relayed it (RFC §6.2 suppression)
void HERMES_Route_Cancel(const uint8_t packet_id[6]) {
    for (uint8_t i = 0; i < HM_FWD_QUEUE_SIZE; i++)
        if (fwd_queue[i].active &&
            memcmp(fwd_queue[i].packet_id, packet_id, HM_PACKET_ID_SIZE) == 0)
            fwd_queue[i].active = false;
}

const HermesDataBlock_t *HERMES_Route_Due(uint32_t now_ms) {
    for (uint8_t i = 0; i < HM_FWD_QUEUE_SIZE; i++) {
        if (fwd_queue[i].active && (int32_t)(now_ms - fwd_queue[i].due_ms) >= 0) {
            fwd_queue[i].active = false;
            return &fwd_queue[i].block;
        }
    }
    return NULL;
}

#endif // ENABLE_HERMES
