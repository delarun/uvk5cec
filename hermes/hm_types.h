/* Hermes Link — shared types & constants (RFC "Hermes Link", qshosfw/hermes)
 * Ported from qshosfw/deltafw-k1-k5v3 (GPL-3.0) to the UV-K5 v1/v2 (DP32G030).
 */
#ifndef HERMES_TYPES_H
#define HERMES_TYPES_H

#ifdef ENABLE_HERMES

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ──── Frame & payload (RFC §3, §5) ────
#define HM_FRAME_SIZE         128   // fixed over-the-air frame
#define HM_DATA_SIZE           96   // RS(128,96) data part
#define HM_PARITY_SIZE         32
#define HM_HEADER_SIZE         24
#define HM_PAYLOAD_SIZE        56
#define HM_INNER_MAC_SIZE       8
#define HM_OUTER_MAC_SIZE       8
#define HM_NODE_ID_SIZE         6   // 48-bit Base40 address
#define HM_PACKET_ID_SIZE       6

// ──── Physical (RFC §1) ────
#define HM_SYNC_WORD     0x2F2A11DBU

// ──── Network / transport ────
#define HM_MAX_TTL             15
#define HM_DEDUP_SLOTS         16
#define HM_NEIGHBOR_SLOTS       6
#define HM_ACK_TIMEOUT_MS    3000   // after end of our TX: ACK = CSMA + key-up + ~1.1 s air
#define HM_MAX_RETRIES          3
#define HM_BACKOFF_MIN_MS      20
#define HM_BACKOFF_MAX_MS     200

// ──── Application ────
#define HM_MSG_MAX_TEXT        64   // GSM-7 septets in 56 bytes
#define HM_MSG_SLOTS            8

typedef enum {
    HM_TYPE_ACK        = 0,
    HM_TYPE_PING       = 1,
    HM_TYPE_MESSAGE    = 2,
    HM_TYPE_TELEMETRY  = 3,
    HM_TYPE_DISCOVERY  = 4,
    HM_TYPE_KEY_RATCHET= 5,
} HermesPacketType_t;

typedef enum {
    HM_ADDR_UNICAST    = 0,
    HM_ADDR_MULTICAST  = 1,
    HM_ADDR_BROADCAST  = 2,
    HM_ADDR_DISCOVER   = 3,
} HermesAddrMode_t;

// ──── Transport header (24 bytes) ────
typedef struct __attribute__((packed)) {
    uint8_t  ctrl0;          // [7:4]=type, [3:0]=ttl
    uint8_t  ctrl1;          // [7:6]=addr_mode, [5]=want_ack, [4:1]=frag_idx, [0]=last_frag
    uint8_t  packet_id[HM_PACKET_ID_SIZE];
    uint8_t  dest[HM_NODE_ID_SIZE];
    uint8_t  src[HM_NODE_ID_SIZE];
    uint8_t  hop_nonce[4];
} HermesHeader_t;

static inline uint8_t HM_HDR_Type(const HermesHeader_t *h)     { return (h->ctrl0 >> 4) & 0x0F; }
static inline uint8_t HM_HDR_TTL(const HermesHeader_t *h)      { return h->ctrl0 & 0x0F; }
static inline uint8_t HM_HDR_AddrMode(const HermesHeader_t *h) { return (h->ctrl1 >> 6) & 0x03; }
static inline bool    HM_HDR_WantAck(const HermesHeader_t *h)  { return (h->ctrl1 >> 5) & 0x01; }

static inline void HM_HDR_SetType(HermesHeader_t *h, uint8_t t)     { h->ctrl0 = (h->ctrl0 & 0x0F) | ((t & 0x0F) << 4); }
static inline void HM_HDR_SetTTL(HermesHeader_t *h, uint8_t ttl)    { h->ctrl0 = (h->ctrl0 & 0xF0) | (ttl & 0x0F); }
static inline void HM_HDR_SetAddrMode(HermesHeader_t *h, uint8_t m) { h->ctrl1 = (h->ctrl1 & 0x3F) | ((m & 0x03) << 6); }
static inline void HM_HDR_SetWantAck(HermesHeader_t *h, bool a)     { h->ctrl1 = (h->ctrl1 & 0xDF) | ((a ? 1 : 0) << 5); }

// ──── Data block: header + payload + MACs = 96 bytes ────
typedef struct __attribute__((packed)) {
    HermesHeader_t header;                       // 24
    uint8_t        payload[HM_PAYLOAD_SIZE];     // 56
    uint8_t        inner_mac[HM_INNER_MAC_SIZE]; // 8
    uint8_t        outer_mac[HM_OUTER_MAC_SIZE]; // 8
} HermesDataBlock_t;

typedef struct __attribute__((packed)) {
    uint8_t raw[HM_FRAME_SIZE];
} HermesFrame_t;

// ──── Stored message (UI) ────
typedef struct {
    uint8_t  addr[HM_NODE_ID_SIZE];      // incoming: src, outgoing: dest
    uint8_t  packet_id[HM_PACKET_ID_SIZE];
    uint8_t  payload[HM_PAYLOAD_SIZE];   // packed GSM-7
    int8_t   rssi;                       // dBm + 0 (clamped), incoming only
    uint8_t  addressing  : 2;
    uint8_t  is_outgoing : 1;
    uint8_t  is_pending  : 1;
    uint8_t  is_acked    : 1;            // unicast: ACKed, broadcast: heard relayed
    uint8_t  is_failed   : 1;
    uint8_t  is_read     : 1;
    uint8_t  is_system   : 1;            // local status line (ping result etc.)
} HermesMessage_t;

// ──── Neighbor entry ────
typedef struct {
    uint8_t  node_id[HM_NODE_ID_SIZE];
    int8_t   rssi;
    uint8_t  battery;
    uint8_t  missed;
    uint8_t  active;
} HermesNeighbor_t;

// ──── Runtime configuration ────
#define HM_F_ENABLED   0x01
#define HM_F_RELAY     0x02
#define HM_F_ACK       0x04
#define HM_F_MUTE      0x08
#define HM_F_BEACON    0x10

typedef struct {
    uint8_t  node_id[HM_NODE_ID_SIZE];
    char     alias[11];      // callsign (U.Info MY CALL), Base40 source of node_id
    char     passcode[11];   // network passphrase (U.Info MESHKEY, "" = open network)
    uint8_t  net_key[32];    // K_net (RFC §2)
    uint8_t  ttl;
    uint8_t  flags;          // HM_F_*
    uint8_t  key_valid;
} HermesConfig_t;

static const uint8_t HM_ADDR_BCAST[HM_NODE_ID_SIZE] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static inline bool HM_IsBroadcast(const uint8_t addr[HM_NODE_ID_SIZE]) {
    return memcmp(addr, HM_ADDR_BCAST, HM_NODE_ID_SIZE) == 0;
}
static inline bool HM_IsOurAddress(const uint8_t addr[HM_NODE_ID_SIZE], const uint8_t our[HM_NODE_ID_SIZE]) {
    return memcmp(addr, our, HM_NODE_ID_SIZE) == 0;
}

#endif // ENABLE_HERMES
#endif // HERMES_TYPES_H
