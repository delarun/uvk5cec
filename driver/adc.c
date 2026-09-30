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


#include "py32f071_ll_adc.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_rcc.h"
#include "driver/adc.h"

void ADC_Init(void)
{
	// PB0 = ADC_IN8, battery voltage divider
	LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOB);
	LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_0, LL_GPIO_MODE_ANALOG);

	LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_ADC1);
	LL_RCC_SetADCClockSource(LL_RCC_ADC_CLKSOURCE_PCLK_DIV4);

	LL_ADC_SetCommonPathInternalCh(ADC1_COMMON, LL_ADC_PATH_INTERNAL_NONE);
	LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
	LL_ADC_SetDataAlignment(ADC1, LL_ADC_DATA_ALIGN_RIGHT);
	LL_ADC_SetSequencersScanMode(ADC1, LL_ADC_SEQ_SCAN_DISABLE);
	LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_SOFTWARE);
	LL_ADC_REG_SetContinuousMode(ADC1, LL_ADC_REG_CONV_SINGLE);
	LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_NONE);
	LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_DISABLE);
	LL_ADC_REG_SetSequencerDiscont(ADC1, LL_ADC_REG_SEQ_DISCONT_DISABLE);

	LL_ADC_StartCalibration(ADC1);
	while (LL_ADC_IsCalibrationOnGoing(ADC1))
		;

	LL_ADC_Enable(ADC1);
}

uint16_t ADC_ReadChannel(uint32_t Channel)
{
	if (!LL_ADC_IsEnabled(ADC1))
		LL_ADC_Enable(ADC1);

	LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, Channel);
	LL_ADC_SetChannelSamplingTime(ADC1, Channel, LL_ADC_SAMPLINGTIME_239CYCLES_5);

	LL_ADC_REG_StartConversionSWStart(ADC1);

	for (unsigned int i = 0; i < 10000; i++)
	{
		if (LL_ADC_IsActiveFlag_EOS(ADC1))
		{
			const uint16_t Value = LL_ADC_REG_ReadConversionData12(ADC1);
			LL_ADC_ClearFlag_EOS(ADC1);
			return Value;
		}
	}

	return 0;
}
