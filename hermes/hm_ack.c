/* Hermes App — ACK Packets (RFC §12.7)
 *
 * Layout:
 * [0..5]   acked_packet_id (6 bytes)
 * [6..13]  acked_inner_mac (8 bytes)
 * [14]     status
 * [15]     flags (HM_ACK_HAS_*)
 * [16..]   optional health blob (battery, lqi, rssi, temperature)
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_ack.h"
#include <string.h>

uint8_t HERMES_ACK_Build(const HermesAck_t *ack, uint8_t out[56]) {
    if (!ack || !out) return 0;
    memset(out, 0, 56);

    memcpy(out, ack->acked_id, 6);
    memcpy(out + 6, ack->acked_mac, 8);
    out[14] = ack->status;
    out[15] = ack->flags;

    uint8_t pos = 16;
    if (ack->flags & HM_ACK_HAS_HEALTH) {
        out[pos++] = ack->battery;
        out[pos++] = ack->lqi;
        out[pos++] = (uint8_t)ack->rssi;
        out[pos++] = ack->temperature;
    }

    return pos;
}


#endif // ENABLE_HERMES
