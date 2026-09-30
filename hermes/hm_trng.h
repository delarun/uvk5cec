#ifndef HERMES_TRNG_H
#define HERMES_TRNG_H
#ifdef ENABLE_HERMES
#include <stdint.h>
uint32_t TRNG_GetU32(void);
void     TRNG_Fill(uint8_t *out, uint8_t len);
#endif
#endif
