#ifndef POLY1305_H
#define POLY1305_H
#include <stdint.h>
#include <stddef.h>
// One-shot Poly1305 (RFC 8439): mac = Poly1305(key, m)
void poly1305_mac(uint8_t mac[16], const uint8_t *m, size_t len, const uint8_t key[32]);
#endif
