# `rgb8` — eight WS2812 as one colour light

ESP32-C3 Super Mini + SN65HVD230 CAN transceiver + eight WS2812(B) in a line. `DeviceType`
0x0022, endpoint 4: `Output` with `LEVEL | COLOR_RGB`, `Purpose = LIGHT` — all eight LEDs
show the same colour, one endpoint (`zadania/devices/DEVICE-03.md`; one endpoint per LED
or per segment is for later). Pins and limits are the defaults of `idf.py menuconfig` →
*LagunBus — rgb8*; power and the general rules are in [`../../README.md`](../../README.md)
§ Sprzęt.

Levels, colour, transitions, `LastLevel` and `PowerOnBehaviour` are the library's
(`lagunbus/types/output.h`, tested on a computer). What this application adds is the
board's: White added to R, G and B (the LEDs have no white one), a brightness curve
(gamma 2.2), the **current cap** — a channel never above `LAGUN_RGB8_MAX_COMPONENT`, 80 of
255 by default, the whole picture scaled so the colour stays — and the GRB byte order.
The driver is `components/ws2812` (RMT).

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
       │                   │  │                     8 × WS2812, in a line
       │                   │  │     470–1000 µF    ┌──────────────┐
       │                5V ├──┼─────────┬──────────┤ +5V          │
       │                   │  └─────────┴──────────┤ GND          │
       │             GPIO6 ├───[330 Ω]─────────────┤ DIN    DOUT  ├──── (nothing)
       │ BOOT (GPIO9)      │                       └──────────────┘
       │ LED  (GPIO8)      │
       └───────────────────┘
```

### The board from the top

The Super Mini's pin labels are printed on its **underside**, hidden once it sits in a
breadboard. This is the board seen from the **top** — components up, USB-C away from
you — with this device's connections: `●` used, `○` free.

```
                                               ┌─┤ USB-C ├─┐
                     SN65HVD230 RXD ──  GPIO5 ●┤           ├● 5V     ── WS2812 +5V (from USB)
              WS2812 DIN, via 330 Ω ──  GPIO6 ●┤           ├● GND    ── SN65HVD230 GND, WS2812 GND
                                        GPIO7 ○┤           ├● 3V3    ── SN65HVD230 3V3
           status LED, on the board ──  GPIO8 ●┤           ├● GPIO4  ── SN65HVD230 TXD
BOOT = service button, on the board ──  GPIO9 ●┤           ├○ GPIO3
                                       GPIO10 ○┤           ├○ GPIO2
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
| Super Mini `5V` | first WS2812 `+5V` | the LEDs on 5 V — from USB; not the board's 3V3 |
| Super Mini `GND` | first WS2812 `GND` | common ground |
| 470–1000 µF between `+5V` and `GND` | at the first LED | swallows the inrush when the chain lights |
| Super Mini `GPIO6` | 330 Ω → first WS2812 `DIN` | data; the resistor damps ringing on the wire |
| — | Super Mini `GPIO9` | service button: BOOT on the board |
| — | Super Mini `GPIO8` | status LED: on the board |

## Things to know

- **Current.** A WS2812 at full white draws about 60 mA, eight of them 480 mA. The cap of
  80/255 makes full white about 150 mA — comfortable on USB. Raise it in menuconfig if the
  supply allows; the colour does not change, only how bright full is.
- **3.3 V data into 5 V LEDs.** A WS2812B at 5 V asks for at least 0.7 × 5 = 3.5 V on `DIN`
  and the ESP32 gives 3.3 V. It usually works. If the first LED flickers or shows the
  wrong colour, put a 74AHCT125 (powered from 5 V) between `GPIO6` and `DIN`.
- **Dark at start.** The application sends black before anything else: a WS2812 can come
  up showing anything, and until `PowerOnBehaviour` decides, the light is off.
- **The lowest levels.** 8 bits a channel and the curve leave little at the bottom; a
  channel meant to shine is never sent 0, so `MinLevel` (10 ‰) still lights.
- **Driving it.** From the hub: `t` `o` `f` in the view, `c` for `SetLevel` / `SetColor`
  with arguments, or the knob `dial` bound to `SetLevel(level: $value)`. A component is
  0–254 — 0xFF on the bus is "absent", and `SetColor` refuses it.
