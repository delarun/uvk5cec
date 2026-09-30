/* Hermes Link mesh messenger for the CEC UV-K5 firmware.
 * Protocol: https://github.com/qshosfw/hermes (GPL-3.0)
 */
#ifndef HERMES_H
#define HERMES_H

#ifdef ENABLE_HERMES

#include <stdbool.h>
#include <stdint.h>
#include "driver/keyboard.h"
#include "hermes/hm_types.h"

#define HERMES_EEPROM_ADDR  0x1F90      // 8 bytes (CEC_EEPROM_START4 area, unused by CEC)

extern HermesConfig_t  gHermesConfig;
extern bool            gHermesActive;         // FSK RX armed on the current VFO
extern HermesMessage_t gHermesMessages[HM_MSG_SLOTS];
extern uint8_t         gHermesMsgCount;
extern bool            gHermesHasNewMessage;

// core
void     HERMES_Boot(void);                   // load settings, derive keys (call once at boot)
void     HERMES_SaveSettings(void);
void     HERMES_ApplySettings(void);          // re-derive keys/ID and re-arm radio
void     HERMES_RefreshIdentity(void);        // re-read MY CALL / MESHKEY from U.Info
uint16_t HERMES_SetupRx(void);                // from RADIO_SetupRegisters(): arm FSK RX, returns REG_3F bits
void     HERMES_HandleFSKInterrupt(uint16_t interrupt_bits);
void     HERMES_Tick10ms(void);
uint32_t HERMES_NowMs(void);

// requests from the UI (executed from the 10 ms tick)
void     HERMES_QueueText(const char *text, const uint8_t dest[HM_NODE_ID_SIZE]);
void     HERMES_QueueBeacon(void);

// UI
void     HERMES_Open(void);
void     HERMES_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);
void     UI_DisplayHermes(void);
void     HERMES_UI_Tick10ms(void);

#endif // ENABLE_HERMES
#endif // HERMES_H
