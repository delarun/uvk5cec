/* Hermes Physical layer — BK4819 FFSK 1200/1800 modem (RFC §1–§4)
 *
 * 1200 baud, 16-byte preamble (TX), 4-byte sync word 0x2F2A11DB,
 * 128-byte fixed frames, HW CRC off (RS FEC lives in the data-link layer).
 * Register setup follows the proven kamilsss655 / matoz FSK messenger and
 * the deltafw Hermes PHY; TX goes through the stock egzumer TX path
 * (RADIO_PrepareTX → FUNCTION_Transmit) so band TX-lock, power
 * calibration and TOT are honoured.
 */
#ifdef ENABLE_HERMES

#include "hermes/hm_phy.h"
#include "hermes/hm_types.h"
#include "driver/bk4819.h"
#include "driver/system.h"
#include "functions.h"
#include "radio.h"
#include "misc.h"

#define REG_5E ((BK4819_REGISTER_t)0x5E)
#define REG_40 ((BK4819_REGISTER_t)0x40)

#define FSK_RX_WORDS  4     // FIFO almost-full threshold (words)

static void ConfigureFSK(bool rx) {
    // Tone2 as FSK baud-rate generator, gain 96
    BK4819_WriteRegister(BK4819_REG_70, (1u << 7) | 96u);
    // 1200 Hz * 10.32444 (26 MHz XTAL)
    BK4819_WriteRegister(BK4819_REG_72, 0x3065);
    // TX FFSK1200/1800, RX FFSK1200/1800, RX gain, auto preamble, RX BW FFSK, enable
    BK4819_WriteRegister(BK4819_REG_58,
        (1u << 13) | (7u << 10) | ((rx ? 3u : 0u) << 8) | (1u << 1) | (1u << 0));
    BK4819_WriteRegister(BK4819_REG_5A, (uint16_t)(HM_SYNC_WORD >> 16));
    BK4819_WriteRegister(BK4819_REG_5B, (uint16_t)(HM_SYNC_WORD & 0xFFFF));
    BK4819_WriteRegister(BK4819_REG_5C, 0x5625);          // HW CRC off
    if (rx)
        BK4819_WriteRegister(REG_5E, (64u << 3) | FSK_RX_WORDS);
    // REG_5D<15:8> = packet length - 1 (aircopy: 0x4700 = 72 bytes).
    // TX sends exactly one frame; RX expects 2 spare bytes so a late bit
    // clock never truncates the frame (extra bytes are dropped by ReadFIFO).
    BK4819_WriteRegister(BK4819_REG_5D, (uint16_t)((rx ? HM_FRAME_SIZE + 1 : HM_FRAME_SIZE - 1) << 8));
}

static uint16_t reg59(bool rx) {
    // no scramble/invert, preamble 0 (RX) / 15 (TX) bytes, 4-byte sync
    return ((rx ? 0u : 15u) << 4) | (1u << 3);
}

void HERMES_PHY_StartRx(void) {
    ConfigureFSK(true);
    const uint16_t r = reg59(true);
    BK4819_WriteRegister(BK4819_REG_59, (1u << 15) | (1u << 14) | r);  // clear FIFOs
    BK4819_WriteRegister(BK4819_REG_59, (1u << 12) | r);               // RX enable
}

bool HERMES_PHY_ChannelBusy(void) {
    // REG_0C bit1: squelch/carrier detect
    return (BK4819_ReadRegister(BK4819_REG_0C) & (1u << 1)) || gCurrentFunction == FUNCTION_RECEIVE;
}

uint16_t HERMES_PHY_ReadFIFO(uint8_t *buf, uint16_t pos, uint16_t max_len, uint8_t words) {
    for (uint8_t i = 0; i < words; i++) {
        const uint16_t w = BK4819_ReadRegister(BK4819_REG_5F);
        if (pos < max_len) buf[pos++] = w & 0xFF;
        if (pos < max_len) buf[pos++] = w >> 8;
    }
    return pos;
}

bool HERMES_PHY_Transmit(const uint8_t *frame, uint16_t len) {
    if (gCurrentFunction == FUNCTION_TRANSMIT)
        return false;                                   // user is on the air

    RADIO_PrepareTX();                                  // TX lock / battery / TOT checks
    if (gCurrentFunction != FUNCTION_TRANSMIT)
        return false;

    BK4819_EnableTXLink();                              // mic ADC off, TX DSP on

    const uint16_t css  = BK4819_ReadRegister(BK4819_REG_51);
    const uint16_t dev  = BK4819_ReadRegister(REG_40);
    const uint16_t filt = BK4819_ReadRegister(BK4819_REG_2B);

    BK4819_WriteRegister(BK4819_REG_51, 0);             // no CTCSS/DCS under FFSK
    BK4819_WriteRegister(REG_40, (dev & 0xF000) |
        (gCurrentVfo->CHANNEL_BANDWIDTH == BK4819_FILTER_BW_WIDE ? 1300 : 1200));
    BK4819_WriteRegister(BK4819_REG_2B, (1u << 2) | (1u << 0));  // TX HPF + pre-emph off

    ConfigureFSK(false);
    const uint16_t r = reg59(false);
    BK4819_WriteRegister(BK4819_REG_59, (1u << 15) | (1u << 14) | r);
    BK4819_WriteRegister(BK4819_REG_59, r);
    BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_FSK_TX_FINISHED);
    BK4819_WriteRegister(BK4819_REG_02, 0);

    SYSTEM_DelayMs(100);                                // PA settle

    for (uint16_t i = 0; i + 1 < len; i += 2)
        BK4819_WriteRegister(BK4819_REG_5F, frame[i] | ((uint16_t)frame[i + 1] << 8));

    BK4819_WriteRegister(BK4819_REG_59, (1u << 11) | r);          // TX enable

    bool ok = false;
    for (unsigned t = 0; t < 2000 / 5 && !ok; t++) {             // 128 B ≈ 1 s on air
        SYSTEM_DelayMs(5);
        if (BK4819_ReadRegister(BK4819_REG_0C) & 1u) {
            BK4819_WriteRegister(BK4819_REG_02, 0);
            ok = (BK4819_ReadRegister(BK4819_REG_02) & BK4819_REG_02_FSK_TX_FINISHED) != 0;
        }
    }

    SYSTEM_DelayMs(50);

    BK4819_WriteRegister(BK4819_REG_59, r);             // TX off
    BK4819_WriteRegister(REG_40, dev);
    BK4819_WriteRegister(BK4819_REG_2B, filt);
    BK4819_WriteRegister(BK4819_REG_51, css);

    // back to RX: PA off, foreground, FSK RX re-armed via RADIO_SetupRegisters hook
    RADIO_SelectVfos();
    RADIO_SetupRegisters(true);
    return ok;
}

#endif // ENABLE_HERMES
