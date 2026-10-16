# `key3` — three keys: click, raw, hold

ESP32-C3 Super Mini + SN65HVD230 CAN transceiver + three keys, on a breadboard for now
(`zadania/zrobione/FEAT-09.md`). `DeviceType` 0x0011, endpoints 4, 5, 6 (`KeyInput`, `RAW` and
`HOLD`, no multi-click). Pins are the defaults of `idf.py menuconfig` → *LagunBus —
key3*; power and the general rules are in [`../../README.md`](../../README.md) § Sprzęt.

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
       │             GPIO6 ├──┼──o  o──┐  K1  key 1 (endpoint 4)
       │             GPIO7 ├──┼──o  o──┤  K2  key 2 (endpoint 5)
       │            GPIO10 ├──┼──o  o──┤  K3  key 3 (endpoint 6)
       │                   │  └────────┘
       │ BOOT (GPIO9)      │  service button — on the board
       │ LED  (GPIO8)      │  status LED — on the board
       └───────────────────┘
```

### The board from the top

The Super Mini's pin labels are printed on its **underside**, hidden once it sits in a
breadboard. This is the board seen from the **top** — components up, USB-C away from
you — with this device's connections: `●` used, `○` free.

```
                                               ┌─┤ USB-C ├─┐
                     SN65HVD230 RXD ──  GPIO5 ●┤           ├○ 5V
                                 K1 ──  GPIO6 ●┤           ├● GND    ── SN65HVD230 GND, K1 K2 K3
                                 K2 ──  GPIO7 ●┤           ├● 3V3    ── SN65HVD230 3V3
           status LED, on the board ──  GPIO8 ●┤           ├● GPIO4  ── SN65HVD230 TXD
BOOT = service button, on the board ──  GPIO9 ●┤           ├○ GPIO3
                                 K3 ── GPIO10 ●┤           ├○ GPIO2
                                       GPIO20 ○┤           ├○ GPIO1
                                       GPIO21 ○┤           ├○ GPIO0
                                               │  antenna  │
                                               └───────────┘
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
| Super Mini `GPIO6` | K1, contact 1 | key 1, endpoint 4, channel 1 |
| Super Mini `GPIO7` | K2, contact 1 | key 2, endpoint 5, channel 2 |
| Super Mini `GPIO10` | K3, contact 1 | key 3, endpoint 6, channel 3 |
| K1, K2, K3, contact 2 | Super Mini `GND` | internal pull-ups; LOW = pressed |
| — | Super Mini `GPIO9` | service button: BOOT on the board |
| — | Super Mini `GPIO8` | status LED: on the board |

Power: USB-C, or 5 V to the `5V` pin — never both at once.
