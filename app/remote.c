/* Remote SDR-style control over UART
 *
 * The radio becomes a swept-tuned spectrum front end for a PC: the host
 * (tools/remote.html, Web Serial) configures a sweep, the radio streams one
 * RSSI line per sweep, and the host can switch to LISTEN on any frequency
 * (audio comes out of the radio's speaker). Independent of the on-radio
 * spectrum analyzers — it has its own sweep engine.
 *
 * Transport: the stock Quansheng/egzumer UART framing (app/uart.c), command
 * IDs 0x0Axx, little endian. Protocol: tools/REMOTE.md.
 *
 * Leaving: the EXIT key, a STOP command, or 5 s without a host packet
 * restores the VFO and BK4819 registers. PTT is NOT an exit: on the K5 the
 * UART RX line shares the PTT contact, so host traffic reads as PTT presses
 * (the stock firmware masks PTT with SerialConfigInProgress() for the same
 * reason; every remote command re-arms that mask). After EXIT only START,
 * SWEEP or LISTEN re-enter remote mode, keep-alives do not.
 */
#ifdef ENABLE_REMOTE

#include <string.h>

#include "app/remote.h"
#include "app/uart.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/ui.h"

extern uint32_t millis10(void);
void UART_SendPacket(void *pReply, uint16_t Size);           // app/uart.c

#define CMD_START      0x0A01
#define CMD_STOP       0x0A02
#define CMD_SWEEP      0x0A03
#define CMD_LISTEN     0x0A04
#define CMD_KEEPALIVE  0x0A05
#define CMD_BAUD       0x0A06
#define RPL_STATUS     0x0A81
#define RPL_SWEEP      0x0A82

#define MAX_POINTS     256
#define HOST_TIMEOUT   500      // x10 ms
#define BAUD_PROBE     200      // x10 ms: no packet at the new speed -> back to 38400
#define BAUD_DEFAULT   38400

enum { M_SWEEP, M_LISTEN };

volatile bool gRemoteRequest;

static struct {
    uint32_t start, step;       // 10 Hz units
    uint16_t points;
    uint8_t  bw;                // 0 = 25k, 1 = 12.5k, 2 = 6.25k (IF filter while sweeping)
    uint8_t  dwell;             // extra settle time per point, x100 us
} sw = { 43300000, 2500, 128, 2, 0 };

static struct {
    uint32_t freq;
    uint8_t  mod;               // ModulationMode_t
    uint8_t  bw;                // 0 = 25k, 1 = 12.5k, 2 = 6.25k
    uint8_t  sq;                // squelch, dBm + 160 (0 = open)
    uint8_t  monitor;
} ls = { 43350000, 0, 0, 0, 0 };

static volatile uint8_t mode;
static volatile bool    running, dirty, wantStatus, hostSeen, baudOk;
static volatile uint32_t lastHost, newBaud;
static uint32_t curBaud = BAUD_DEFAULT, baudSwitchT;
static uint16_t sweepMs;
static bool     audioOn;
static uint8_t  lastRssi;

static const uint16_t bwReg43[3] = {
    0b0011011000101000,         // 25 kHz
    0b0111111100001000,         // 12.5 kHz
    0b0100100001011000,         // 6.25 kHz
};

static const BK4819_REGISTER_t saveRegs[] = {
    BK4819_REG_30, BK4819_REG_37, BK4819_REG_3D, BK4819_REG_43,
    BK4819_REG_47, BK4819_REG_48, BK4819_REG_7E,
};
static uint16_t savedRegs[ARRAY_SIZE(saveRegs)];

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

// ─────────────────────────── UART side ───────────────────────────

void REMOTE_HandleCommand(const uint8_t *p)
{
    const uint16_t id = rd16(p);
    const uint8_t *d  = p + 4;                                // after the 4-byte inner header

    lastHost = millis10();
    hostSeen = true;
    baudOk   = true;                                          // a packet made it at the current speed
    gSerialConfigCountDown_500ms = 12;                        // mask PTT: host traffic toggles it
    switch (id) {
        case CMD_START:
            mode = M_SWEEP;
            break;
        case CMD_STOP:
            running = false;
            return;
        case CMD_SWEEP:
            sw.start  = rd32(d);
            sw.step   = rd32(d + 4);
            sw.points = rd16(d + 8);
            sw.bw     = d[10] % 3;
            sw.dwell  = d[11];
            if (sw.points == 0 || sw.points > MAX_POINTS)
                sw.points = 128;
            mode = M_SWEEP;
            break;
        case CMD_LISTEN:
            ls.freq    = rd32(d);
            ls.mod     = d[4] < MODULATION_UKNOWN ? d[4] : 0;
            ls.bw      = d[5] % 3;
            ls.sq      = d[6];
            ls.monitor = d[7] & 1;
            mode = M_LISTEN;
            break;
        case CMD_KEEPALIVE:
            wantStatus = true;                                // never (re)enters remote mode
            return;
        case CMD_BAUD:                                        // only inside a session
            if (running) {
                const uint32_t b = rd32(d);
                if (b == 38400 || b == 57600 || b == 115200 || b == 230400)
                    newBaud = b;
            }
            return;
        default:
            return;
    }
    dirty = true;                                             // (re)apply settings, send status
    if (!running)
        gRemoteRequest = true;
}

static void sendStatus(void)
{
    struct __attribute__((packed)) {
        uint16_t id, size;
        uint8_t  mode, mod, bw, battery;
        uint32_t freq, start, step;
        uint16_t points;
        uint8_t  rssi, sqOpen;
        uint16_t sweepMs;
        uint32_t baud;
    } s;
    s.id      = RPL_STATUS;
    s.size    = sizeof(s) - 4;
    s.mode    = mode;
    s.mod     = ls.mod;
    s.bw      = mode == M_LISTEN ? ls.bw : sw.bw;
    s.battery = BATTERY_VoltsToPercent(gBatteryVoltageAverage);
    s.freq    = ls.freq;
    s.start   = sw.start;
    s.step    = sw.step;
    s.points  = sw.points;
    s.rssi    = lastRssi;
    s.sqOpen  = audioOn;
    s.sweepMs = sweepMs;
    s.baud    = curBaud;
    UART_SendPacket(&s, sizeof(s));
}

static void setBaud(uint32_t b)
{
    SYSTEM_DelayMs(15);                                       // let the TX FIFO drain
    UART_Init((b * 1017u + 500u) / 1000u);                    // same scale as UART_BAUD_*_CLOCK_DIV
    CECHWUartClearBuffer();                                   // DMA restarted at index 0
    curBaud = b;
}

// ─────────────────────────── radio side ───────────────────────────

static uint16_t reg30;
static int8_t   lastPath = -1;

static void tune(uint32_t f)
{
    BK4819_SetFrequency(f);
    const int8_t path = f >= 28000000;                        // VHF / UHF front-end switch
    if (path != lastPath) {
        lastPath = path;
        BK4819_PickRXFilterPathBasedOnFrequency(f);
    }
    BK4819_WriteRegister(BK4819_REG_30, 0);                   // restart RSSI measurement
    BK4819_WriteRegister(BK4819_REG_30, reg30);
}

// RSSI as dBm + 160 (BK4819 raw RSSI is dBm*2 + 320)
static uint8_t measure(void)
{
    for (uint8_t t = 0; t < 30 && (BK4819_ReadRegister(BK4819_REG_63) & 0xFF) >= 255; t++)
        SYSTICK_DelayUs(100);                                 // wait for the AGC/glitch detector
    const uint16_t raw = BK4819_GetRSSI() >> 1;
    return raw > 255 ? 255 : raw;
}

static void audio(bool on)
{
    if (on == audioOn)
        return;
    audioOn = on;
    uint16_t r30 = BK4819_ReadRegister(BK4819_REG_30) & ~(1u << 9);
    uint16_t r47 = BK4819_ReadRegister(BK4819_REG_47) & ~(1u << 8);
    if (on) {
        r30 |= 1u << 9;                                       // AF DAC
        r47 |= 1u << 8;                                       // AF out
        AUDIO_AudioPathOn();
    } else
        AUDIO_AudioPathOff();
    BK4819_WriteRegister(BK4819_REG_30, r30);
    BK4819_WriteRegister(BK4819_REG_47, r47);
    BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, on);
}

static void applyListen(void)
{
    lastPath = -1;
    gRxVfo->pRX->Frequency = ls.freq;
    gRxVfo->Modulation     = ls.mod;
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
    RADIO_SetupRegisters(false);                              // full RX chain for this freq/mode
    RADIO_SetModulation(ls.mod);
    RADIO_SetupAGC(ls.mod == MODULATION_AM, false);
    BK4819_WriteRegister(BK4819_REG_43, bwReg43[ls.bw]);
    audioOn = true;
    audio(false);
}

static void sweep(void)
{
    struct __attribute__((packed)) {
        uint16_t id, size;
        uint32_t start, step;
        uint16_t points;
        uint8_t  rssi[MAX_POINTS];
    } pkt;

    audio(false);
    BK4819_WriteRegister(BK4819_REG_43, bwReg43[sw.bw]);
    reg30    = BK4819_ReadRegister(BK4819_REG_30);
    lastPath = -1;
    const uint32_t t0 = millis10();

    pkt.start  = sw.start;
    pkt.step   = sw.step;
    pkt.points = sw.points;
    uint32_t f = sw.start;
    for (uint16_t i = 0; i < pkt.points; i++, f += sw.step) {
        tune(f);
        if (sw.dwell)
            SYSTICK_DelayUs(sw.dwell * 100u);
        pkt.rssi[i] = measure();
    }
    sweepMs  = (millis10() - t0) * 10;
    pkt.id   = RPL_SWEEP;
    pkt.size = 10 + pkt.points;
    UART_SendPacket(&pkt, 14 + pkt.points);
}

static void draw(void)
{
    char s[22];
    UI_DisplayClear();
    UI_PrintStringSmallNormal("REMOTE SDR", 0, 127, 0);
    if (mode == M_LISTEN) {
        sprintf(s, "RX %u.%05u", ls.freq / 100000, ls.freq % 100000);
        UI_PrintStringSmallNormal(s, 0, 127, 2);
        sprintf(s, "%s %d dBm %s", gModulationStr[ls.mod], lastRssi - 160, audioOn ? "SQ" : "");
    } else {
        sprintf(s, "%u.%03u +%u", sw.start / 100000, (sw.start / 100) % 1000, sw.points);
        UI_PrintStringSmallNormal(s, 0, 127, 2);
        sprintf(s, "STEP %u.%02ukHz", sw.step / 100, sw.step % 100);
    }
    UI_PrintStringSmallNormal(s, 0, 127, 4);
    if (!hostSeen)
        strcpy(s, "WAITING FOR PC");
    else
        sprintf(s, "%lu BD %ums", (unsigned long)curBaud, sweepMs);
    UI_PrintStringSmallNormal(s, 0, 127, 5);
    UI_PrintStringSmallNormal("EXIT: quit", 0, 127, 6);
    ST7565_BlitFullScreen();
}

// Local entry from the keypad: wait for the PC without the host timeout.
void REMOTE_Open(void)
{
    hostSeen = false;
    REMOTE_Run();
}

void REMOTE_Run(void)
{
    const uint32_t origFreq = gRxVfo->pRX->Frequency;
    const uint8_t  origMod  = gRxVfo->Modulation;

    gRemoteRequest = false;
    running        = true;
    newBaud        = 0;
    baudOk         = true;
    dirty          = true;
    audioOn        = true;
    lastHost       = millis10();

    for (uint8_t i = 0; i < ARRAY_SIZE(saveRegs); i++)
        savedRegs[i] = BK4819_ReadRegister(saveRegs[i]);
    audio(false);

    uint8_t  prevMode = 0xFF;
    uint32_t lastDraw = 0, lastStat = 0;

    while (running) {
        if (UART_IsCommandAvailable())
            UART_HandleCommand();

        if (KEYBOARD_Poll() == KEY_EXIT || (hostSeen && millis10() - lastHost > HOST_TIMEOUT))
            break;

        if (newBaud) {                                        // ack at the old speed, then switch
            const uint32_t b = newBaud;
            newBaud = 0;
            sendStatus();
            setBaud(b);
            baudOk      = (b == BAUD_DEFAULT);
            baudSwitchT = millis10();
        }
        if (!baudOk && millis10() - baudSwitchT > BAUD_PROBE) {
            setBaud(BAUD_DEFAULT);                            // host never came back: fall back
            baudOk = true;
        }

        if (dirty) {
            dirty = false;
            if (mode == M_LISTEN)
                applyListen();
            else if (prevMode != M_SWEEP)
                RADIO_SetModulation(MODULATION_FM);
            prevMode   = mode;
            wantStatus = true;
            lastDraw   = 0;
        }
        if (wantStatus) {
            wantStatus = false;
            sendStatus();
        }

        const uint32_t now = millis10();
        if (mode == M_SWEEP) {
            sweep();
        } else {
            lastRssi = measure();
            audio(ls.monitor || lastRssi >= ls.sq);
            if (now - lastStat >= 10) {                       // S-meter at ~10 Hz
                lastStat = now;
                sendStatus();
            }
            SYSTEM_DelayMs(5);
        }
        if (now - lastDraw >= 50) {
            lastDraw = now;
            draw();
        }
    }

    // restore everything
    running = false;
    if (curBaud != BAUD_DEFAULT)
        setBaud(BAUD_DEFAULT);                                // CHIRP / k5prog expect 38400
    audio(false);
    for (uint8_t i = 0; i < ARRAY_SIZE(saveRegs); i++)
        BK4819_WriteRegister(saveRegs[i], savedRegs[i]);
    gRxVfo->pRX->Frequency = origFreq;
    gRxVfo->Modulation     = origMod;
    RADIO_ConfigureChannel(gEeprom.RX_VFO, VFO_CONFIGURE_RELOAD);
    RADIO_SetupRegisters(true);
    gRequestDisplayScreen = DISPLAY_MAIN;
    gUpdateStatus  = true;
    gUpdateDisplay = true;
}

#endif // ENABLE_REMOTE
