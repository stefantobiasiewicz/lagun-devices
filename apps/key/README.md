# `key` — one key, multi-click

ESP32-C3 Super Mini + SN65HVD230 CAN transceiver + one key. `DeviceType` 0x0010,
endpoint 4 (`KeyInput`, `MULTICLICK`). Pins are the defaults of `idf.py menuconfig` →
*LagunBus — key*; power and the general rules are in [`../../README.md`](../../README.md)
§ Sprzęt.

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
       │             GPIO3 ├──┼──o  o──┐  key
       │                   │  └────────┘
       │ BOOT (GPIO9)      │  service button — on the board
       │ LED  (GPIO8)      │  status LED — on the board
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
| Super Mini `GPIO3` | key, contact 1 | internal pull-up; LOW = pressed |
| key, contact 2 | Super Mini `GND` | |
| — | Super Mini `GPIO9` | service button: BOOT on the board |
| — | Super Mini `GPIO8` | status LED: on the board |

Power: USB-C, or 5 V to the `5V` pin — never both at once.
