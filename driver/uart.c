/* Copyright 2025 muzkr https://github.com/muzkr
 * Copyright 2023 Dual Tachyon
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

#include <stdbool.h>
#include "py32f071_ll_bus.h"
#include "py32f071_ll_system.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_usart.h"
#include "driver/uart.h"

// K-plug: PA9 = TX, PA10 = RX
#define USARTx      USART1
#define DMA_CHANNEL LL_DMA_CHANNEL_2

static bool UART_IsLogEnabled;
uint8_t UART_DMA_Buffer[256];

static void UART_ConfigurePins(void)
{
	LL_GPIO_InitTypeDef InitStruct;
	LL_GPIO_StructInit(&InitStruct);
	InitStruct.Pin        = LL_GPIO_PIN_9 | LL_GPIO_PIN_10;
	InitStruct.Mode       = LL_GPIO_MODE_ALTERNATE;
	InitStruct.Alternate  = LL_GPIO_AF1_USART1;
	InitStruct.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
	InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
	InitStruct.Pull       = LL_GPIO_PULL_UP;
	LL_GPIO_Init(GPIOA, &InitStruct);
}

void UART_Init(uint32_t BaudRate)
{
	LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
	LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SYSCFG);
	LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_USART1);

	UART_ConfigurePins();

	// RX: circular DMA into UART_DMA_Buffer
	LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);

	LL_DMA_InitTypeDef DMA_InitStruct;
	LL_DMA_StructInit(&DMA_InitStruct);
	DMA_InitStruct.Direction              = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                   = LL_DMA_MODE_CIRCULAR;
	DMA_InitStruct.PeriphOrM2MSrcAddress  = LL_USART_DMA_GetRegAddr(USARTx);
	DMA_InitStruct.PeriphOrM2MSrcIncMode  = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstAddress  = (uint32_t)UART_DMA_Buffer;
	DMA_InitStruct.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstIncMode  = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.NbData                 = sizeof(UART_DMA_Buffer);
	DMA_InitStruct.Priority               = LL_DMA_PRIORITY_HIGH;
	LL_DMA_Init(DMA1, DMA_CHANNEL, &DMA_InitStruct);

	LL_SYSCFG_SetDMARemap(DMA1, DMA_CHANNEL, LL_SYSCFG_DMA_MAP_USART1_RD);

	LL_APB1_GRP2_ForceReset(LL_APB1_GRP2_PERIPH_USART1);
	LL_APB1_GRP2_ReleaseReset(LL_APB1_GRP2_PERIPH_USART1);

	LL_USART_Disable(USARTx);

	LL_USART_InitTypeDef USART_InitStruct;
	LL_USART_StructInit(&USART_InitStruct);
	USART_InitStruct.BaudRate          = BaudRate;
	USART_InitStruct.TransferDirection = LL_USART_DIRECTION_TX_RX;
	LL_USART_Init(USARTx, &USART_InitStruct);

	LL_USART_EnableDMAReq_RX(USARTx);

	LL_DMA_EnableChannel(DMA1, DMA_CHANNEL);
	LL_USART_Enable(USARTx);
}

void UART_SendByte(uint8_t Byte)
{
	while (!LL_USART_IsActiveFlag_TXE(USARTx))
		;
	LL_USART_TransmitData8(USARTx, Byte);
}

void UART_Send(const void *pBuffer, uint32_t Size)
{
	const uint8_t *pData = (const uint8_t *)pBuffer;

	for (uint32_t i = 0; i < Size; i++)
		UART_SendByte(pData[i]);
}

void UART_LogSend(const void *pBuffer, uint32_t Size)
{
	if (UART_IsLogEnabled)
		UART_Send(pBuffer, Size);
}

uint16_t UART_GetDmaWriteIndex(void)
{
	return (sizeof(UART_DMA_Buffer) - LL_DMA_GetDataLength(DMA1, DMA_CHANNEL)) % sizeof(UART_DMA_Buffer);
}

void UART_ReleaseRxPin(bool PullDown)
{
	LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_10, LL_GPIO_MODE_INPUT);
	LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_10, PullDown ? LL_GPIO_PULL_DOWN : LL_GPIO_PULL_UP);
}

bool UART_IsRxPinHigh(void)
{
	return LL_GPIO_IsInputPinSet(GPIOA, LL_GPIO_PIN_10);
}
