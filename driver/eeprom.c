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
#include <stddef.h>
#include <string.h>

#include "driver/eeprom.h"
#include "driver/py25q16.h"

// The UV-K5 V3 / UV-K1 have no I2C EEPROM, settings live in the PY25Q16 SPI
// flash. The 8 KB EEPROM address space of the UV-K5 is emulated on top of it,
// every EEPROM block mapped to its own 4 KB flash sector.
//
// The blocks up to 0x1F90 use the same flash addresses as the factory K1/K5 V3
// firmware (channels, settings, calibration), so the factory calibration is
// picked up as-is. Areas the factory layout does not have (CEC settings, CW
// memories, spectrum, Hermes) get sectors of their own from 0x160000 up.

#define FLASH_NONE 0xFFFFFFFFu

typedef struct
{
	uint32_t FlashAddr;   // sector address
	uint16_t EepromAddr;
	uint16_t Size;
} AddrMapping_t;

#define MAPPING(FlashAddr, EepromFrom, EepromTo) { FlashAddr, EepromFrom, (EepromTo) - (EepromFrom) }

static const AddrMapping_t ADDR_MAPPINGS[] = {
	// sorted by EEPROM address
	MAPPING(0x000000, 0x0000, 0x0C80),  // channels
	MAPPING(0x001000, 0x0C80, 0x0D60),  // VFOs
	MAPPING(0x002000, 0x0D60, 0x0E30),  // channel attributes
	MAPPING(0x160000, 0x0E30, 0x0E40),
	MAPPING(0x003000, 0x0E40, 0x0E68),  // FM channels
	MAPPING(0x161000, 0x0E68, 0x0E70),
	MAPPING(0x004000, 0x0E70, 0x0E80),  // settings
	MAPPING(0x005000, 0x0E80, 0x0E88),  // VFO indices
	MAPPING(0x006000, 0x0E88, 0x0E90),  // FM config
	MAPPING(0x007000, 0x0E90, 0x0EE0),  // settings / welcome strings
	MAPPING(0x008000, 0x0EE0, 0x0F18),  // DTMF
	MAPPING(0x009000, 0x0F18, 0x0F20),  // scan lists
	MAPPING(0x162000, 0x0F20, 0x0F30),
	MAPPING(0x00A000, 0x0F30, 0x0F40),  // AES key
	MAPPING(0x00B000, 0x0F40, 0x0F48),  // F-LOCK
	MAPPING(0x163000, 0x0F48, 0x0F50),
	MAPPING(0x00E000, 0x0F50, 0x1BD0),  // channel names / CEC data 1
	MAPPING(0x164000, 0x1BD0, 0x1C00),  // CEC_EEPROM_START3, CW QSO data
	MAPPING(0x00F000, 0x1C00, 0x1D00),  // DTMF contacts / CW messages
	MAPPING(0x165000, 0x1D00, 0x1E00),  // CEC_EEPROM_START1/2
	MAPPING(0x010000, 0x1E00, 0x1F90),  // calibration
	MAPPING(0x166000, 0x1F90, 0x1FF0),  // CEC_EEPROM_START4 (spectrum, Hermes)
	MAPPING(0x00C000, 0x1FF0, 0x2000),  // build options
};

static uint16_t AddrTranslate(uint16_t EepromAddr, uint16_t Size, uint32_t *pFlashAddr, bool *pEnd)
{
	for (unsigned int i = 0; i < sizeof(ADDR_MAPPINGS) / sizeof(ADDR_MAPPINGS[0]); i++)
	{
		const AddrMapping_t *p = &ADDR_MAPPINGS[i];
		if (p->EepromAddr <= EepromAddr && EepromAddr < p->EepromAddr + p->Size)
		{
			const uint16_t Offset = EepromAddr - p->EepromAddr;
			const uint16_t Remain = p->Size - Offset;

			if (Size > Remain)
				Size = Remain;

			*pFlashAddr = p->FlashAddr + Offset;
			*pEnd       = (Size == Remain);
			return Size;
		}
	}

	*pFlashAddr = FLASH_NONE;
	*pEnd       = false;
	return Size;
}

void EEPROM_ReadBuffer(uint16_t Address, void *pBuffer, uint8_t Size)
{
	uint8_t *pData = (uint8_t *)pBuffer;

	while (Size)
	{
		uint32_t FlashAddr;
		bool     End;
		const uint16_t Chunk = AddrTranslate(Address, Size, &FlashAddr, &End);

		if (FlashAddr == FLASH_NONE)
			memset(pData, 0xFF, Chunk);
		else
			PY25Q16_ReadBuffer(FlashAddr, pData, Chunk);

		Address += Chunk;
		pData   += Chunk;
		Size    -= Chunk;
	}
}

void EEPROM_WriteBuffer(uint16_t Address, const void *pBuffer)
{
	// always 8 bytes, like the 24C64 page writes of the UV-K5
	const uint8_t *pData = (const uint8_t *)pBuffer;
	uint16_t       Size  = 8;

	if (pBuffer == NULL || Address >= 0x2000)
		return;

	while (Size)
	{
		uint32_t FlashAddr;
		bool     End;
		const uint16_t Chunk = AddrTranslate(Address, Size, &FlashAddr, &End);

		if (FlashAddr != FLASH_NONE)
			PY25Q16_WriteBuffer(FlashAddr, pData, Chunk, End);

		Address += Chunk;
		pData   += Chunk;
		Size    -= Chunk;
	}
}
