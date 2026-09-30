#ifndef HERMES_ROUTING_H
#define HERMES_ROUTING_H
#ifdef ENABLE_HERMES
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hermes/hm_types.h"
void     HERMES_Route_Init(void);
bool     HERMES_Route_IsDuplicate(const uint8_t packet_id[6]);
void     HERMES_Route_Record(const uint8_t packet_id[6]);
bool     HERMES_Route_ShouldForward(const HermesHeader_t *hdr, const uint8_t our_id[6]);
uint16_t HERMES_Route_CalcBackoff(int16_t rssi_dbm);
bool     HERMES_Route_QueueForward(const HermesDataBlock_t *block, const uint8_t packet_id[6], uint32_t due_ms);
void     HERMES_Route_Cancel(const uint8_t packet_id[6]);
const HermesDataBlock_t *HERMES_Route_Due(uint32_t now_ms);
#endif
#endif
