#ifndef HERMES_PHY_H
#define HERMES_PHY_H
#ifdef ENABLE_HERMES
#include <stdint.h>
#include <stdbool.h>
void     HERMES_PHY_StartRx(void);
bool     HERMES_PHY_Transmit(const uint8_t *frame, uint16_t len);
uint16_t HERMES_PHY_ReadFIFO(uint8_t *buf, uint16_t pos, uint16_t max_len, uint8_t words);
bool     HERMES_PHY_ChannelBusy(void);
#endif
#endif
