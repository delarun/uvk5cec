/* Hermes Link — core (RX/TX pipelines, ARQ, flooding, discovery, ping)
 *
 * Protocol: https://github.com/qshosfw/hermes (RFC), reference C code from
 * qshosfw/deltafw-k1-k5v3 (GPL-3.0), re-worked for the DP32G030 UV-K5 and the
 * CEC firmware:
 *   - sync word 0x2F2A11DB per RFC (deltafw left it at 0)
 *   - outer MAC verified on the de-whitened, RS-corrected block
 *   - forwarded frames get a new hop nonce, TTL-1, fresh outer seal + MAC
 *   - ACKs go through the full inner/outer pipeline
 *   - duplicate unicast frames addressed to us are re-ACKed (lost ACK case)
 *   - receiver derives the unicast key from header.dest (no per-contact secret)
 *
 * Hermes runs in the background on the current VFO frequency (FM only):
 * put both VFOs / the channel on a simplex mesh frequency.
 */
#ifdef ENABLE_HERMES

#include <string.h>

#include "hermes/hermes.h"
#include "hermes/hm_phy.h"
#include "hermes/hm_framing.h"
#include "hermes/hm_packet.h"
#include "hermes/hm_csma.h"
#include "hermes/hm_reliability.h"
#include "hermes/hm_routing.h"
#include "hermes/hm_seal.h"
#include "hermes/hm_kdf.h"
#include "hermes/hm_messaging.h"
#include "hermes/hm_ack.h"
#include "hermes/hm_discovery.h"
#include "hermes/hm_addressing.h"
#include "hermes/hm_trng.h"

#include "audio.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "helper/battery.h"
#include "app/chFrScanner.h"
#include "external/printf/printf.h"

extern uint32_t millis10(void);

#define BEACON_INTERVAL_MS  (10ul * 60 * 1000)
// Identity & key live in the CEC "U.Info" strings (menu U.Info):
#define EEPROM_MYCALL       (0x0F50 + 170 * 16)   // "MY CALL" → callsign / Base40 node ID
#define EEPROM_MESHKEY      (0x0F50 + 179 * 16)   // "MESHKEY" (was SSTV M1) → network passphrase

typedef struct __attribute__((packed)) {
    uint16_t magic;           // 'HM'
    uint8_t  flags;
    uint8_t  ttl;
    uint8_t  reserved[4];
} HermesEeprom_t;             // one 8-byte EEPROM page

static const uint8_t SALT[16] = { 'H','E','R','M','E','S' };

HermesConfig_t  gHermesConfig;
bool            gHermesActive;
HermesMessage_t gHermesMessages[HM_MSG_SLOTS];
uint8_t         gHermesMsgCount;
bool            gHermesHasNewMessage;

static uint8_t  rx_buf[HM_FRAME_SIZE];
static uint8_t  rx_pos;
static int8_t   rx_rssi;
static volatile bool rx_ready;

static struct {
    uint8_t kind;             // 0 none, 1 text, 3 beacon
    uint8_t dest[HM_NODE_ID_SIZE];
    char    text[HM_MSG_MAX_TEXT + 1];
} req;

static uint32_t beacon_due;

// ═══════════════════════════════════════════════════════════
// Settings
// ═══════════════════════════════════════════════════════════

uint32_t HERMES_NowMs(void) {
    return millis10() * 10;
}

static void read_str(uint16_t addr, char *dst) {
    char raw[10];
    EEPROM_ReadBuffer(addr, raw, 10);
    uint8_t i = 0;
    for (; i < 10 && raw[i] && (uint8_t)raw[i] != 0xFF; i++)
        dst[i] = raw[i];
    dst[i] = '\0';
    while (i > 0 && dst[i - 1] == ' ')          // channel names are space padded
        dst[--i] = '\0';
}

// Re-read MY CALL / MESHKEY (they may have been edited in the U.Info menu).
// The 10 000-round KDF only runs when the passphrase actually changed.
void HERMES_RefreshIdentity(void) {
    char call[11], key[11];
    read_str(EEPROM_MYCALL, call);
    read_str(EEPROM_MESHKEY, key);
    if (call[0] == '\0')
        strcpy(call, "NOCALL");
    if (strcmp(call, gHermesConfig.alias) != 0) {
        strcpy(gHermesConfig.alias, call);
        HERMES_Addr_Encode(call, gHermesConfig.node_id);
    }
    if (strcmp(key, gHermesConfig.passcode) != 0 || !gHermesConfig.key_valid) {
        strcpy(gHermesConfig.passcode, key);
        HERMES_KDF_DeriveNetworkKey(key, SALT, gHermesConfig.net_key);
        gHermesConfig.key_valid = 1;
    }
}

void HERMES_ApplySettings(void) {
    HERMES_RefreshIdentity();
    RADIO_SetupRegisters(true);                 // (dis)arm FSK RX
}

void HERMES_SaveSettings(void) {
    HermesEeprom_t e;
    memset(&e, 0, sizeof(e));
    e.magic = 0x4D48;
    e.flags = gHermesConfig.flags;
    e.ttl   = gHermesConfig.ttl;
    EEPROM_WriteBuffer(HERMES_EEPROM_ADDR, &e);
}

void HERMES_Boot(void) {
    HermesEeprom_t e;
    EEPROM_ReadBuffer(HERMES_EEPROM_ADDR, &e, sizeof(e));
    memset(&gHermesConfig, 0, sizeof(gHermesConfig));
    if (e.magic == 0x4D48) {
        gHermesConfig.flags = e.flags;
        gHermesConfig.ttl   = e.ttl;
    } else {
        gHermesConfig.flags = HM_F_RELAY | HM_F_ACK | HM_F_MUTE | HM_F_BEACON;   // disabled by default
    }
    if (gHermesConfig.ttl < 1 || gHermesConfig.ttl > 7)
        gHermesConfig.ttl = 4;

    HERMES_Route_Init();
    HERMES_ARQ_Init();
    HERMES_DISC_Init();
    HERMES_RefreshIdentity();
    beacon_due = HERMES_NowMs() + 15000 + (TRNG_GetU32() & 0x3FFF);
}

// ═══════════════════════════════════════════════════════════
// Message store
// ═══════════════════════════════════════════════════════════

static HermesMessage_t *msg_push(void) {
    if (gHermesMsgCount >= HM_MSG_SLOTS) {
        memmove(&gHermesMessages[0], &gHermesMessages[1], sizeof(HermesMessage_t) * (HM_MSG_SLOTS - 1));
        gHermesMsgCount = HM_MSG_SLOTS - 1;
    }
    HermesMessage_t *m = &gHermesMessages[gHermesMsgCount++];
    memset(m, 0, sizeof(*m));
    gUpdateDisplay = true;
    return m;
}

static HermesMessage_t *msg_find(const uint8_t packet_id[6]) {
    for (uint8_t i = 0; i < gHermesMsgCount; i++)
        if (gHermesMessages[i].is_outgoing &&
            memcmp(gHermesMessages[i].packet_id, packet_id, HM_PACKET_ID_SIZE) == 0)
            return &gHermesMessages[i];
    return NULL;
}

// ═══════════════════════════════════════════════════════════
// Sealing helpers
// ═══════════════════════════════════════════════════════════

static uint8_t scope_label(uint8_t mode) {
    static const uint8_t labels[4] = { HM_LABEL_UNICAST, HM_LABEL_MULTICAST, HM_LABEL_BROADCAST, HM_LABEL_DISCOVERY };
    return labels[mode & 3];
}

static void traffic_key(const HermesDataBlock_t *b, uint8_t key[32]) {
    HERMES_KDF_DeriveTrafficKey(gHermesConfig.net_key, scope_label(HM_HDR_AddrMode(&b->header)),
                                b->header.dest, NULL, key);
}

static uint32_t tx_freq(void) { return gTxVfo->pTX->Frequency; }
static uint32_t rx_freq(void) { return gRxVfo->pRX->Frequency; }

static void seal_outer(HermesDataBlock_t *b) {
    HERMES_Seal_ObfuscateOuter(b, gHermesConfig.net_key, HM_SYNC_WORD, tx_freq());
    HERMES_Seal_CalculateOuterMAC(b, gHermesConfig.net_key, HM_SYNC_WORD, tx_freq());
}

static void build(HermesDataBlock_t *b, uint8_t type, uint8_t mode, bool want_ack,
                  const uint8_t dest[6], uint8_t ttl) {
    memset(b, 0, sizeof(*b));
    HERMES_Pkt_Build(&b->header, type, mode, ttl, want_ack, dest, gHermesConfig.node_id);
}

// inner encrypt + outer seal; the packet ID is recorded so echoes are recognised
static void seal(HermesDataBlock_t *b) {
    uint8_t key[32];
    HERMES_Route_Record(b->header.packet_id);   // before the outer seal hides the ID
    traffic_key(b, key);
    HERMES_Seal_EncryptInner(b, key);
    seal_outer(b);
}

static bool transmit(const HermesDataBlock_t *b, Hermes_Priority_t prio) {
    HermesFrame_t f;
    HERMES_Frame_Pack(b, &f, HM_SYNC_WORD);
    return HERMES_CSMA_Transmit(f.raw, HM_FRAME_SIZE, prio);
}

static uint8_t battery_percent(void) {
    return (uint8_t)BATTERY_VoltsToPercent(gBatteryVoltageAverage);
}

// ═══════════════════════════════════════════════════════════
// TX
// ═══════════════════════════════════════════════════════════

static void send_text(void) {
    const bool bcast   = HM_IsBroadcast(req.dest);
    const bool want_ack = !bcast && (gHermesConfig.flags & HM_F_ACK);
    HermesDataBlock_t b;
    build(&b, HM_TYPE_MESSAGE, bcast ? HM_ADDR_BROADCAST : HM_ADDR_UNICAST, want_ack, req.dest, gHermesConfig.ttl);
    HERMES_MSG_PackGSM7(req.text, strlen(req.text), b.payload);

    uint8_t pid[HM_PACKET_ID_SIZE];
    memcpy(pid, b.header.packet_id, HM_PACKET_ID_SIZE);
    HermesMessage_t *m = msg_push();
    memcpy(m->addr, req.dest, HM_NODE_ID_SIZE);
    memcpy(m->packet_id, b.header.packet_id, HM_PACKET_ID_SIZE);
    memcpy(m->payload, b.payload, HM_PAYLOAD_SIZE);
    m->addressing  = HM_HDR_AddrMode(&b.header);
    m->is_outgoing = 1;
    m->is_read     = 1;
    m->is_pending  = 1;

    seal(&b);

    if (want_ack) {
        if (!HERMES_ARQ_Send(&b, pid, HM_SYNC_WORD)) {
            m->is_pending = 0;
            m->is_failed  = 1;
        }
    } else {
        const bool ok = transmit(&b, HM_PRIO_LOW);
        m = msg_find(pid);
        if (m) {
            m->is_pending = 0;
            m->is_failed  = !ok;
        }
    }
    gUpdateDisplay = true;
}

static void send_beacon(void) {
    HermesDataBlock_t b;
    build(&b, HM_TYPE_DISCOVERY, HM_ADDR_DISCOVER, false, HM_ADDR_BCAST, 1);
    HERMES_DISC_BuildBeacon(b.payload, gHermesConfig.node_id, gHermesConfig.alias, battery_percent());
    seal(&b);
    transmit(&b, HM_PRIO_LOW);
}

static void send_ack(const HermesDataBlock_t *rx, int8_t rssi) {
    HermesAck_t a;
    memset(&a, 0, sizeof(a));
    memcpy(a.acked_id, rx->header.packet_id, HM_PACKET_ID_SIZE);
    memcpy(a.acked_mac, rx->inner_mac, HM_INNER_MAC_SIZE);
    a.flags   = HM_ACK_HAS_HEALTH;
    a.battery = battery_percent();
    a.rssi    = rssi;

    HermesDataBlock_t b;
    build(&b, HM_TYPE_ACK, HM_ADDR_UNICAST, false, rx->header.src, gHermesConfig.ttl);
    HERMES_ACK_Build(&a, b.payload);
    seal(&b);
    transmit(&b, HM_PRIO_CRITICAL);
}

void HERMES_QueueText(const char *text, const uint8_t dest[HM_NODE_ID_SIZE]) {
    strncpy(req.text, text, HM_MSG_MAX_TEXT);
    req.text[HM_MSG_MAX_TEXT] = '\0';
    memcpy(req.dest, dest, HM_NODE_ID_SIZE);
    req.kind = 1;
}

void HERMES_QueueBeacon(void) {
    req.kind = 3;
}

// ═══════════════════════════════════════════════════════════
// RX
// ═══════════════════════════════════════════════════════════

static void process_frame(const uint8_t *raw, int8_t rssi) {
    HermesDataBlock_t b;
    if (!HERMES_Frame_Unpack((const HermesFrame_t *)raw, &b, HM_SYNC_WORD))
        return;                                               // RS decode failed
    if (!HERMES_Seal_VerifyOuterMAC(&b, gHermesConfig.net_key, HM_SYNC_WORD, rx_freq()))
        return;                                               // other network / corrupted
    HERMES_Seal_DeobfuscateOuter(&b, gHermesConfig.net_key, HM_SYNC_WORD, rx_freq());

    const uint8_t type   = HM_HDR_Type(&b.header);
    const uint8_t mode   = HM_HDR_AddrMode(&b.header);
    const bool    for_us = HM_IsOurAddress(b.header.dest, gHermesConfig.node_id);
    uint8_t key[32];

    if (HERMES_Route_IsDuplicate(b.header.packet_id)) {
        HERMES_Route_Cancel(b.header.packet_id);              // somebody else relayed it: suppress ours
        HermesMessage_t *m = msg_find(b.header.packet_id);
        if (m && m->addressing != HM_ADDR_UNICAST && !m->is_acked) {
            m->is_acked    = 1;                               // our broadcast was relayed
            gUpdateDisplay = true;
        }
        // retransmission of a message to us: our ACK was lost, ACK again
        if (for_us && type == HM_TYPE_MESSAGE && HM_HDR_WantAck(&b.header)) {
            traffic_key(&b, key);
            if (HERMES_Seal_DecryptInner(&b, key))
                send_ack(&b, rssi);
        }
        return;
    }
    HERMES_Route_Record(b.header.packet_id);

    // controlled flooding: header-only decision, payload stays E2E encrypted
    if ((gHermesConfig.flags & HM_F_RELAY) && HERMES_Route_ShouldForward(&b.header, gHermesConfig.node_id)) {
        HermesDataBlock_t f = b;
        HM_HDR_SetTTL(&f.header, HM_HDR_TTL(&f.header) - 1);
        HERMES_Pkt_NewHopNonce(f.header.hop_nonce);
        seal_outer(&f);
        HERMES_Route_QueueForward(&f, b.header.packet_id, HERMES_NowMs() + HERMES_Route_CalcBackoff(rssi));
    }

    if (mode == HM_ADDR_UNICAST && !for_us)
        return;

    traffic_key(&b, key);
    if (!HERMES_Seal_DecryptInner(&b, key))
        return;

    switch (type) {
        case HM_TYPE_MESSAGE: {
            HermesMessage_t *m = msg_push();
            memcpy(m->addr, b.header.src, HM_NODE_ID_SIZE);
            memcpy(m->packet_id, b.header.packet_id, HM_PACKET_ID_SIZE);
            memcpy(m->payload, b.payload, HM_PAYLOAD_SIZE);
            m->addressing = mode;
            m->rssi       = rssi;
            gHermesHasNewMessage = true;
            gUpdateStatus        = true;
            gBeepToPlay          = BEEP_880HZ_60MS_TRIPLE_BEEP;
            if (for_us && HM_HDR_WantAck(&b.header))
                send_ack(&b, rssi);
            break;
        }

        case HM_TYPE_ACK:                                     // payload[0..5] = acked packet ID
            if (HERMES_ARQ_HandleAck(b.payload)) {
                HermesMessage_t *m = msg_find(b.payload);
                if (m) {
                    m->is_pending  = 0;
                    m->is_acked    = 1;
                    gUpdateDisplay = true;
                }
            }
            break;

        case HM_TYPE_DISCOVERY:
            HERMES_DISC_ProcessBeacon(b.payload, rssi);
            gUpdateDisplay = true;
            break;

        default:
            break;
    }
}

uint16_t HERMES_SetupRx(void) {
    const bool arm = (gHermesConfig.flags & HM_F_ENABLED) && gRxVfo->Modulation == MODULATION_FM;
    if (!arm) {
        if (gHermesActive) {                                   // switch the modem off
            BK4819_WriteRegister(BK4819_REG_58, 0);
            BK4819_WriteRegister(BK4819_REG_59, 0);
            BK4819_WriteRegister(BK4819_REG_70, 0);
        }
        gHermesActive = false;
        return 0;
    }
    rx_pos = 0;
    HERMES_PHY_StartRx();
    gHermesActive = true;
    return BK4819_REG_3F_FSK_RX_SYNC | BK4819_REG_3F_FSK_RX_FINISHED | BK4819_REG_3F_FSK_FIFO_ALMOST_FULL;
}

void HERMES_HandleFSKInterrupt(uint16_t bits) {
    if (!gHermesActive)
        return;

    if (bits & BK4819_REG_02_FSK_RX_SYNC) {
        rx_pos  = 0;
        rx_rssi = (int8_t)BK4819_GetRSSI_dBm();
        if (gHermesConfig.flags & HM_F_MUTE)
            AUDIO_AudioPathOff();
    }

    if (bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL)
        rx_pos = HERMES_PHY_ReadFIFO(rx_buf, rx_pos, HM_FRAME_SIZE, 4);

    if (bits & BK4819_REG_02_FSK_RX_FINISHED) {
        if (rx_pos < HM_FRAME_SIZE)
            rx_pos = HERMES_PHY_ReadFIFO(rx_buf, rx_pos, HM_FRAME_SIZE, 4);
        if (rx_pos >= HM_FRAME_SIZE)
            rx_ready = true;
        rx_pos = 0;
        HERMES_PHY_StartRx();
        if ((gHermesConfig.flags & HM_F_MUTE) && gEnableSpeaker)
            AUDIO_AudioPathOn();
    }
}

// ═══════════════════════════════════════════════════════════
// 10 ms tick
// ═══════════════════════════════════════════════════════════

void HERMES_Tick10ms(void) {
    HERMES_UI_Tick10ms();

    if (!gHermesActive)
        return;

    if (rx_ready) {
        uint8_t frame[HM_FRAME_SIZE];
        memcpy(frame, rx_buf, sizeof(frame));
        rx_ready = false;
        process_frame(frame, rx_rssi);
        return;
    }

    // never key up under the user, while scanning, or on a busy channel
    if (gCurrentFunction == FUNCTION_TRANSMIT || gScanStateDir != SCAN_OFF ||
        gCssBackgroundScan || HERMES_PHY_ChannelBusy())
        return;

    const uint32_t now = HERMES_NowMs();

    if (req.kind) {
        const uint8_t kind = req.kind;
        req.kind = 0;
        if (kind == 1) send_text();
        else           send_beacon();
        return;
    }

    const HermesDataBlock_t *fwd = HERMES_Route_Due(now);
    if (fwd) {
        transmit(fwd, HM_PRIO_NORMAL);
        return;
    }

    uint8_t failed[HM_PACKET_ID_SIZE];
    if (HERMES_ARQ_Tick(now, HM_SYNC_WORD, failed) < 0) {
        HermesMessage_t *m = msg_find(failed);
        if (m) {
            m->is_pending  = 0;
            m->is_failed   = 1;
            gUpdateDisplay = true;
        }
        return;
    }

    if ((gHermesConfig.flags & HM_F_BEACON) && (int32_t)(now - beacon_due) >= 0) {
        beacon_due = now + BEACON_INTERVAL_MS + (TRNG_GetU32() & 0x7FFF);
        HERMES_DISC_EvictStale();
        send_beacon();
    }
}

#endif // ENABLE_HERMES
