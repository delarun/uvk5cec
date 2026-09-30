/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#include <stdbool.h>
#include <stdint.h>

// USART1 is clocked directly from PCLK, so UART_Init() takes the baud rate itself
#define UART_BAUD_9600_CLOCK_DIV  9600U
#define UART_BAUD_38400_CLOCK_DIV 38400U
#define UART_BAUD_57600_CLOCK_DIV 57600U
#define UART_BAUD_1152K_CLOCK_DIV 115200U

extern uint8_t UART_DMA_Buffer[256];

void UART_Init(uint32_t BaudRate);
void UART_Send(const void *pBuffer, uint32_t Size);
void UART_SendByte(uint8_t Byte);
void UART_LogSend(const void *pBuffer, uint32_t Size);

// Index in UART_DMA_Buffer the RX DMA will write next
uint16_t UART_GetDmaWriteIndex(void);

// K-plug RX line (PA10) as a plain GPIO input, used by the CW key / soft UART
void UART_ReleaseRxPin(bool PullDown);
bool UART_IsRxPinHigh(void);

#endif
