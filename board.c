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


#include <string.h>

#include "board.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_adc.h"
#include "driver/adc.h"
#include "driver/backlight.h"
#if defined(ENABLE_FMRADIO) || defined(ENABLE_CEC_FMRADIO)
	#include "driver/bk1080.h"
#endif
#include "driver/crc.h"
#include "driver/gpio.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"

void BOARD_GPIO_Init(void)
{
	LL_IOP_GRP1_EnableClock(
		LL_IOP_GRP1_PERIPH_GPIOA |
		LL_IOP_GRP1_PERIPH_GPIOB |
		LL_IOP_GRP1_PERIPH_GPIOC |
		LL_IOP_GRP1_PERIPH_GPIOF);

	LL_GPIO_InitTypeDef InitStruct;
	LL_GPIO_StructInit(&InitStruct);
	InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
	InitStruct.Pull       = LL_GPIO_PULL_UP;
	InitStruct.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;

	// ---- inputs

	InitStruct.Mode = LL_GPIO_MODE_INPUT;

	// keypad rows: PB15..PB12
	InitStruct.Pin = LL_GPIO_PIN_15 | LL_GPIO_PIN_14 | LL_GPIO_PIN_13 | LL_GPIO_PIN_12;
	LL_GPIO_Init(GPIOB, &InitStruct);

	// PTT: PB10
	InitStruct.Pin = LL_GPIO_PIN_10;
	LL_GPIO_Init(GPIOB, &InitStruct);

	// ---- outputs

	LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_6);   // LCD A0
	LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_2);   // LCD CS
	LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_3);   // SPI flash CS
	LL_GPIO_SetOutputPin(GPIOF, LL_GPIO_PIN_9);   // BK4819 CS

	InitStruct.Mode = LL_GPIO_MODE_OUTPUT;

	// keypad columns: PB6..PB3
	InitStruct.Pin = LL_GPIO_PIN_6 | LL_GPIO_PIN_5 | LL_GPIO_PIN_4 | LL_GPIO_PIN_3;
	LL_GPIO_Init(GPIOB, &InitStruct);

	// audio PA: PA8, LCD A0: PA6, SPI flash CS: PA3
	InitStruct.Pin = LL_GPIO_PIN_8 | LL_GPIO_PIN_6 | LL_GPIO_PIN_3;
	LL_GPIO_Init(GPIOA, &InitStruct);

	// BK4819 SCK: PB8, BK4819 SDA: PB9, LCD CS: PB2
	InitStruct.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_8 | LL_GPIO_PIN_2;
	LL_GPIO_Init(GPIOB, &InitStruct);

	// flashlight: PC13
	InitStruct.Pin = LL_GPIO_PIN_13;
	LL_GPIO_Init(GPIOC, &InitStruct);

	// BK1080 SCK: PF5, BK1080 SDA: PF6, backlight: PF8, BK4819 CS: PF9
	InitStruct.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_8 | LL_GPIO_PIN_6 | LL_GPIO_PIN_5;
	LL_GPIO_Init(GPIOF, &InitStruct);

#ifndef ENABLE_SWD
	// SWDIO / SWCLK as plain outputs
	InitStruct.Pin = LL_GPIO_PIN_14 | LL_GPIO_PIN_13;
	LL_GPIO_Init(GPIOA, &InitStruct);
#endif
}

void BOARD_ADC_Init(void)
{
	ADC_Init();
}

void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent)
{
	*pVoltage = ADC_ReadChannel(LL_ADC_CHANNEL_8);
	// no charge current sense on the UV-K5 V3 / UV-K1
	*pCurrent = 0;
}

void BOARD_Init(void)
{
	BOARD_GPIO_Init();
	BACKLIGHT_InitHardware();
	BOARD_ADC_Init();
	PY25Q16_Init();
	ST7565_Init();
#ifdef ENABLE_FMRADIO
	BK1080_Init(0, false);
#endif

#if defined(ENABLE_UART) || defined(ENABLE_AIRCOPY)
	CRC_Init();
#endif
}
