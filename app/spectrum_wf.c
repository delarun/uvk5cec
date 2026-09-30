/* Copyright 2023 fagci
 * https://github.com/fagci
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
#include "app/spectrum_wf.h"
#include "driver/eeprom.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/gpio.h"
#include "am_fix.h"
#include "audio.h"
#include "misc.h"
#include "driver/bk4819.h"
#include "functions.h"
#include "radio.h"
#include "ui/ui.h"

// ---- helpers that deltafw has in ui/helper.c / gpio.h (not present in uvk5cec) ----
static void NUMBER_ToDecimal(char *str, uint32_t val, uint8_t len, bool leadingZero)
{
    str[len] = '\0';
    for (int i = len - 1; i >= 0; i--) {
        uint8_t digit = val % 10;
        str[i] = (val == 0 && !leadingZero && i < len - 1) ? ' ' : digit + '0';
        val /= 10;
    }
}

static void UI_PrintFrequencyEx(char *str, uint32_t frequency, bool highRes)
{
    NUMBER_ToDecimal(str, frequency / 100000, 3, false);
    str[3] = '.';
    if (highRes)
        NUMBER_ToDecimal(str + 4, frequency % 100000, 5, true);
    else
        NUMBER_ToDecimal(str + 4, (frequency % 100000) / 100, 3, true);
}

// 3x5 font, full table (FONT3X5_FIRST == ' ' when ENABLE_SPECTRUM_WF)
static void UI_PrintStringSmallest(const char *pString, uint8_t x, uint8_t y, bool statusbar, bool fill)
{
    uint8_t c;
    while ((c = (uint8_t)*pString++) != '\0') {
        if (c >= FONT3X5_FIRST && c < 0x7F) {
            c -= FONT3X5_FIRST;
            for (int i = 0; i < 3; ++i) {
                uint8_t pixels = gFont3x5[c][i];
                for (int j = 0; j < 6; ++j, pixels >>= 1)
                    if (pixels & 1)
                        UI_DrawPixelBuffer(statusbar ? &gStatusLine : gFrameBuffer, x + i, y + j, fill);
            }
        }
        x += 4;
    }
}

static inline bool GPIO_IsPttPressed(void)
{
    return !GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT);
}

#ifdef ENABLE_SPECTRUM_ADVANCED
// Utility: Set LED color based on frequency and TX/RX state
static void SetBandLed(uint32_t freq, bool isTx, bool hasSignal)
{
    if (!hasSignal) {
        BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);
        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
        return;
    }

    BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);
    BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);

    FREQUENCY_Band_t band = FREQUENCY_GetBand(freq);
    if (isTx) {
        BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
    } else {
        if (band == BAND3_137MHz || band == BAND4_174MHz) {
            BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
        } else if (band == BAND6_400MHz) {
            BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
            BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
        } else {
            BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
        }
    }
}
#endif

#ifdef ENABLE_SPECTRUM_ADVANCED
#include <string.h>
#endif

#ifdef ENABLE_SCAN_RANGES
#include "app/chFrScanner.h"
#endif

#include "driver/backlight.h"
#include "frequencies.h"
#include "ui/helper.h"
#include "ui/main.h"

#ifdef ENABLE_SERIAL_SCREENCAST
#include "features/screencast/screencast.h"
#endif
#include "settings.h"

#ifdef ENABLE_SPECTRUM_EXTENSIONS
#endif

struct FrequencyBandInfo
{
    uint32_t lower;
    uint32_t upper;
    uint32_t middle;
};

#define F_MIN frequencyBandTable[0].lower
#define F_MAX frequencyBandTable[BAND_N_ELEM - 1].upper

#ifdef ENABLE_SPECTRUM_ADVANCED
/** @brief Number of waterfall history lines */
#define WATERFALL_HISTORY_DEPTH     16U
/** @brief Minimum dBm value for display */
#define DISPLAY_DBM_MIN             -130
/** @brief Maximum dBm value for display */
#define DISPLAY_DBM_MAX             -50
/** @brief Peak hold decay rate */
#define PEAK_HOLD_DECAY             2
#endif

const uint16_t RSSI_MAX_VALUE = 65535;

static uint32_t initialFreq;
static uint32_t originalFreq;
static uint8_t originalModulation;
static uint16_t originalChannelSave;
static char String[32];

static bool isInitialized = false;
bool isListening = true;
bool monitorMode = false;
bool redrawStatus = true;
bool redrawScreen = false;
bool newScanStart = true;
bool preventKeypress = true;
bool audioState = true;
bool lockAGC = false;
static bool gFKeyActive = false;

State currentState = SPECTRUM, previousState = SPECTRUM;

PeakInfo peak;
ScanInfo scanInfo;
static KeyboardState kbd = {KEY_INVALID, KEY_INVALID, 0};

#ifdef ENABLE_SCAN_RANGES
static uint16_t blacklistFreqs[15];
static uint8_t blacklistFreqsIdx;
#endif

#ifdef ENABLE_SPECTRUM_ADVANCED
static uint16_t displayRssi = 0;             /**< Filtered RSSI for display to reduce flickering */
uint8_t waterfallHistory[128][WATERFALL_HISTORY_DEPTH / 2]; /**< 4-bit packed waterfall (2 samples/byte) */
static uint16_t peakHold[128] = {0};
static uint8_t peakHoldAge[64]; // Shared timer to save RAM (1 byte per 2 bins)
uint8_t waterfallIndex = 0;                  /**< Current waterfall line index (0 to WATERFALL_HISTORY_DEPTH-1) */
#endif

const char *bwOptions[] = {"25", "12.5", "6.25"};
const uint8_t modulationTypeTuneSteps[] = {100, 50, 10};
const uint8_t modTypeReg47Values[] = {1, 7, 5};

SpectrumSettings settings = {.stepsCount = STEPS_64,
                             .scanStepIndex = S_STEP_25_0kHz,
                             .frequencyChangeStep = 80000,
                             .scanDelay = 3200,
                             .rssiTriggerLevel = 150,
                             .backlightState = true,
                             .bw = BK4819_FILTER_BW_WIDE,
                             .listenBw = BK4819_FILTER_BW_WIDE,
                             .modulationType = false,
                             .dbMin = -130,
                             .dbMax = -50};

uint32_t fMeasure = 0;
uint32_t currentFreq, tempFreq;
uint16_t rssiHistory[128];
int vfo;
uint8_t freqInputIndex = 0;
uint8_t freqInputDotIndex = 0;
KEY_Code_t freqInputArr[10];
char freqInputString[11];

uint8_t menuState = 0;
uint16_t listenT = 0;
static uint16_t freqInputTimer = 0;

RegisterSpec registerSpecs[] = {
    {},
    {"LNAs", BK4819_REG_13, 8, 0b11, 1},
    {"LNA", BK4819_REG_13, 5, 0b111, 1},
    {"VGA", BK4819_REG_13, 0, 0b111, 1},
    {"BPF", BK4819_REG_3D, 0, 0xFFFF, 0x2aaa},
    // {"MIX", 0x13, 3, 0b11, 1}, // TODO: hidden
};

#ifdef ENABLE_SPECTRUM_EXTENSIONS
const int8_t LNAsOptions[] = {-19, -16, -11, 0};
const int8_t LNAOptions[] = {-24, -19, -14, -9, -6, -4, -2, 0};
const int8_t VGAOptions[] = {-33, -27, -21, -15, -9, -6, -3, 0};
const char *BPFOptions[] = {"8.46", "7.25", "6.35", "5.64", "5.08", "4.62", "4.23"};
#endif

uint16_t statuslineUpdateTimer = 0;

#ifdef ENABLE_SPECTRUM_ADVANCED
static void LoadSettings(void)
{
    uint8_t data[8] = {0};
    EEPROM_ReadBuffer(SPECTRUM_WF_EEPROM, data, sizeof(data));

    settings.scanStepIndex = ((data[3] & 0xF0) >> 4);
    if (settings.scanStepIndex > 14) settings.scanStepIndex = S_STEP_25_0kHz;

    settings.stepsCount = (data[3] & 0b1100) >> 2;
    if (settings.stepsCount > 3) settings.stepsCount = STEPS_64;

    settings.listenBw = (data[3] & 0b0011);
    if (settings.listenBw > 2) settings.listenBw = BK4819_FILTER_BW_WIDE;
}

static void SaveSettings(void)
{
    uint8_t data[8] = {0};
    EEPROM_ReadBuffer(SPECTRUM_WF_EEPROM, data, sizeof(data));

    data[3] = (settings.scanStepIndex << 4) | (settings.stepsCount << 2) | settings.listenBw;
    EEPROM_WriteBuffer(SPECTRUM_WF_EEPROM, data);
}
#endif

static uint8_t DBm2S(int dbm)
{
    uint8_t i = 0;
    dbm *= -1;
    for (i = 0; i < ARRAY_SIZE(U8RssiMap); i++)
    {
        if (dbm >= U8RssiMap[i])
        {
            return i;
        }
    }
    return i;
}

static int Rssi2DBm(uint16_t rssi)
{
    return (rssi / 2) - 160 + dBmCorrTable[gRxVfo->Band];
}

static uint16_t GetRegMenuValue(uint8_t st)
{
    RegisterSpec s = registerSpecs[st];
    return (BK4819_ReadRegister(s.num) >> s.offset) & s.mask;
}

void LockAGC()
{
    //RADIO_SetupAGC(settings.modulationType == MODULATION_AM, lockAGC);
    RADIO_SetupAGC(false, lockAGC);
    //lockAGC = true;
    lockAGC = false;
}

static void SetRegMenuValue(uint8_t st, bool add)
{
    uint16_t v = GetRegMenuValue(st);
    RegisterSpec s = registerSpecs[st];

    if (s.num == BK4819_REG_13)
        LockAGC();

    uint16_t reg = BK4819_ReadRegister(s.num);
    if (add && v <= s.mask - s.inc)
    {
        v += s.inc;
    }
    else if (!add && v >= 0 + s.inc)
    {
        v -= s.inc;
    }
    // TODO: use max value for bits count in max value, or reset by additional
    // mask in spec
    reg &= ~(s.mask << s.offset);
    BK4819_WriteRegister(s.num, reg | (v << s.offset));
    redrawScreen = true;
}

// GUI functions

#ifndef ENABLE_CUSTOM_FIRMWARE_MODS
static void PutPixel(uint8_t x, uint8_t y, bool fill)
{
    UI_DrawPixelBuffer(gFrameBuffer, x, y, fill);
}
#endif

#ifndef ENABLE_SPECTRUM_ADVANCED
static void DrawVLine(int sy, int ey, int nx, bool fill)
{
    for (int i = sy; i <= ey; i++)
    {
        if (i < 56 && nx < 128)
        {
            PutPixel(nx, i, fill);
        }
    }
}
#endif

// Utility functions

static KEY_Code_t GetKey()
{
    KEY_Code_t btn = KEYBOARD_Poll();
    if (btn == KEY_INVALID && GPIO_IsPttPressed())
    {
        btn = KEY_PTT;
    }
    return btn;
}

static int clamp(int v, int min, int max)
{
    return v <= min ? min : (v >= max ? max : v);
}

static uint8_t my_abs(signed v) { return v > 0 ? v : -v; }

void SetState(State state)
{
    previousState = currentState;
    currentState = state;
    redrawScreen = true;
    redrawStatus = true;

#ifdef ENABLE_SPECTRUM_ADVANCED
    if (state == STILL) {
        displayRssi = scanInfo.rssi;
    }
#endif
}

// Radio functions

static void ToggleAFBit(bool on)
{
    uint16_t reg = BK4819_ReadRegister(BK4819_REG_47);
    reg &= ~(1 << 8);
    if (on)
        reg |= on << 8;
    BK4819_WriteRegister(BK4819_REG_47, reg);
}

static const BK4819_REGISTER_t registers_to_save[] = {
    BK4819_REG_30,
    BK4819_REG_37,
    BK4819_REG_3D,
    BK4819_REG_43,
    BK4819_REG_47,
    BK4819_REG_48,
    BK4819_REG_7E,
};

static uint16_t registers_stack[sizeof(registers_to_save)];

static void BackupRegisters()
{
    for (uint32_t i = 0; i < ARRAY_SIZE(registers_to_save); i++)
    {
        registers_stack[i] = BK4819_ReadRegister(registers_to_save[i]);
    }
}

static void RestoreRegisters()
{

    for (uint32_t i = 0; i < ARRAY_SIZE(registers_to_save); i++)
    {
        BK4819_WriteRegister(registers_to_save[i], registers_stack[i]);
    }

#ifdef ENABLE_CUSTOM_FIRMWARE_MODS
    gVfoConfigureMode = VFO_CONFIGURE;
#endif
}

static void ToggleAFDAC(bool on)
{
    uint32_t Reg = BK4819_ReadRegister(BK4819_REG_30);
    Reg &= ~(1 << 9);
    if (on)
        Reg |= (1 << 9);
    BK4819_WriteRegister(BK4819_REG_30, Reg);
}

static void SetF(uint32_t f)
{
    fMeasure = f;

    BK4819_SetFrequency(fMeasure);
    BK4819_PickRXFilterPathBasedOnFrequency(fMeasure);
    uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30, reg);
}

// Spectrum related

bool IsPeakOverLevel() { return peak.rssi >= settings.rssiTriggerLevel; }

static void ResetPeak()
{
    peak.t = 0;
    peak.rssi = 0;
}

#if defined(ENABLE_SPECTRUM_EXTENSIONS) && !defined(ENABLE_SPECTRUM_ADVANCED)
    static void setTailFoundInterrupt()
    {
        BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_02_CxCSS_TAIL | BK4819_REG_02_SQUELCH_FOUND);
    }

    static bool checkIfTailFound()
    {
      uint16_t interrupt_status_bits;
      // if interrupt waiting to be handled
      if(BK4819_ReadRegister(BK4819_REG_0C) & 1u) {
        // reset the interrupt
        BK4819_WriteRegister(BK4819_REG_02, 0);
        // fetch the interrupt status bits
        interrupt_status_bits = BK4819_ReadRegister(BK4819_REG_02);
        // if tail found interrupt
        if (interrupt_status_bits & BK4819_REG_02_CxCSS_TAIL)
        {
            listenT = 0;
            // disable interrupts
            BK4819_WriteRegister(BK4819_REG_3F, 0);
            // reset the interrupt
            BK4819_WriteRegister(BK4819_REG_02, 0);
            return true;
        }
      }
      return false;
    }
#endif

bool IsCenterMode() { return settings.scanStepIndex < S_STEP_2_5kHz; }
// scan step in 0.01khz
uint16_t GetScanStep() { return scanStepValues[settings.scanStepIndex]; }

uint16_t GetStepsCount()
{
#ifdef ENABLE_SCAN_RANGES
    if (gScanRangeStart)
    {
        uint32_t range = gScanRangeStop - gScanRangeStart;
        uint16_t step = GetScanStep();
        return (range / step) + 1;  // +1 to include up limit
    }
#endif
    return 128 >> settings.stepsCount;
}

#ifdef ENABLE_SCAN_RANGES
static uint16_t GetStepsCountDisplay()
{
    if (gScanRangeStart)
    {
        return (gScanRangeStop - gScanRangeStart) / GetScanStep();
    }
    return GetStepsCount();
}
#endif

uint32_t GetBW() { return GetStepsCount() * GetScanStep(); }
uint32_t GetFStart()
{
    return IsCenterMode() ? currentFreq - (GetBW() >> 1) : currentFreq;
}

uint32_t GetFEnd()
{
#ifdef ENABLE_SCAN_RANGES
    if (gScanRangeStart)
    {
        return gScanRangeStop;
    }
#endif
    return currentFreq + GetBW();
}

static void DeInitSpectrum()
{
    gTxVfo->pRX->Frequency = originalFreq;
    gTxVfo->pTX->Frequency = originalFreq;
    gTxVfo->Modulation = originalModulation;
    gTxVfo->CHANNEL_SAVE = originalChannelSave;
    gEeprom.ScreenChannel[vfo] = originalChannelSave;

    RestoreRegisters();
    RADIO_ConfigureChannel(vfo, VFO_CONFIGURE_RELOAD);
    isInitialized = false;
}

uint8_t GetBWRegValueForScan()
{
    return scanStepBWRegValues[settings.scanStepIndex];
}

uint16_t GetRssi()
{
    // SYSTICK_DelayUs(800);
    // testing autodelay based on Glitch value
    while ((BK4819_ReadRegister(0x63) & 0b11111111) >= 255)
    {
        SYSTICK_DelayUs(100);
    }
    uint16_t rssi = BK4819_GetRSSI();
#ifdef ENABLE_AM_FIX
    if (settings.modulationType == MODULATION_AM && gSetting_AM_fix)
        rssi += AM_fix_get_gain_diff() * 2;
#endif
    return rssi;
}

static void ToggleAudio(bool on)
{
    if (on == audioState)
    {
        return;
    }
    audioState = on;
    if (on)
    {
        AUDIO_AudioPathOn();
    }
    else
    {
        AUDIO_AudioPathOff();
    }
}

static void ToggleRX(bool on)
{
#ifdef ENABLE_SPECTRUM_EXTENSIONS
    if (isListening == on) {
        return;
    }
#endif
    isListening = on;

    if (on)
    {
        /*
         * Apply full VFO RX configuration for best audio and signal path,
         * matching VFO mode. This sets up squelch, filter, gain, compander, etc.
         * Use the current frequency as a temporary VFO.
         */
        extern VFO_Info_t *gRxVfo;
        if (gRxVfo) {
            gRxVfo->pRX->Frequency = fMeasure;
            RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
        }
        RADIO_SetupRegisters(false);
        RADIO_SetupAGC(settings.modulationType == MODULATION_AM, lockAGC);
        ToggleAudio(true);
        ToggleAFDAC(true);
        ToggleAFBit(true);
        // Set LED after RX is fully configured and signal is present
        SetBandLed(fMeasure, false, true);

#ifdef ENABLE_SPECTRUM_ADVANCED
        listenT = 2; // Very short startup hold for real-time response
#else
        listenT = 1000;
#endif
        BK4819_WriteRegister(0x43, listenBWRegValues[settings.listenBw]);
    }
    else
    {
        BK4819_WriteRegister(0x43, GetBWRegValueForScan());
        ToggleAudio(false);
        ToggleAFDAC(false);
        ToggleAFBit(false);
        // Turn off both LEDs when RX is off
        SetBandLed(fMeasure, false, false);
    }
}

// Scan info

static void ResetScanStats()
{
    scanInfo.rssi = 0;
    scanInfo.rssiMax = 0;
    scanInfo.iPeak = 0;
    scanInfo.fPeak = 0;
}

static void InitScan()
{
    ResetScanStats();
    scanInfo.i = 0;
    scanInfo.f = GetFStart();

    scanInfo.scanStep = GetScanStep();
    scanInfo.measurementsCount = GetStepsCount();
    scanInfo.rssiMin = RSSI_MAX_VALUE;
}

static void ResetBlacklist()
{
    for (int i = 0; i < 128; ++i)
    {
        if (rssiHistory[i] == RSSI_MAX_VALUE)
            rssiHistory[i] = 0;
    }
#ifdef ENABLE_SCAN_RANGES
    memset(blacklistFreqs, 0, sizeof(blacklistFreqs));
    blacklistFreqsIdx = 0;
#endif
}

static void RelaunchScan()
{
    InitScan();
    ResetPeak();
    ToggleRX(false);
#ifdef SPECTRUM_AUTOMATIC_SQUELCH
    settings.rssiTriggerLevel = RSSI_MAX_VALUE;
#endif
    preventKeypress = true;
    scanInfo.rssiMin = RSSI_MAX_VALUE;
}

static void UpdateScanInfo()
{
    if (scanInfo.rssi > scanInfo.rssiMax)
    {
        scanInfo.rssiMax = scanInfo.rssi;
        scanInfo.fPeak = scanInfo.f;
        scanInfo.iPeak = scanInfo.i;
    }

    if (scanInfo.rssi < scanInfo.rssiMin)
    {
        scanInfo.rssiMin = scanInfo.rssi;
        settings.dbMin = Rssi2DBm(scanInfo.rssiMin);
        redrawStatus = true;
    }
}

static void AutoTriggerLevel()
{
#ifdef ENABLE_SPECTRUM_ADVANCED
    if (settings.rssiTriggerLevel == RSSI_MAX_VALUE)
    {
        uint16_t initialTrigger = scanInfo.rssiMax + 20;
        settings.rssiTriggerLevel = clamp(initialTrigger, 0, RSSI_MAX_VALUE);
    }
    else
    {
        uint16_t newTrigger = clamp(scanInfo.rssiMax + 8, 0, RSSI_MAX_VALUE);
        const uint16_t MIN_TRIGGER = scanInfo.rssiMax + 15;
        if (newTrigger < MIN_TRIGGER) newTrigger = MIN_TRIGGER;

        if (newTrigger > settings.rssiTriggerLevel)
        {
            int16_t diff = (int16_t)newTrigger - (int16_t)settings.rssiTriggerLevel;
            int16_t step = (diff > 6) ? 3 : ((diff > 3) ? 2 : 1);
            settings.rssiTriggerLevel += step;
        }
        else if (newTrigger < settings.rssiTriggerLevel - 4)
        {
            int16_t diff = (int16_t)settings.rssiTriggerLevel - (int16_t)newTrigger;
            int16_t step = (diff > 6) ? 3 : ((diff > 3) ? 2 : 1);
            settings.rssiTriggerLevel -= step;
        }
    }
#else
    if (settings.rssiTriggerLevel == RSSI_MAX_VALUE)
    {
        settings.rssiTriggerLevel = clamp(scanInfo.rssiMax + 8, 0, RSSI_MAX_VALUE);
    }
#endif
}

static void UpdatePeakInfoForce()
{
    peak.t = 0;
    peak.rssi = scanInfo.rssiMax;
    peak.f = scanInfo.fPeak;
    peak.i = scanInfo.iPeak;
    AutoTriggerLevel();
}

static void UpdatePeakInfo()
{
    if (peak.f == 0 || peak.t >= 1024 || peak.rssi < scanInfo.rssiMax)
        UpdatePeakInfoForce();
    else
        peak.t++;
}

#ifdef ENABLE_SPECTRUM_ADVANCED
static void UpdateWaterfall(void)
{
    waterfallIndex = (waterfallIndex + 1) % WATERFALL_HISTORY_DEPTH;

    uint16_t minRssi = 65535, maxRssi = 0;
    uint32_t sumRssi = 0;
    uint16_t validSamples = 0;

    for (uint8_t x = 0; x < 128; x++)
    {
        uint16_t rssi = rssiHistory[x];
        if (rssi != RSSI_MAX_VALUE && rssi != 0)
        {
            if (rssi < minRssi) minRssi = rssi;
            if (rssi > maxRssi) maxRssi = rssi;
            sumRssi += rssi;
            validSamples++;
        }
    }

    for (uint8_t x = 0; x < 128; x++)
    {
        uint16_t rssi = rssiHistory[x];
        uint8_t level;

        if (rssi == RSSI_MAX_VALUE || rssi == 0)
        {
            level = 0;
        }
        else if (validSamples > 0)
        {
            uint16_t range = (maxRssi > minRssi) ? (maxRssi - minRssi) : 1;
            uint32_t normalized = ((rssi - minRssi) * 15) / range;
            level = (uint8_t)(normalized & 0x0F);
            if (level > 0 && level < 3) level = 3;
        }
        else
        {
            level = 0;
        }

        // Pack 4-bit level into waterfall history (2 levels per byte)
        uint8_t *pPacked = &waterfallHistory[x][waterfallIndex / 2];
        if (waterfallIndex % 2 == 0) {
            *pPacked = (*pPacked & 0xF0) | (level & 0x0F);
        } else {
            *pPacked = (*pPacked & 0x0F) | (level << 4);
        }
    }
}
#endif

static void SetRssiHistory(uint16_t idx, uint16_t rssi)
{
#ifdef ENABLE_SCAN_RANGES
    if (scanInfo.measurementsCount > 128)
    {
        uint8_t i = (uint32_t)128 * 1000 / scanInfo.measurementsCount * idx / 1000;
        if (rssiHistory[i] < rssi || isListening)
            rssiHistory[i] = rssi;
        rssiHistory[(i + 1) % 128] = 0;
#ifdef ENABLE_SPECTRUM_ADVANCED
        if (currentState == SPECTRUM) {
            static uint8_t wf_counter = 0;
            if (++wf_counter >= (isListening ? 1 : 2)) { wf_counter = 0; UpdateWaterfall(); }
        }
#endif
        return;
    }
#endif
    rssiHistory[idx] = rssi;
#ifdef ENABLE_SPECTRUM_ADVANCED
    if (currentState == SPECTRUM) {
        static uint8_t wf_counter2 = 0;
        if (++wf_counter2 >= (isListening ? 1 : 2)) { wf_counter2 = 0; UpdateWaterfall(); }
    }
#endif
}

static void Measure()
{
    scanInfo.rssi = GetRssi();
    SetRssiHistory(scanInfo.i, scanInfo.rssi);
}

// Update things by keypress

static uint16_t dbm2rssi(int dBm)
{
    return (dBm + 160 - dBmCorrTable[gRxVfo->Band]) * 2;
}

static void ClampRssiTriggerLevel()
{
    settings.rssiTriggerLevel =
        clamp(settings.rssiTriggerLevel, dbm2rssi(settings.dbMin),
              dbm2rssi(settings.dbMax));
}

static void UpdateRssiTriggerLevel(bool inc)
{
    if (inc)
        settings.rssiTriggerLevel += 2;
    else
        settings.rssiTriggerLevel -= 2;

    ClampRssiTriggerLevel();

    redrawScreen = true;
    redrawStatus = true;
}

static void UpdateDBMax(bool inc)
{
    if (inc && settings.dbMax < 10)
    {
        settings.dbMax += 1;
    }
    else if (!inc && settings.dbMax > settings.dbMin)
    {
        settings.dbMax -= 1;
    }
    else
    {
        return;
    }

    ClampRssiTriggerLevel();
    redrawStatus = true;
    redrawScreen = true;
    SYSTEM_DelayMs(20);
}

static void UpdateScanStep(bool inc)
{
    if (inc)
    {
        settings.scanStepIndex = settings.scanStepIndex != S_STEP_100_0kHz ? settings.scanStepIndex + 1 : 0;
    }
    else
    {
        settings.scanStepIndex = settings.scanStepIndex != 0 ? settings.scanStepIndex - 1 : S_STEP_100_0kHz;
    }

    settings.frequencyChangeStep = GetBW() >> 1;
    RelaunchScan();
    ResetBlacklist();
    redrawScreen = true;
}

static void UpdateCurrentFreq(bool inc)
{
    if (inc && currentFreq < F_MAX)
    {
        currentFreq += settings.frequencyChangeStep;
    }
    else if (!inc && currentFreq > F_MIN)
    {
        currentFreq -= settings.frequencyChangeStep;
    }
    else
    {
        return;
    }
    RelaunchScan();
    ResetBlacklist();
    redrawScreen = true;
}

static void UpdateCurrentFreqStill(bool inc)
{
    uint8_t offset = modulationTypeTuneSteps[settings.modulationType];
    uint32_t f = fMeasure;
    if (inc && f < F_MAX)
    {
        f += offset;
    }
    else if (!inc && f > F_MIN)
    {
        f -= offset;
    }
    SetF(f);
    if (isListening) {
        BK4819_WriteRegister(0x43, listenBWRegValues[settings.listenBw]);
    }
    redrawScreen = true;
}

static void UpdateFreqChangeStep(bool inc)
{
    uint16_t diff = GetScanStep() * 4;
    if (inc && settings.frequencyChangeStep < 200000)
    {
        settings.frequencyChangeStep += diff;
    }
    else if (!inc && settings.frequencyChangeStep > 10000)
    {
        settings.frequencyChangeStep -= diff;
    }
    SYSTEM_DelayMs(100);
    redrawScreen = true;
}

static void ToggleModulation()
{
    if (settings.modulationType < MODULATION_UKNOWN - 1)
    {
        settings.modulationType++;
    }
    else
    {
        settings.modulationType = MODULATION_FM;
    }
    RADIO_SetModulation(settings.modulationType);

    RelaunchScan();
    redrawScreen = true;
}

static void ToggleListeningBW()
{
    if (settings.listenBw == BK4819_FILTER_BW_NARROWER)
    {
        settings.listenBw = BK4819_FILTER_BW_WIDE;
    }
    else
    {
        settings.listenBw++;
    }
    redrawScreen = true;
}

static void ToggleBacklight()
{
    settings.backlightState = !settings.backlightState;
    if (settings.backlightState)
    {
        BACKLIGHT_TurnOn();
    }
    else
    {
        BACKLIGHT_TurnOff();
    }
}

static void ToggleStepsCount()
{
    if (settings.stepsCount == STEPS_128)
    {
        settings.stepsCount = STEPS_16;
    }
    else
    {
        settings.stepsCount--;
    }
    settings.frequencyChangeStep = GetBW() >> 1;
    RelaunchScan();
    ResetBlacklist();
    redrawScreen = true;
}

static void ResetFreqInput()
{
    tempFreq = 0;
    for (int i = 0; i < 10; ++i)
    {
        freqInputString[i] = '-';
    }
}

static void FreqInput()
{
    freqInputIndex = 0;
    freqInputDotIndex = 0;
    ResetFreqInput();
    SetState(FREQ_INPUT);
}

static void UpdateFreqInput(KEY_Code_t key)
{
    if (key != KEY_EXIT && freqInputIndex >= 10)
    {
        return;
    }
    if (key == KEY_STAR)
    {
        if (freqInputIndex == 0 || freqInputDotIndex)
        {
            return;
        }
        freqInputDotIndex = freqInputIndex;
    }
    if (key == KEY_EXIT)
    {
        freqInputIndex--;
        if (freqInputDotIndex == freqInputIndex)
            freqInputDotIndex = 0;
    }
    else
    {
        freqInputArr[freqInputIndex++] = key;
    }

    ResetFreqInput();

    uint8_t dotIndex =
        freqInputDotIndex == 0 ? freqInputIndex : freqInputDotIndex;

    KEY_Code_t digitKey;
    for (int i = 0; i < 10; ++i)
    {
        if (i < freqInputIndex)
        {
            digitKey = freqInputArr[i];
            freqInputString[i] = digitKey <= KEY_9 ? '0' + digitKey - KEY_0 : '.';
        }
        else
        {
            freqInputString[i] = '-';
        }
    }

    uint32_t base = 100000; // 1MHz in BK units
    for (int i = dotIndex - 1; i >= 0; --i)
    {
        tempFreq += (freqInputArr[i] - KEY_0) * base;
        base *= 10;
    }

    base = 10000; // 0.1MHz in BK units
    if (dotIndex < freqInputIndex)
    {
        for (int i = dotIndex + 1; i < freqInputIndex; ++i)
        {
            tempFreq += (freqInputArr[i] - KEY_0) * base;
            base /= 10;
        }
    }
    redrawScreen = true;
}

static void Blacklist()
{
#ifdef ENABLE_SCAN_RANGES
    blacklistFreqs[blacklistFreqsIdx++ % ARRAY_SIZE(blacklistFreqs)] = peak.i;
#endif

    SetRssiHistory(peak.i, RSSI_MAX_VALUE);
    ResetPeak();
    ToggleRX(false);
    ResetScanStats();
}

#ifdef ENABLE_SCAN_RANGES
static bool IsBlacklisted(uint16_t idx)
{
    if (blacklistFreqsIdx)
        for (uint8_t i = 0; i < ARRAY_SIZE(blacklistFreqs); i++)
            if (blacklistFreqs[i] == idx)
                return true;
    return false;
}
#endif

// Draw things

// applied x2 to prevent initial rounding
uint8_t Rssi2PX(uint16_t rssi, uint8_t pxMin, uint8_t pxMax)
{
    const int DB_MIN = settings.dbMin << 1;
    const int DB_MAX = settings.dbMax << 1;
    const int DB_RANGE = DB_MAX - DB_MIN;

    const uint8_t PX_RANGE = pxMax - pxMin;

    int dbm = clamp(Rssi2DBm(rssi) << 1, DB_MIN, DB_MAX);

    return ((dbm - DB_MIN) * PX_RANGE + DB_RANGE / 2) / DB_RANGE + pxMin;
}

uint8_t Rssi2Y(uint16_t rssi)
{
    return DrawingEndY - Rssi2PX(rssi, 0, DrawingEndY);
}

#ifdef ENABLE_SPECTRUM_ADVANCED
static void DrawWaterfall(void)
{
    static const uint8_t bayerMatrix[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 }
    };

    const uint8_t WATERFALL_START_Y = 41;
    const uint8_t WATERFALL_HEIGHT = 19;
    const uint8_t WATERFALL_WIDTH = 128;
    const uint16_t SPEC_WIDTH = GetStepsCount();
    // const float xScale = (float)SPEC_WIDTH / WATERFALL_WIDTH;

    for (uint8_t y_offset = 0; y_offset < WATERFALL_HEIGHT - 1; y_offset++)
    {
        int8_t historyRow = (int8_t)waterfallIndex - (int8_t)y_offset;
        if (historyRow < 0) historyRow += WATERFALL_HISTORY_DEPTH;

        uint8_t y_pos = WATERFALL_START_Y + y_offset;
        if (y_pos > 63) break;

        uint8_t fadeFactor = (uint8_t)(((uint16_t)(WATERFALL_HEIGHT - 1 - y_offset) * 16) / (WATERFALL_HEIGHT - 1));

        for (uint8_t x = 0; x < WATERFALL_WIDTH; x++)
        {
            uint16_t specIdx = (uint16_t)(((uint32_t)x * SPEC_WIDTH) / WATERFALL_WIDTH);
            if (specIdx >= SPEC_WIDTH - 1) specIdx = SPEC_WIDTH - 2;
            
            // Unpack 4-bit levels
            uint8_t val0 = waterfallHistory[specIdx][historyRow / 2];
            uint8_t l0 = (historyRow % 2 == 0) ? (val0 & 0x0F) : (val0 >> 4);
            
            uint8_t val1 = waterfallHistory[specIdx + 1][historyRow / 2];
            uint8_t l1 = (historyRow % 2 == 0) ? (val1 & 0x0F) : (val1 >> 4);
            
            uint16_t fracNumerator = (uint16_t)(((uint32_t)x * SPEC_WIDTH * 256) / WATERFALL_WIDTH) % 256;
            uint16_t interpValue = ((uint16_t)l0 * (256 - fracNumerator) + (uint16_t)l1 * fracNumerator) / 256;
            uint8_t level = (uint8_t)(((interpValue * fadeFactor) / 16) & 0x0F);

            if (level > bayerMatrix[y_offset & 3][x & 3]) {
                UI_DrawPixelBuffer(gFrameBuffer, x, y_pos, true);
            }
        }
    }
}

#define SMOOTHING_WINDOW 3
static void SmoothRssiHistory(const uint16_t *input, uint16_t *output, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i)
    {
        uint32_t sum = 0;
        uint8_t n = 0;
        for (int8_t j = -(SMOOTHING_WINDOW/2); j <= SMOOTHING_WINDOW/2; ++j)
        {
            int k = i + j;
            if (k >= 0 && k < count) { sum += input[k]; n++; }
        }
        output[i] = (n > 0) ? (sum / n) : input[i];
    }
}

static void DrawSpectrumCurve(const uint16_t *smoothed, uint8_t bars)
{
    uint8_t prev_x = 0, prev_y = 0;
    for (uint8_t i = 0; i < bars; ++i)
    {
        uint8_t x = (i * 128) / bars;
        uint8_t y = Rssi2Y(smoothed[i]);
        if (i > 0)
        {
            int dx = x - prev_x;
            int dy = y - prev_y;
            int steps = (dx > 0) ? dx : (dy > 0 ? dy : -dy); 
            if (dx < 0 && -dx > steps) steps = -dx;
            if (dy < 0 && -dy > steps) steps = -dy;
            if (steps == 0) steps = 1;
            for (int s = 1; s <= steps; ++s)
            {
                UI_DrawPixelBuffer(gFrameBuffer, prev_x + (dx * s) / steps, prev_y + (dy * s) / steps, true);
            }
        }
        prev_x = x; prev_y = y;
    }
}

static void DrawSpectrumEnhanced(void)
{
    uint16_t steps = GetStepsCount();
    uint8_t bars = (steps > 128) ? 128 : (uint8_t)steps;
    uint16_t procBuffer[128]; 
    SmoothRssiHistory(rssiHistory, procBuffer, bars);

    uint16_t minRssi = 65535, maxRssi = 0, valid = 0;
    for (uint8_t i = 0; i < bars; ++i) {
        uint16_t r = procBuffer[i];
        if (r > peakHold[i]) {
            peakHold[i] = r;
            peakHoldAge[i >> 1] = 0;
        } else if (peakHold[i] > 0) {
            if (peakHoldAge[i >> 1] < 50) { 
                if (!(i & 1)) peakHoldAge[i >> 1]++; 
            } else {
                if (peakHold[i] > PEAK_HOLD_DECAY) peakHold[i] -= PEAK_HOLD_DECAY;
                else peakHold[i] = 0;
            }
        }
        if (r != RSSI_MAX_VALUE && r != 0) {
            if (r < minRssi) minRssi = r;
            if (r > maxRssi) maxRssi = r;
            valid++;
        }
    }
    
    if (valid == 0) { minRssi = 0; maxRssi = 1; }
    uint16_t range = (maxRssi > minRssi) ? (maxRssi - minRssi) : 1;
    static const uint8_t sugar1_map[16] = { 0, 4, 5, 7, 8, 9, 9, 10, 11, 12, 12, 13, 13, 14, 14, 15 };

    for (uint8_t i = 0; i < bars; ++i) {
        uint16_t r = procBuffer[i];
        uint8_t level = (uint8_t)(((r - minRssi) * 15) / range);
        if (level > 15) level = 15;
        uint8_t mapped = isListening ? sugar1_map[level] : level;
        uint8_t boosted = (uint8_t)((mapped * mapped) / 15);
        procBuffer[i] = minRssi + (boosted * range) / 15;
    }

    DrawSpectrumCurve(procBuffer, bars);

    uint8_t horizonY = Rssi2Y(minRssi);
    for (uint8_t i = 0; i < 128; i += 8) { UI_DrawPixelBuffer(gFrameBuffer, i, horizonY, true); }

    for (uint8_t i = 0; i < bars; ++i) {
        if ((i % 2) == 0 && peakHold[i] > 0) { 
            uint8_t level = (uint8_t)(((peakHold[i] - minRssi) * 15) / range);
            if (level > 15) level = 15;
            uint8_t mapped = isListening ? sugar1_map[level] : level;
            uint16_t drawRssi = minRssi + (((uint16_t)mapped * mapped / 15) * range) / 15;
            UI_DrawPixelBuffer(gFrameBuffer, (i * 128) / bars, Rssi2Y(drawRssi), true);
        }
    }
}
#endif

#ifndef ENABLE_SPECTRUM_ADVANCED
#ifdef ENABLE_CUSTOM_FIRMWARE_MODS
    static void DrawSpectrum()
    {
        uint16_t steps = GetStepsCount();
        // max bars at 128 to correctly draw larger numbers of samples
        uint8_t bars = (steps > 128) ? 128 : steps;

        uint8_t ox = 0;
        for (uint8_t i = 0; i < bars; ++i)
        {
            uint16_t rssi = rssiHistory[(bars>128) ? i >> settings.stepsCount : i];
            
#ifdef ENABLE_SCAN_RANGES
            uint8_t x;
            if (gScanRangeStart && bars > 1)
            {
                // Total width units = (bars - 1) full bars + 2 half bars = bars
                // First bar: half width, middle bars: full width, last bar: half width
                // Scale: 128 pixels / (bars - 1) = pixels per full bar
                uint16_t fullWidth = (128 << 8) / (bars - 1); // x256 for precision
                
                if (i == 0)
                {
                    x = fullWidth / (2 << 8); // half of /256 (because fullWidth is x256)
                }
                else
                {
                    // Position = half + (i-1) full bars + current bar
                    x = fullWidth / (2 << 8) + (uint16_t)i * fullWidth / (1 << 8);
                    if (i == bars - 1) x = 128; // Last bar ends at screen edge
                }
            }
            else
#endif
            {
                uint8_t shift_graph = 64 / steps + 1;
                x = i * 128 / bars + shift_graph;
            }

            if (rssi != RSSI_MAX_VALUE)
            {
                for (uint8_t xx = ox; xx < x; xx++)
                {
                    DrawVLine(Rssi2Y(rssi), DrawingEndY, xx, true);
                }
            }
            ox = x;
        }
    }
#else
    static void DrawSpectrum()
    {
        for (uint8_t x = 0; x < 128; ++x)
        {
            uint16_t rssi = rssiHistory[x >> settings.stepsCount];
            if (rssi != RSSI_MAX_VALUE)
            {
                DrawVLine(Rssi2Y(rssi), DrawingEndY, x, true);
            }
        }
    }
#endif
#endif

static void DrawStatus()
{
#ifdef SPECTRUM_EXTRA_VALUES
    // sprintf(String, "%d/%d P:%d T:%d", settings.dbMin, settings.dbMax, Rssi2DBm(peak.rssi), Rssi2DBm(settings.rssiTriggerLevel));
    NUMBER_ToDecimal(String, settings.dbMin, 4, false); // Assuming max 4 digits for db
    String[4] = '/';
    NUMBER_ToDecimal(String + 5, settings.dbMax, 4, false);
    strcpy(String + 9, " P:");
    NUMBER_ToDecimal(String + 12, Rssi2DBm(peak.rssi), 4, false);
    strcpy(String + 16, " T:");
    NUMBER_ToDecimal(String + 19, Rssi2DBm(settings.rssiTriggerLevel), 4, false);
#else
    // sprintf(String, "%d/%d", settings.dbMin, settings.dbMax);
    NUMBER_ToDecimal(String, settings.dbMin, 4, false);
    String[4] = '/';
    NUMBER_ToDecimal(String + 5, settings.dbMax, 4, false);
#endif
    UI_PrintStringSmallest(String, 0, 0, true, true);

    BOARD_ADC_GetBatteryInfo(&gBatteryVoltages[gBatteryCheckCounter++ % 4], &gBatteryCurrent);

    uint16_t voltage = (gBatteryVoltages[0] + gBatteryVoltages[1] +
                        gBatteryVoltages[2] + gBatteryVoltages[3]) /
                       4 * 760 / gBatteryCalibration[3];

    unsigned perc = BATTERY_VoltsToPercent(voltage);

    // sprintf(String, "%d %d", voltage, perc);
    // UI_PrintStringSmallest(String, 48, 1, true, true);

    gStatusLine[116] = 0b00001110;
    gStatusLine[117] = 0b00011111;
    for (int i = 118; i <= 126; i++)
    {
        gStatusLine[i] = 0b00010001;
    }

    for (unsigned i = 127; i >= 118; i--)
    {
        if (127 - i <= (perc + 5) * 9 / 100)
        {
            gStatusLine[i] = 0b00011111;
        }
    }

    if (gFKeyActive)
    {
        // Draw inverted F-key (bits 0-5)
        for (int i = 50; i <= 54; i++) {
            gStatusLine[i] |= 0b00111111;
        }
        // Draw 'F' inverted (XOR with gFont3x5 'F' bitmap {0x1f, 0x05, 0x05})
        gStatusLine[51] ^= 0x1f;
        gStatusLine[52] ^= 0x05;
        gStatusLine[53] ^= 0x05;
    }
    
    // Draw separator line at y=6 (Row 7)
    for (int i = 0; i < 128; i++) {
        gStatusLine[i] |= (1 << 6);
    }
}

#ifdef ENABLE_SPECTRUM_EXTENSIONS
static void ShowChannelName(uint32_t f)
{
    char channelName[13];
    memset(channelName, 0, sizeof(channelName));

    for (int i = 0; i < 200; i++) {
        if (RADIO_CheckValidChannel(i, false, 0)) {
            if (SETTINGS_FetchChannelFrequency(i) == f) {
                SETTINGS_FetchChannelName(channelName, i);
                if (channelName[0] && channelName[0] != 0xFF) {
#ifdef ENABLE_SPECTRUM_ADVANCED
                    UI_PrintStringSmallest(channelName, 0, 14, false, true);
#else
                    UI_PrintStringSmallBufferNormal(channelName, gStatusLine + 36);
                    ST7565_BlitStatusLine();
#endif
                    return;
                }
            }
        }
    }
}
#endif

static void DrawF(uint32_t f)
{
    UI_PrintFrequencyEx(String, f, true);
    strcat(String, " MHz");
    UI_PrintStringSmallest(String, 38, 1, false, true);

    strcpy(String, gModulationStr[settings.modulationType]);
    UI_PrintStringSmallest(String, 116, 1, false, true);
    strcpy(String, bwOptions[settings.listenBw]);
    strcat(String, "k");
    UI_PrintStringSmallest(String, 108, 7, false, true);

#ifdef ENABLE_SPECTRUM_EXTENSIONS
    ShowChannelName(f);
#endif
}

static void TuneToPeak()
{
    SetF(peak.f);
    if (currentState == SPECTRUM)
    {
        ToggleRX(true);
    }
}

static void JumpToNextPeak(bool inc)
{
    uint16_t steps = GetStepsCount();
    uint32_t span = GetFEnd() - GetFStart();
    
    // Find current index in rssiHistory
    uint16_t currentIdx = (uint16_t)(128u * (fMeasure - GetFStart()) / span);
    if (currentIdx >= 128) currentIdx = 0;

    int8_t dir = inc ? 1 : -1;
    for (int i = 1; i < 128; i++) {
        uint8_t idx = (currentIdx + i * dir + 256) % 128;
        if (rssiHistory[idx] > settings.rssiTriggerLevel && rssiHistory[idx] != RSSI_MAX_VALUE) {
            SetF(GetFStart() + (uint32_t)idx * span / 128);
            if (currentState == SPECTRUM) ToggleRX(true);
            redrawScreen = true;
        }
    }
}

static void DrawNums()
{

    if (currentState == SPECTRUM)
    {
#ifdef ENABLE_SCAN_RANGES
        if (gScanRangeStart)
        {
            NUMBER_ToDecimal(String, GetStepsCountDisplay(), 3, false);
            strcat(String, "x");
        }
        else
#endif
        {
            NUMBER_ToDecimal(String, GetStepsCount(), 3, false);
            strcat(String, "x");
        }
        UI_PrintStringSmallest(String, 0, 1, false, true);
        NUMBER_ToDecimal(String, GetScanStep() / 100, 3, false);
        String[3] = '.';
        NUMBER_ToDecimal(String + 4, GetScanStep() % 100, 2, true);
        strcat(String, "k");
        UI_PrintStringSmallest(String, 0, 7, false, true);
    }

    if (IsCenterMode())
    {
        UI_PrintFrequencyEx(String, currentFreq, true);
        strcat(String, " \x7F");
        NUMBER_ToDecimal(String + strlen(String), settings.frequencyChangeStep / 100, 2, false);
        strcat(String, ".");
        NUMBER_ToDecimal(String + strlen(String), settings.frequencyChangeStep % 100, 2, true);
        strcat(String, "k");
#ifdef ENABLE_SPECTRUM_ADVANCED
        UI_PrintStringSmallest(String, 36, 33, false, true);
#else
        UI_PrintStringSmallest(String, 36, 49, false, true);
#endif
    }
    else
    {
        UI_PrintFrequencyEx(String, GetFStart(), true);
#ifdef ENABLE_SPECTRUM_ADVANCED
        UI_PrintStringSmallest(String, 0, 34, false, true);
#else
        UI_PrintStringSmallest(String, 0, 49, false, true);
#endif

        strcpy(String, "\x7F");
        NUMBER_ToDecimal(String + 1, settings.frequencyChangeStep / 100, 2, false);
        strcat(String, ".");
        NUMBER_ToDecimal(String + strlen(String), settings.frequencyChangeStep % 100, 2, true);
        strcat(String, "k");
#ifdef ENABLE_SPECTRUM_ADVANCED
        UI_PrintStringSmallest(String, 48, 34, false, true);
#else
        UI_PrintStringSmallest(String, 48, 49, false, true);
#endif

        UI_PrintFrequencyEx(String, GetFEnd(), true);
#ifdef ENABLE_SPECTRUM_ADVANCED
        UI_PrintStringSmallest(String, 93, 34, false, true);
#else
        UI_PrintStringSmallest(String, 93, 49, false, true);
#endif
    }
}

static void DrawRssiTriggerLevel()
{
    if (settings.rssiTriggerLevel == RSSI_MAX_VALUE || monitorMode)
        return;
    uint8_t y = Rssi2Y(settings.rssiTriggerLevel);
    uint8_t bank = y >> 3;
    uint8_t bit = 1 << (y & 7);

    for (uint8_t x = 0; x < 128; x += 2)
    {
        gFrameBuffer[bank][x] |= bit;
    }
}

static void DrawTicks()
{
    uint32_t f = GetFStart();
    uint32_t span = GetFEnd() - GetFStart();
    uint32_t step = span / 128;
    for (uint8_t i = 0; i < 128; i += (1 << settings.stepsCount))
    {
        f = GetFStart() + span * i / 128;
#ifdef ENABLE_SPECTRUM_ADVANCED
        uint8_t barValue = 0b00010000; // Bit 4 (Pixel 28)
        if ((f % 10000) < step)  barValue |= 0b00100000; // Pixel 29
        if ((f % 50000) < step)  barValue |= 0b01000000; // Pixel 30
        if ((f % 100000) < step) barValue |= 0b10000000; // Pixel 31
#else
        uint8_t barValue = 0b00010000; 
        (f % 10000) < step && (barValue |= 0b00100000);
        (f % 50000) < step && (barValue |= 0b01000000);
        (f % 100000) < step && (barValue |= 0b10000000);
#endif
        gFrameBuffer[3][i] |= barValue;
    }

    if (IsCenterMode())
    {
        // Brackets bits 0-3 (Pixels 24-27)
        memset(gFrameBuffer[3] + 62, 0x08, 5); 
        gFrameBuffer[3][64] = 0x0f;
    }
    else
    {
        memset(gFrameBuffer[3] + 1, 0x08, 3);
        memset(gFrameBuffer[3] + 124, 0x08, 3);
        gFrameBuffer[3][0] = 0x0f;
        gFrameBuffer[3][127] = 0x0f;
    }
}

static void DrawArrow(uint8_t x)
{
#ifdef ENABLE_SPECTRUM_ADVANCED
    for (signed i = -2; i <= 2; ++i)
    {
        signed v = (signed)x + i;
        if (v >= 0 && v < 128)
        {
            uint8_t column_pattern = 0;
            if (my_abs(i) == 0)      column_pattern = 0b11100000; // Center (bits 5,6,7)
            else if (my_abs(i) == 1) column_pattern = 0b11000000; // Inner (bits 6,7)
            else if (my_abs(i) == 2) column_pattern = 0b10000000; // Outer (bit 7)
            gFrameBuffer[3][v] |= column_pattern;
        }
    }
#else
    for (signed i = -2; i <= 2; ++i)
    {
        signed v = x + i;
        if (!(v & 128))
        {
            uint8_t column_pattern = 0;
            if (my_abs(i) == 0)      column_pattern = 0b11100000;
            else if (my_abs(i) == 1) column_pattern = 0b11000000;
            else if (my_abs(i) == 2) column_pattern = 0b10000000;
            gFrameBuffer[3][v] |= column_pattern;
        }
    }
#endif
}

static void OnKeyDown(uint8_t key)
{
    switch (key)
    {
    case KEY_UP:
#ifdef ENABLE_SCAN_RANGES
        if (!gScanRangeStart)
#endif
        {
            if (gFKeyActive) {
                gFKeyActive = false;
                JumpToNextPeak(true);
            } else {
                if (SPECTRUM_NAV_INVERTED)
                    UpdateCurrentFreq(false);
                else
                    UpdateCurrentFreq(true);
            }
        }
        break;
    case KEY_DOWN:
#ifdef ENABLE_SCAN_RANGES
        if (!gScanRangeStart)
#endif
        {
            if (gFKeyActive) {
                gFKeyActive = false;
                JumpToNextPeak(false);
            } else {
                if (SPECTRUM_NAV_INVERTED)
                    UpdateCurrentFreq(true);
                else
                    UpdateCurrentFreq(false);
            }
        }
        break;
    case KEY_SIDE1:
        Blacklist();
        break;
    case KEY_STAR:
        UpdateRssiTriggerLevel(true);
        break;
    case KEY_F:
        break;
    case KEY_0:
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
    case KEY_6:
    case KEY_7:
    case KEY_8:
    case KEY_9:
        if (!gFKeyActive) {
            FreqInput();
            freqInputTimer = 800; // ~2 seconds (Spectrum Tick is very fast)
            UpdateFreqInput(key);
        } else {
            gFKeyActive = false;
            switch(key) {
                case KEY_0: ToggleModulation(); break;
                case KEY_1: UpdateScanStep(true); break;
                case KEY_7: UpdateScanStep(false); break;
                case KEY_2: UpdateFreqChangeStep(true); break;
                case KEY_8: UpdateFreqChangeStep(false); break;
                case KEY_3: UpdateDBMax(true); break;
                case KEY_9: UpdateDBMax(false); break;
                case KEY_4: ToggleStepsCount(); break;
                case KEY_6: ToggleListeningBW(); break;
                case KEY_STAR: UpdateRssiTriggerLevel(false); break;
                default: break;
            }
        }
        break;
    case KEY_SIDE2:
        ToggleBacklight();
        break;
    case KEY_PTT:
        SetState(STILL);
        TuneToPeak();
        break;
    case KEY_MENU:
        break;
    case KEY_EXIT:
        if (menuState)
        {
            menuState = 0;
            break;
        }
#ifdef ENABLE_SPECTRUM_ADVANCED
        SaveSettings();
#endif
#ifdef ENABLE_BOOT_RESUME_STATE
        gEeprom.CURRENT_STATE = 0;
        SETTINGS_WriteCurrentState();
#endif
        DeInitSpectrum();
        break;
    default:
        break;
    }
}

static void OnKeyDownFreqInput(uint8_t key)
{
    switch (key)
    {
    case KEY_0:
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
    case KEY_6:
    case KEY_7:
    case KEY_8:
    case KEY_9:
    case KEY_STAR:
        UpdateFreqInput(key);
        freqInputTimer = 800; // Reset timeout
        break;
    case KEY_EXIT:
        if (freqInputIndex == 0)
        {
            SetState(previousState);
            break;
        }
        UpdateFreqInput(key);
        freqInputTimer = 800; // Reset timeout
        break;
    case KEY_MENU:
        if (tempFreq < F_MIN || tempFreq > F_MAX)
        {
            break;
        }
        SetState(previousState);
        currentFreq = tempFreq;
        if (currentState == SPECTRUM)
        {
            ResetBlacklist();
            RelaunchScan();
        }
        else
        {
            SetF(currentFreq);
        }
        break;
    default:
        break;
    }
}

void OnKeyDownStill(KEY_Code_t key)
{
    switch (key)
    {
    case KEY_UP:
        if (menuState)
        {
            if (SPECTRUM_NAV_INVERTED)
                SetRegMenuValue(menuState, false);
            else
                SetRegMenuValue(menuState, true);
            break;
        }

        if (gFKeyActive) {
            gFKeyActive = false;
            JumpToNextPeak(true);
        } else {
            if (SPECTRUM_NAV_INVERTED)
                UpdateCurrentFreqStill(false);
            else
                UpdateCurrentFreqStill(true);
        }
        break;
    case KEY_DOWN:
        if (menuState)
        {
            if (SPECTRUM_NAV_INVERTED)
                SetRegMenuValue(menuState, true);
            else
                SetRegMenuValue(menuState, false);
            break;
        }

        if (gFKeyActive) {
            gFKeyActive = false;
            JumpToNextPeak(false);
        } else {
            if (SPECTRUM_NAV_INVERTED)
                UpdateCurrentFreqStill(true);
            else
                UpdateCurrentFreqStill(false);
        }
        break;
    case KEY_STAR:
        UpdateRssiTriggerLevel(true);
        break;
    case KEY_F:
        break;
    case KEY_0:
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
    case KEY_6:
    case KEY_7:
    case KEY_8:
    case KEY_9:
        if (!gFKeyActive) {
            FreqInput();
            freqInputTimer = 800; // Reset timeout
            UpdateFreqInput(key);
        } else {
            gFKeyActive = false;
            switch(key) {
                case KEY_0: ToggleModulation(); break;
                case KEY_3: UpdateDBMax(true); break;
                case KEY_9: UpdateDBMax(false); break;
                case KEY_6: ToggleListeningBW(); break;
                default: break;
            }
        }
        break;
    case KEY_SIDE1:
        monitorMode = !monitorMode;
        break;
    case KEY_SIDE2:
        ToggleBacklight();
        break;
    case KEY_PTT:
        break;
    case KEY_MENU:
        if (menuState == ARRAY_SIZE(registerSpecs) - 1)
        {
            menuState = 1;
        }
        else
        {
            menuState++;
        }
        redrawScreen = true;
        break;
    case KEY_EXIT:
        if (!menuState)
        {
            SetState(SPECTRUM);
            lockAGC = false;
            monitorMode = false;
            RelaunchScan();
            break;
        }
        menuState = 0;
        break;
    default:
        break;
    }
}

static void RenderFreqInput() { UI_PrintString(freqInputString, 2, 127, 0, 8); }

static void RenderStatus()
{
    memset(gStatusLine, 0, sizeof(gStatusLine));
    DrawStatus();
    ST7565_BlitStatusLine();
}

static void RenderSpectrum()
{
#ifdef ENABLE_SPECTRUM_ADVANCED
    DrawTicks();
    DrawSpectrumEnhanced();
    DrawNums();
    DrawWaterfall();
    DrawArrow(128u * peak.i / GetStepsCount());
    DrawRssiTriggerLevel();
    DrawF(peak.f);
#else
    DrawTicks();
    DrawSpectrum();
    DrawNums();
    DrawArrow(128u * peak.i / GetStepsCount());
    DrawRssiTriggerLevel();
    DrawF(peak.f);
#endif
}

static void RenderStill()
{
    DrawF(fMeasure);

    const uint8_t METER_PAD_LEFT = 3;

    memset(&gFrameBuffer[2][METER_PAD_LEFT], 0b00010000, 121);

    for (int i = 0; i < 121; i += 5)
    {
        gFrameBuffer[2][i + METER_PAD_LEFT] = 0b00110000;
    }

    for (int i = 0; i < 121; i += 10)
    {
        gFrameBuffer[2][i + METER_PAD_LEFT] = 0b01110000;
    }

    uint8_t x = Rssi2PX(scanInfo.rssi, 0, 121);
    for (int i = 0; i < x; ++i)
    {
        if (i % 5)
        {
            gFrameBuffer[2][i + METER_PAD_LEFT] |= 0b00000111;
        }
    }

    int dbm = Rssi2DBm(scanInfo.rssi);
    uint8_t s = DBm2S(dbm);
    // sprintf(String, "S: %u", s);
    strcpy(String, "S: ");
    String[3] = s + '0';
    String[4] = '\0';
    UI_PrintStringSmallest(String, 4, 25, false, true);
    
    // sprintf(String, "%d dBm", dbm);
    NUMBER_ToDecimal(String, dbm, 4, false);
    strcat(String, " dBm");
    UI_PrintStringSmallest(String, 28, 25, false, true);

    if (!monitorMode)
    {
        uint8_t x = Rssi2PX(settings.rssiTriggerLevel, 0, 121);
        gFrameBuffer[2][METER_PAD_LEFT + x] = 0b11111111;
    }

    const uint8_t PAD_LEFT = 4;
    const uint8_t CELL_WIDTH = 30;
    uint8_t offset = PAD_LEFT;
    uint8_t row = 4;

    for (int i = 0, idx = 1; idx <= 4; ++i, ++idx)
    {
        if (idx == 5)
        {
            row += 2;
            i = 0;
        }
        offset = PAD_LEFT + i * CELL_WIDTH;
        if (menuState == idx)
        {
            for (int j = 0; j < CELL_WIDTH; ++j)
            {
                gFrameBuffer[row][j + offset] = 0xFF;
                gFrameBuffer[row + 1][j + offset] = 0xFF;
            }
        }
        // sprintf(String, "%s", registerSpecs[idx].name);
        strcpy(String, registerSpecs[idx].name);
        UI_PrintStringSmallest(String, offset + 2, row * 8 + 2, false,
                            menuState != idx);

#ifdef ENABLE_SPECTRUM_EXTENSIONS
        if(idx == 1)
        {
            NUMBER_ToDecimal(String, LNAsOptions[GetRegMenuValue(idx)], 3, false);
            strcat(String, "dB");
        }
        else if(idx == 2)
        {
            NUMBER_ToDecimal(String, LNAOptions[GetRegMenuValue(idx)], 3, false);
            strcat(String, "dB");
        }
        else if(idx == 3)
        {
            NUMBER_ToDecimal(String, VGAOptions[GetRegMenuValue(idx)], 3, false);
            strcat(String, "dB");
        }
        else if(idx == 4)
        {
            strcpy(String, BPFOptions[(GetRegMenuValue(idx) / 0x2aaa)]);
            strcat(String, "kHz");
        }
#else
        NUMBER_ToDecimal(String, GetRegMenuValue(idx), 4, false);
#endif
        UI_PrintStringSmallest(String, offset + 2, (row + 1) * 8 + 1, false,
                            menuState != idx);
    }
}

static void Render()
{
    UI_DisplayClear();

    switch (currentState)
    {
    case SPECTRUM:
        RenderSpectrum();
        break;
    case FREQ_INPUT:
        RenderFreqInput();
        break;
    case STILL:
        RenderStill();
        break;
    }

    ST7565_BlitFullScreen();
}

static bool HandleUserInput()
{
    kbd.prev = kbd.current;
    kbd.current = GetKey();

    if (kbd.current != KEY_INVALID && kbd.current == kbd.prev)
    {
        if (kbd.counter < 16)
            kbd.counter++;
        else
            kbd.counter -= 3;
        SYSTEM_DelayMs(20);
    }
    else
    {
        kbd.counter = 0;
    }

    if (kbd.counter == 3 || kbd.counter == 16)
    {
        if (kbd.current == KEY_F && kbd.counter == 3) {
            gFKeyActive = !gFKeyActive;
            redrawScreen = true;
            return true;
        }

        switch (currentState)
        {
        case SPECTRUM:
            OnKeyDown(kbd.current);
            break;
        case FREQ_INPUT:
            OnKeyDownFreqInput(kbd.current);
            break;
        case STILL:
            OnKeyDownStill(kbd.current);
            break;
        }
    }

    return true;
}

static void Scan()
{
    if (rssiHistory[scanInfo.i] != RSSI_MAX_VALUE
#ifdef ENABLE_SCAN_RANGES
        && !IsBlacklisted(scanInfo.i)
#endif
    )
    {
        SetF(scanInfo.f);
        Measure();
        UpdateScanInfo();
    }
}

static void NextScanStep()
{
    ++peak.t;
    ++scanInfo.i;
    scanInfo.f += scanInfo.scanStep;
}

static void UpdateScan()
{
    Scan();
#ifdef ENABLE_SPECTRUM_ADVANCED
    if (scanInfo.i < scanInfo.measurementsCount)
    {
        uint8_t oldRssi = (uint8_t)rssiHistory[scanInfo.i];
        if (scanInfo.rssi > oldRssi) rssiHistory[scanInfo.i] = scanInfo.rssi;
        else {
            const uint8_t DECAY_STEP = 2;
            if (oldRssi > (scanInfo.rssi + DECAY_STEP)) rssiHistory[scanInfo.i] = oldRssi - DECAY_STEP;
            else rssiHistory[scanInfo.i] = scanInfo.rssi;
        }
    }
#endif

    if (scanInfo.i < scanInfo.measurementsCount)
    {
        NextScanStep();
        return;
    }

    if (! (scanInfo.measurementsCount >> 7)) // if (scanInfo.measurementsCount < 128)
        memset(&rssiHistory[scanInfo.measurementsCount], 0,
               sizeof(rssiHistory) - scanInfo.measurementsCount * sizeof(rssiHistory[0]));

    redrawScreen = true;
    preventKeypress = false;

    UpdatePeakInfo();
    if (IsPeakOverLevel())
    {
        ToggleRX(true);
        TuneToPeak();
        return;
    }

    newScanStart = true;
}

static void UpdateStill()
{
    Measure();
#ifdef ENABLE_SPECTRUM_ADVANCED
    if (displayRssi == 0) displayRssi = scanInfo.rssi;
    else displayRssi = (displayRssi * 9 + scanInfo.rssi) / 10;
#endif
    redrawScreen = true;
    preventKeypress = false;

    peak.rssi = scanInfo.rssi;
    AutoTriggerLevel();

    if (IsPeakOverLevel() || monitorMode) {
        ToggleRX(true);
    }
}

static void UpdateListening()
{
    preventKeypress = false;

    if (listenT)
    {
        listenT--;
        return;
    }

    // --- THE HELICOPTER FIX (THROTTLED EXECUTION) ---
    // We only talk to the hardware and update the screen every 8th cycle.
    // This removes the "helicopter" buzzing/clicking from the SPI bus.
    static uint8_t quietCounter = 0;
    if (++quietCounter >= 8)
    {
        quietCounter = 0;

        Measure();
        peak.rssi = scanInfo.rssi;
        redrawScreen = true;

        if (IsPeakOverLevel() || monitorMode)
        {
            listenT = 2; // Tiny hold until next throttle check
            return;
        }

        ToggleRX(false);
        ResetScanStats();
    }
}

static void Tick()
{
#ifdef ENABLE_AM_FIX
    if (gNextTimeslice)
    {
        gNextTimeslice = false;
        if (settings.modulationType == MODULATION_AM && !lockAGC)
        {
            AM_fix_10ms(vfo); // allow AM_Fix to apply its AGC action
        }
    }
#endif

#ifdef ENABLE_SCAN_RANGES
    if (gNextTimeslice_500ms)
    {
        gNextTimeslice_500ms = false;

        // if a lot of steps then it takes long time
        // we don't want to wait for whole scan
        // listening has it's own timer
        if (GetStepsCount() > 128 && !isListening)
        {
            UpdatePeakInfo();
            if (IsPeakOverLevel())
            {
                ToggleRX(true);
                TuneToPeak();
                return;
            }
            redrawScreen = true;
            preventKeypress = false;
        }
    }
#endif

    if (!preventKeypress)
    {
        HandleUserInput();
    }
    if (newScanStart)
    {
        InitScan();
        newScanStart = false;
    }
    if (isListening && currentState != FREQ_INPUT)
    {
        UpdateListening();
    }

    if (currentState == FREQ_INPUT && freqInputIndex > 0)
    {
        if (freqInputTimer > 0) {
            freqInputTimer--;
        } else {
            // Auto-confirm frequency
            if (tempFreq >= F_MIN && tempFreq <= F_MAX) {
                SetState(previousState);
                currentFreq = tempFreq;
                if (currentState == SPECTRUM) {
                    ResetBlacklist();
                    RelaunchScan();
                } else {
                    SetF(currentFreq);
                }
            } else {
                SetState(previousState); // Cancel if invalid
            }
        }
    }
    else
    {
        if (currentState == SPECTRUM)
        {
            UpdateScan();
        }
        else if (currentState == STILL)
        {
            UpdateStill();
        }
    }
    if (redrawStatus || ++statuslineUpdateTimer > 4096)
    {
        RenderStatus();
        redrawStatus = false;
        statuslineUpdateTimer = 0;
    }
    if (redrawScreen)
    {
        Render();
        // For screenshot
        #ifdef ENABLE_SERIAL_SCREENCAST
            getScreenShot(false);
        #endif
        redrawScreen = false;
    }
}

void APP_RunSpectrum()
{
    // TX here coz it always? set to active VFO
    vfo = gEeprom.TX_VFO;

#ifdef ENABLE_SPECTRUM_ADVANCED
    LoadSettings();
#endif

    // Capture original state for restoration on exit
    originalFreq = gTxVfo->pRX->Frequency;
    originalModulation = gTxVfo->Modulation;
    originalChannelSave = gTxVfo->CHANNEL_SAVE;

    // set the current frequency in the middle of the display
#ifdef ENABLE_SCAN_RANGES
    if (gScanRangeStart)
    {
        currentFreq = initialFreq = gScanRangeStart;
        for (uint8_t i = 0; i < ARRAY_SIZE(scanStepValues); i++)
        {
            if (scanStepValues[i] >= gTxVfo->StepFrequency)
            {
                settings.scanStepIndex = i;
                break;
            }
        }
        settings.stepsCount = STEPS_128;
        #ifdef ENABLE_BOOT_RESUME_STATE
            gEeprom.CURRENT_STATE = 5;
        #endif
    }
    else {
#endif
        currentFreq = initialFreq = gTxVfo->pRX->Frequency -
                                    ((GetStepsCount() / 2) * GetScanStep());
        #ifdef ENABLE_BOOT_RESUME_STATE
            gEeprom.CURRENT_STATE = 4;
        #endif
#ifdef ENABLE_SCAN_RANGES
    }
#endif

    #ifdef ENABLE_BOOT_RESUME_STATE
        SETTINGS_WriteCurrentState();
    #endif

    BackupRegisters();

    isListening = true; // to turn off RX later
    redrawStatus = true;
    redrawScreen = true;
    newScanStart = true;

    ToggleRX(true), ToggleRX(false); // hack to prevent noise when squelch off
    RADIO_SetModulation(settings.modulationType = gTxVfo->Modulation);

#ifdef ENABLE_SPECTRUM_EXTENSIONS
    BK4819_SetFilterBandwidth(settings.listenBw, gTxVfo->Modulation, false);
#else
    BK4819_SetFilterBandwidth(settings.listenBw = BK4819_FILTER_BW_WIDE, gTxVfo->Modulation, false);
#endif

    RelaunchScan();

    memset(rssiHistory, 0, sizeof(rssiHistory));
#ifdef ENABLE_SPECTRUM_ADVANCED
    memset(waterfallHistory, 0, sizeof(waterfallHistory));
    waterfallIndex = 0;
#endif

    isInitialized = true;

    while (isInitialized)
    {
        Tick();
    }
}