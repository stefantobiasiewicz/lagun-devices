# `output` — one relay

ESP32-C3 Super Mini + SN65HVD230 CAN transceiver + a one-channel relay module switched
by a LOW level. `DeviceType` 0x0020, endpoint 4 (`Output`, `Invert = 1`). Pins are the
defaults of `idf.py menuconfig` → *LagunBus — output*; power and the general rules are
in [`../../README.md`](../../README.md) § Sprzęt.

```
        ESP32-C3 Super Mini                         SN65HVD230 (3,3 V)
       ┌───────────────────┐                       ┌──────────────┐
  USB ─┤ USB-C         3V3 ├───────────────────────┤ 3V3          │
       │               GND ├──┬────────────────────┤ GND          │
       │             GPIO4 ├──┼────────────────────┤ TXD (D)      │
       │             GPIO5 ├──┼────────────────────┤ RXD (R)      │
       │                   │  │                    │        CANH  ├──── bus CANH
       │                   │  │                    │        CANL  ├──── bus CANL
       │                   │  │                    └──────────────┘
       │                   │  │                     relay module
       │                   │  │                    ┌──────────────┐
       │            GPIO10 ├──┼────────────────────┤ IN           │
       │                   │  └────────────────────┤ GND          │
       │                   │       separate 5 V ───┤ VCC          │
       │ BOOT (GPIO9)      │                       │  COM NO NC   ├──── load
       │ LED  (GPIO8)      │                       └──────────────┘
       └───────────────────┘
```

## Pin to pin

| from | to | what |
|---|---|---|
| Super Mini `3V3` | SN65HVD230 `3V3` | transceiver supply — **3.3 V only** |
| Super Mini `GND` | SN65HVD230 `GND` | |
| Super Mini `GPIO4` | SN65HVD230 `TXD` / `D` | TWAI transmit |
| Super Mini `GPIO5` | SN65HVD230 `RXD` / `R` | TWAI receive |
| SN65HVD230 `CANH` | bus `CANH` | twisted pair |
| SN65HVD230 `CANL` | bus `CANL` | twisted pair |
| Super Mini `GPIO10` | relay module `IN` | LOW = on, hence `Invert = 1` |
| Super Mini `GND` | relay module `GND` | common ground |
| separate 5 V supply | relay module `VCC` | **not** the board's 3V3 — the coil can pull it down |
| relay `COM` / `NO` | the load | |
| — | Super Mini `GPIO9` | service button: BOOT on the board |
| — | Super Mini `GPIO8` | status LED: on the board (no longer a mirror of the relay) |
