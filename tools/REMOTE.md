# Remote SDR-style control over UART

`ENABLE_REMOTE=1` (or `make PROFILE=sdr`) lets a PC drive the radio over the
programming cable: the radio sweeps a frequency range and streams one RSSI
line per sweep (panorama + waterfall on the PC), and the PC can switch the
radio to listen on any frequency. Audio stays on the radio's speaker.

It is a swept-tuned analyzer, not an IQ SDR: the BK4819 has hardware
demodulators and no IQ output. Resolution bandwidth is the IF filter
(6.25 / 12.5 / 25 kHz); one 128-point sweep takes roughly 0.2-0.4 s.

Client: `tools/remote.html` (Chromium-based browser, Web Serial). Open it
from disk or any https/localhost origin.

## Entering / leaving

* The radio enters remote mode on `START`, `SWEEP` or `LISTEN` (not while
  transmitting), or from the keypad: long MENU (F + long MENU when Hermes is
  also built). Entered from the keypad it shows `WAITING FOR PC` and has no
  host timeout until the first packet arrives. `KEEPALIVE` keeps a session alive but never re-enters it,
  so the EXIT key on the radio really hands control back.
* It leaves on `STOP`, the EXIT key, or 5 s without any host packet, and
  restores the VFO and BK4819 registers.
* PTT is ignored while the host talks: the K5 UART RX shares the PTT contact
  of the K-plug, so serial traffic looks like PTT presses (stock firmware
  masks PTT during CHIRP sessions the same way; every remote command re-arms
  that 6 s mask).

## Transport

Stock Quansheng/egzumer framing (same as CHIRP), 38400 8N1, little endian.

```
host -> radio:  AB CD | len:u16 | body[len] | crc:u16 | DC BA
radio -> host:  AB CD | len:u16 | body[len] | pad:u16 | DC BA
body          = id:u16 | size:u16 | data[size]
crc           = CRC-16/XMODEM (poly 0x1021, init 0) over the plain body
```

`body` (+ `crc` / `pad`) may be XOR-obfuscated with the 16-byte key
`16 6C 14 E6 2E 91 0D 40 21 35 D5 40 13 03 E9 80` (index `i % 16`). The
radio chooses the mode from the *raw* header of the hello `0x0514`
(`data = timestamp:u32`): a plain hello starts a plain session, an
obfuscated one (raw id reads `0x6902`) an obfuscated session, and the
radio's replies follow. Every later packet must use the same mode as the
hello. `tools/remote.html` sends a plain hello.

## Host -> radio

| id | name | data |
|---|---|---|
| 0x0A01 | START | - (enter remote, sweep mode) |
| 0x0A02 | STOP | - |
| 0x0A03 | SWEEP | start:u32, step:u32 (10 Hz units), points:u16 (1..256), ifbw:u8 (0 = 25k, 1 = 12.5k, 2 = 6.25k), dwell:u8 (extra settle per point, x100 us) |
| 0x0A04 | LISTEN | freq:u32 (10 Hz), mod:u8 (0 FM, 1 AM, 2 SSB, 3 CW), bw:u8 (0 = 25k, 1 = 12.5k, 2 = 6.25k), squelch:u8 (dBm + 160, 0 = open), flags:u8 (bit0 = monitor) |
| 0x0A05 | KEEPALIVE | - (send at least every 5 s; answered with STATUS) |
| 0x0A06 | BAUD | baud:u32 (38400, 57600, 115200, 230400). Only inside a session. The radio answers with STATUS at the old speed, then switches; if no valid packet arrives at the new speed within 2 s it falls back to 38400. Leaving remote mode always restores 38400 (CHIRP, k5prog). |

## Radio -> host

| id | name | data |
|---|---|---|
| 0x0A81 | STATUS | mode:u8 (0 sweep, 1 listen), mod:u8, bw:u8, battery:u8 (%), freq:u32, start:u32, step:u32, points:u16, rssi:u8 (dBm + 160), squelchOpen:u8, sweepMs:u16 (time the last sweep took on the radio), baud:u32 |
| 0x0A82 | SWEEP | start:u32, step:u32, points:u16, rssi[points]:u8 (dBm + 160) |

`STATUS` is sent after every accepted command and at ~10 Hz while listening.
