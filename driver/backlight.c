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

#include "backlight.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_system.h"
#include "py32f071_ll_tim.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "settings.h"
#include "misc.h"

// The backlight (PF8) is not on a timer output: TIM7 update events drive a
// circular DMA that writes a duty cycle pattern into GPIOF->BSRR.

#define PWM_FREQ             240
#define DUTY_CYCLE_LEVELS    64

#define DUTY_CYCLE_ON_VALUE  GPIO_PIN_MASK(GPIO_PIN_BACKLIGHT)
#define DUTY_CYCLE_OFF_VALUE (DUTY_CYCLE_ON_VALUE << 16)

#define TIMx        TIM7
#define DMA_CHANNEL LL_DMA_CHANNEL_7

static uint32_t dutyCycle[DUTY_CYCLE_LEVELS];

// brightness 0..10 -> 0..255
static const uint8_t brightnessValue[] = {0, 3, 6, 9, 15, 24, 38, 62, 100, 159, 255};

// this is decremented once every 500ms
uint16_t gBacklightCountdown_500ms = 0;
bool backlightOn;

void BACKLIGHT_InitHardware()
{
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM7);
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

	LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_TIM7);
	LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_TIM7);

	// 48 MHz / ((1 + PSC) * (1 + ARR)) == PWM_FREQ * DUTY_CYCLE_LEVELS
	LL_TIM_SetPrescaler(TIMx, 0);
	LL_TIM_SetAutoReload(TIMx, SystemCoreClock / PWM_FREQ / DUTY_CYCLE_LEVELS - 1);
	LL_TIM_EnableARRPreload(TIMx);
	LL_TIM_EnableDMAReq_UPDATE(TIMx);
	LL_TIM_EnableUpdateEvent(TIMx);

	LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);
	LL_SYSCFG_SetDMARemap(DMA1, DMA_CHANNEL, LL_SYSCFG_DMA_MAP_TIM7_UP);

	LL_DMA_ConfigTransfer(DMA1, DMA_CHANNEL,
		LL_DMA_DIRECTION_MEMORY_TO_PERIPH |
		LL_DMA_MODE_CIRCULAR              |
		LL_DMA_PERIPH_NOINCREMENT         |
		LL_DMA_MEMORY_INCREMENT           |
		LL_DMA_PDATAALIGN_WORD            |
		LL_DMA_MDATAALIGN_WORD            |
		LL_DMA_PRIORITY_HIGH);

	LL_DMA_SetMemoryAddress(DMA1, DMA_CHANNEL, (uint32_t)dutyCycle);
	LL_DMA_SetPeriphAddress(DMA1, DMA_CHANNEL, (uint32_t)(&GPIO_PORT(GPIO_PIN_BACKLIGHT)->BSRR));
	LL_DMA_SetDataLength(DMA1, DMA_CHANNEL, DUTY_CYCLE_LEVELS);
}

void BACKLIGHT_TurnOn(void)
{
	if (gEeprom.BACKLIGHT_TIME == 0) {
		BACKLIGHT_TurnOff();
		return;
	}

	backlightOn = true;
	BACKLIGHT_SetBrightness(gEeprom.BACKLIGHT_MAX);

	switch (gEeprom.BACKLIGHT_TIME) {
		default:
		case 1:	// 5 sec
		case 2:	// 10 sec
		case 3:	// 20 sec
			gBacklightCountdown_500ms = 1 + (2 << (gEeprom.BACKLIGHT_TIME - 1)) * 5;
			break;
		case 4:	// 1 min
		case 5:	// 2 min
		case 6:	// 4 min
			gBacklightCountdown_500ms = 1 + (2 << (gEeprom.BACKLIGHT_TIME - 4)) * 60;
			break;
		case 7:	// always on
			gBacklightCountdown_500ms = 0;
			break;
	}
}

void BACKLIGHT_TurnOff()
{
#ifdef ENABLE_BLMIN_TMP_OFF
	register uint8_t tmp;

	if (gEeprom.BACKLIGHT_MIN_STAT == BLMIN_STAT_ON)
		tmp = gEeprom.BACKLIGHT_MIN;
	else
		tmp = 0;

	BACKLIGHT_SetBrightness(tmp);
#else
	BACKLIGHT_SetBrightness(gEeprom.BACKLIGHT_MIN);
#endif
	gBacklightCountdown_500ms = 0;
	backlightOn = false;
}

bool BACKLIGHT_IsOn()
{
	return backlightOn;
}

static uint8_t currentBrightness;

void BACKLIGHT_SetBrightness(uint8_t brigtness)
{
	if (brigtness >= ARRAY_SIZE(brightnessValue))
		brigtness = ARRAY_SIZE(brightnessValue) - 1;

	currentBrightness = brigtness;

	const uint32_t level = (uint32_t)brightnessValue[brigtness] * DUTY_CYCLE_LEVELS / 255;

	if (level == 0 || level >= DUTY_CYCLE_LEVELS)
	{
		LL_TIM_DisableCounter(TIMx);
		LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);
		SYSTICK_DelayUs(1);
		if (level == 0)
			GPIO_ResetOutputPin(GPIO_PIN_BACKLIGHT);
		else
			GPIO_SetOutputPin(GPIO_PIN_BACKLIGHT);
		return;
	}

	for (uint32_t i = 0; i < DUTY_CYCLE_LEVELS; i++)
		dutyCycle[i] = i < level ? DUTY_CYCLE_ON_VALUE : DUTY_CYCLE_OFF_VALUE;

	if (!LL_TIM_IsEnabledCounter(TIMx))
	{
		LL_DMA_EnableChannel(DMA1, DMA_CHANNEL);
		LL_TIM_EnableCounter(TIMx);
	}
}

uint8_t BACKLIGHT_GetBrightness(void)
{
	return currentBrightness;
}
