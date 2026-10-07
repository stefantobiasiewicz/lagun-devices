# `out8` — eight outputs on an MCP23017

ESP32-C3 Super Mini + SN65HVD230 CAN transceiver + MCP23017 I²C port expander with eight
LEDs on port A, on a breadboard for now (`zadania/zrobione/FEAT-09.md`; relays later).
`DeviceType` 0x0021, endpoints 4 … 11 (`Output`, `Invert = 0`). Pins and the address are
the defaults of `idf.py menuconfig` → *LagunBus — out8*; power and the general rules are
in [`../../README.md`](../../README.md) § Sprzęt. The chip's driver is
[`components/mcp23017`](../../components/mcp23017/).

```
        ESP32-C3 Super Mini                         SN65HVD230 (3,3 V)
       ┌───────────────────┐                       ┌──────────────┐
  USB ─┤ USB-C         3V3 ├──┬────────────────────┤ 3V3          │
       │               GND ├──┼──┬─────────────────┤ GND          │
       │             GPIO4 ├──┼──┼─────────────────┤ TXD (D)      │
       │             GPIO5 ├──┼──┼─────────────────┤ RXD (R)      │
       │                   │  │  │                 │        CANH  ├──── bus CANH
       │                   │  │  │                 │        CANL  ├──── bus CANL
       │                   │  │  │                 └──────────────┘
       │             GPIO6 ├──┼──┼──────┬─────────────── SDA
       │             GPIO7 ├──┼──┼──────┼──────┬──────── SCL
       │                   │  │  │    [4k7]  [4k7]
       │                   │  ├──┼──────┴──────┘         pull-ups to 3V3
       │ BOOT (GPIO9)      │  │  │
       │ LED  (GPIO8)      │  │  │
       └───────────────────┘  │  │
                              │  │      MCP23017 (DIP-28, top view)
                              │  │     ┌────────────────────────────┐
                              │  │     │ 1  GPB0           GPA7  28 ├──[330Ω]──▶|── GND   out 8
                              │  │     │ 2  GPB1           GPA6  27 ├──[330Ω]──▶|── GND   out 7
                              │  │     │ 3  GPB2           GPA5  26 ├──[330Ω]──▶|── GND   out 6
                              │  │     │ 4  GPB3           GPA4  25 ├──[330Ω]──▶|── GND   out 5
                              │  │     │ 5  GPB4           GPA3  24 ├──[330Ω]──▶|── GND   out 4
                              │  │     │ 6  GPB5           GPA2  23 ├──[330Ω]──▶|── GND   out 3
                              │  │     │ 7  GPB6           GPA1  22 ├──[330Ω]──▶|── GND   out 2
                              │  │     │ 8  GPB7           GPA0  21 ├──[330Ω]──▶|── GND   out 1
                              ├──┼─────┤ 9  VDD           INTA  20 ├── n.c.
                              │  └─────┤ 10 VSS           INTB  19 ├── n.c.
                              │        │ 11 NC           RESET  18 ├────── 3V3
                     SCL ─────┼────────┤ 12 SCL             A2  17 ├────── GND
                     SDA ─────┼────────┤ 13 SDA             A1  16 ├────── GND
                              │        │ 14 NC              A0  15 ├────── GND
                              │        └────────────────────────────┘
                              └── 100 nF between VDD (9) and VSS (10), close to the chip
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
| Super Mini `3V3` | MCP23017 pin 9 `VDD` | the chip on 3.3 V — then no level shifter on I²C |
| Super Mini `GND` | MCP23017 pin 10 `VSS` | |
| 100 nF capacitor | MCP23017 pin 9 ↔ pin 10 | decoupling, close to the chip |
| Super Mini `GPIO6` | MCP23017 pin 13 `SDA` | I²C data |
| Super Mini `GPIO7` | MCP23017 pin 12 `SCL` | I²C clock, 400 kHz |
| 4.7 kΩ resistor | `SDA` ↔ `3V3` | I²C pull-up |
| 4.7 kΩ resistor | `SCL` ↔ `3V3` | I²C pull-up |
| MCP23017 pin 18 `RESET` | `3V3` | tied for good — the registers are written in full before every start |
| MCP23017 pin 15 `A0` | `GND` | address 0x20 |
| MCP23017 pin 16 `A1` | `GND` | |
| MCP23017 pin 17 `A2` | `GND` | |
| MCP23017 pin 21 `GPA0` | 330 Ω → LED anode, cathode → `GND` | output 1, endpoint 4 |
| MCP23017 pin 22 `GPA1` | 330 Ω → LED → `GND` | output 2, endpoint 5 |
| MCP23017 pin 23 `GPA2` | 330 Ω → LED → `GND` | output 3, endpoint 6 |
| MCP23017 pin 24 `GPA3` | 330 Ω → LED → `GND` | output 4, endpoint 7 |
| MCP23017 pin 25 `GPA4` | 330 Ω → LED → `GND` | output 5, endpoint 8 |
| MCP23017 pin 26 `GPA5` | 330 Ω → LED → `GND` | output 6, endpoint 9 |
| MCP23017 pin 27 `GPA6` | 330 Ω → LED → `GND` | output 7, endpoint 10 |
| MCP23017 pin 28 `GPA7` | 330 Ω → LED → `GND` | output 8, endpoint 11 |
| MCP23017 pins 1–8 `GPB0`–`GPB7`, 19 `INTB`, 20 `INTA` | — | not connected |
| — | Super Mini `GPIO9` | service button: BOOT on the board |
| — | Super Mini `GPIO8` | status LED: on the board |

A LED lights on a HIGH pin, so `Invert = 0` on every output. 330 Ω from 3.3 V gives a
few mA per LED — well inside the chip's 25 mA per pin.
