# Hermes Link for the CEC UV-K5 firmware

Port of the [Hermes Link](https://github.com/qshosfw/hermes) FSK mesh protocol
(reference C code: qshosfw/deltafw-k1-k5v3, GPL-3.0) to the DP32G030 UV-K5.
Because of this code, a firmware built with `ENABLE_HERMES=1` is GPL-3.0.

## Air interface
FFSK 1200/1800 Bd on the BK4819 modem, 128-byte frames, sync word `0x2F2A11DB`,
PN15 whitening, RS(128,96) FEC, ChaCha20-Poly1305 inner (end-to-end) and outer
(hop) seal, controlled flooding with RSSI-weighted backoff, stop-and-wait ARQ,
discovery beacons. Hermes listens in the background on the current VFO
frequency (FM only) — use a simplex channel shared by all nodes.

## Setup
* Menu **U.Info → MY CALL**: callsign = node address (Base40, max 9 chars).
* Menu **U.Info → MESHKEY**: network passphrase (empty = open network).
  All nodes of a network need the same MESHKEY.
* Long **MENU** on the main screen opens Hermes, **F** = settings,
  switch **HERMES ON**.

## Keys
| Screen | Keys |
|---|---|
| Chat | UP/DOWN scroll, MENU write, `*` neighbours, F settings, EXIT back |
| Compose | multi-tap 0-9 (`1` = `.,?!@-/:`), `*` delete (hold = clear), UP/DOWN recipient, MENU send, `@CALL text` = direct message |
| Neighbours | UP/DOWN select, MENU write to, `*` send beacon now |
| Settings | UP/DOWN, MENU toggle, EXIT save |

Status after a sent message: `...` sending, `OK` ACKed (unicast), `RLY` heard
relayed (broadcast), `SENT`, `FAIL`.

## Notes
* Battery save is suspended while Hermes is active.
* Settings (flags, TTL) are stored in EEPROM 0x1F90..0x1F97.
* Not interoperable with the current deltafw Hermes build (it transmits with a
  zero sync word and has several pipeline bugs, see hermes.c header).
