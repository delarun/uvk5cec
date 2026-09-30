#ifndef HERMES_RELIABILITY_H
#define HERMES_RELIABILITY_H
#ifdef ENABLE_HERMES
#include <stdint.h>
#include <stdbool.h>
#include "hermes/hm_types.h"
void   HERMES_ARQ_Init(void);
bool   HERMES_ARQ_Send(const HermesDataBlock_t *block, const uint8_t packet_id[6], uint32_t sync_word);
bool   HERMES_ARQ_HandleAck(const uint8_t packet_id[6]);
int8_t HERMES_ARQ_Tick(uint32_t now_ms, uint32_t sync_word, uint8_t failed_id[6]);
bool   HERMES_ARQ_IsPending(void);
#endif
#endif
