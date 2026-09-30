#ifndef HERMES_ADDRESSING_H
#define HERMES_ADDRESSING_H
#ifdef ENABLE_HERMES
#include <stdint.h>
void HERMES_Addr_Encode(const char *callsign, uint8_t out[6]);
void HERMES_Addr_Decode(const uint8_t addr[6], char *out, uint8_t max_len);
#endif
#endif
