/* Hermes Transport — CSMA/CA with binary exponential backoff (RFC §9)
 *
 * The channel must be clear for 5 consecutive 1 ms samples before keying up.
 * Contention window: 100 ms (ACK/critical 25 ms, broadcast 200 ms), doubled
 * after every busy attempt up to 1.6 s, 6 attempts. Waiting blocks the main
 * loop (at most a few seconds).
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_csma.h"
#include "hermes/hm_phy.h"
#include "hermes/hm_trng.h"
#include "driver/system.h"

static bool wait_clear(uint16_t ms) {
    uint8_t clear = 0;
    while (ms--) {
        SYSTEM_DelayMs(1);
        clear = HERMES_PHY_ChannelBusy() ? 0 : clear + 1;
        if (clear >= 5)
            return true;
    }
    return false;
}

bool HERMES_CSMA_Transmit(const uint8_t *frame, uint16_t len, Hermes_Priority_t prio) {
    uint16_t cw = prio == HM_PRIO_CRITICAL ? 25 : prio == HM_PRIO_LOW ? 200 : 100;
    for (uint8_t attempt = 0; attempt < 6; attempt++) {
        if (wait_clear(20))
            return HERMES_PHY_Transmit(frame, len);
        SYSTEM_DelayMs(cw + TRNG_GetU32() % cw);
        if (cw < 1600)
            cw <<= 1;
    }
    return false;
}

#endif // ENABLE_HERMES
