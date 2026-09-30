/* Remote SDR-style control over UART (panorama sweep + listen), see tools/REMOTE.md */
#ifndef APP_REMOTE_H
#define APP_REMOTE_H

#ifdef ENABLE_REMOTE

#include <stdbool.h>
#include <stdint.h>

extern volatile bool gRemoteRequest;     // set by a host command, main loop enters REMOTE_Run()

void REMOTE_HandleCommand(const uint8_t *pBuffer);   // from UART_HandleCommand (IRQs off!)
void REMOTE_Run(void);                                // modal loop, returns on STOP/EXIT/timeout
void REMOTE_Open(void);                               // from the keypad: wait for the PC

#endif
#endif
