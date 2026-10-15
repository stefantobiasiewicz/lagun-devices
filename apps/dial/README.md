# `dial` — a knob: an encoder with a key in its shaft

ESP32-C3 Super Mini + SN65HVD230 CAN transceiver + an EC11 rotary encoder with a push
switch. `DeviceType` 0x0012; endpoint 4 is the value (`ValueInput`, 20 ‰ a detent, no
wrap), endpoint 5 the key in the shaft (`KeyInput`, `MULTICLICK` | `HOLD`), both on
channel 1 — one thing a person touches (`zadania/devices/DEVICE-02.md`). Pins are the
defaults of `idf.py menuconfig` → *LagunBus — dial*; power and the general rules are in
[`../../README.md`](../../README.md) § Sprzęt.

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
       │             GPIO0 ├──┼──────────── A   ┐
       │             GPIO1 ├──┼──────────── B   │ EC11
       │                   │  ├──────────── C   │ (common)
       │             GPIO3 ├──┼──────────── SW  │ key in the shaft
       │                   │  └──────────── SW2 ┘ (the key's other contact)
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
                                   5V ○┤           ├● GPIO5  ── SN65HVD230 RXD
SN65HVD230 GND, EC11 C + SW2 ──   GND ●┤           ├○ GPIO6
              SN65HVD230 3V3 ──   3V3 ●┤           ├○ GPIO7
              SN65HVD230 TXD ── GPIO4 ●┤           ├● GPIO8  ── status LED, on the board
                     EC11 SW ── GPIO3 ●┤           ├● GPIO9  ── BOOT = service button, on the board
                                GPIO2 ○┤           ├○ GPIO10
                      EC11 B ── GPIO1 ●┤           ├○ GPIO20
                      EC11 A ── GPIO0 ●┤           ├○ GPIO21
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
| Super Mini `GPIO0` | EC11 `A` | internal pull-up; an interrupt on both edges |
| Super Mini `GPIO1` | EC11 `B` | the same |
| EC11 `C` | Super Mini `GND` | the encoder's common pin |
| Super Mini `GPIO3` | EC11 `SW` | the shaft key; internal pull-up, LOW = pressed |
| EC11 `SW2` | Super Mini `GND` | the key's other contact |
| — | Super Mini `GPIO9` | service button: BOOT on the board |
| — | Super Mini `GPIO8` | status LED: on the board |

A module with resistors on it (KY-040 and the like) instead of a bare EC11: its `+` to
**3V3, not 5V** — the pins of the C3 are not 5 V tolerant — `GND` to `GND`, `CLK` is A,
`DT` is B, `SW` the key.

The whole encoder sits on the row with `GND` — five wires in one place. GPIO0 and GPIO1
are the pins of a 32 kHz crystal this board does not have, free here; GPIO2 between them
and GPIO3 decides how the chip starts, so it stays empty.

Power: USB-C, or 5 V to the `5V` pin — never both at once.

## Settings in `menuconfig`

| | default | what |
|---|---|---|
| `LAGUN_ENCODER_STEPS_PER_DETENT` | 4 | transitions of A and B per click. An EC11 makes 4, some encoders 2: if one click moves the value twice, set 2… the other way round, if it takes two clicks to move once, set 4 |
| `LAGUN_ENCODER_REVERSE` | no | clockwise turns the value down — the same as swapping A and B |

## What it does

- A click of the knob is one step of `StepSize` (20 ‰ — fifty clicks from dark to full):
  a `Changed(value, delta)` at once, and a report of `Value` at most once a second. At
  `MaxValue` or `MinValue` further turning sends nothing.
- Clicks that came between two turns of the loop (2 ms) go out as **one** `Changed` with
  the summed `delta`.
- The value is stored 5 s after the knob stops; the board comes up with it after a power
  cut. A write of `Value` from the hub (the light dimmed from a phone) moves the knob's
  value without a `Changed`, and the next click starts from there.
- The shaft key is a key like `apps/key`: click, double, triple, hold.

The decoding is the board's, not the library's (`DEVICE-02` N10): the C3 has no pulse
counter, so A and B raise an interrupt on every edge and a table of the sixteen
transitions of the Gray code counts the turn; a transition where both pins changed is a
bounce and counts nothing — no time filter, so a fast turn is not eaten.
