# `epd` — temperature and humidity on e-paper

ESP32-C3 Super Mini + SN65HVD230 CAN transceiver + **Waveshare 1.9inch Segment e-Paper**
(91 segments, black and white, I²C), on a breadboard for now
(`zadania/zrobione/DEVICE-04.md`). `DeviceType` 0x0023, class CONTROL. Pins are the
defaults of `idf.py menuconfig` → *LagunBus — epd*; power and the general rules are in
[`../../README.md`](../../README.md) § Sprzęt. The driver is
[`components/epd1in9`](../../components/epd1in9/).

| ep | type | `Quantity` | on the glass | range |
|---|---|---|---|---|
| 4 | `Display`, `VALUE` | `TEMPERATURE` | `188.8 °C` | −9.9 … 199.9 °C |
| 5 | `Display`, `VALUE` | `HUMIDITY` | `88.8 %` | 0.0 … 99.9 % |

The hub writes `Value`; the board rounds it to tenths. A value outside the range, "absent",
or no new value for `ContentTimeout` (600 s by default) — dashes (`--.-`). **100 % does
not fit** — the panel has no hundreds place for humidity — and shows dashes too. Always
°C; the Bluetooth and power icons stay off. The same device runs on a computer as
`lagun-sim epd`, its glass drawn by the same library code.

**Every write reaches the glass at once** as a partial refresh (0.3 s, no flash), and
every 10th refresh is a full one — the whole glass flashes once to wipe the traces partial
refreshes leave (`LAGUN_EPD_FULL_EVERY`). A write of what the glass already shows costs
no refresh. The maker asks for minutes between refreshes, as every one wears the panel;
`LAGUN_EPD_MIN_GAP_S` sets such a gap, 0 by default since a minute behind the sensor was
too slow (DEVICE-04, N3). At power-up the glass is cleared: it keeps whatever it showed
when the power went.

```
                     ESP32-C3 Super Mini
                   ┌─────────────────────┐
  USB (power)     ─┤ USB-C           3V3 ├──┬──────────── VCC ┐
                   │                 GND ├──┼──┬───────── GND │ SN65HVD230
                   │               GPIO4 ├──┼──┼───────── TXD │  (3.3 V)
                   │               GPIO5 ├──┼──┼───────── RXD ┘  CANH/CANL ── bus
                   │                     │  │  │
                   │               GPIO6 ├──┼──┼──────────────── SDA  ┐
                   │               GPIO7 ├──┼──┼──────────────── SCL  │ Waveshare
                   │              GPIO10 ├──┼──┼──────────────── RST  │ 1.9inch Segment
                   │              GPIO20 ├──┼──┼──────────────── BUSY │ e-Paper
                   │                     │  ├──┼──────────────── VCC  │ (3.3 V)
                   │  BOOT (GPIO9)       │  │  └──────────────── GND  ┘
                   │  LED  (GPIO8)       │  │
                   └─────────────────────┘  │
```

- **Every pin of the screen is on the right side of the board.** GPIO20 is UART0 RX,
  free as an input because the console goes over USB (DEVICE-04 N2).
- **BUSY is low while the panel works** (`BUSY_N`). The board pulls it down: unwired, it
  reads "busy", and the log says `BUSY low … is it wired?` after every refresh.
- **No resistors on I²C**: the module has its own pull-ups. Addresses `0x3C` (commands)
  and `0x3D` (data).
- **Where it may hang: 0–50 °C, at most 70 % RH, no condensation** (the panel's notes,
  V1.1). Not outdoors, not in a bathroom (DEVICE-04 N7).

### The first power-up of a panel — the test pattern

The segment map comes from code — Waveshare's demo and a community driver — not from the
maker's notes, which have none. Before trusting it, check it on the glass:
`idf.py menuconfig` → *LagunBus — epd* → **Test pattern instead of the node**. The board
then runs no node and steps through patterns every 4 s, the log saying what each one is:
everything black, every byte of the image alone, the bits of bytes 12 and 13, every
digit in every place, the half digit "1", the minus, the dashes, sample readings. What
to look for: digits 1, 2, 4, 6, 8 (Waveshare and the community code disagree on them),
the minus, and what bit 0x20 of byte 12 lights.

Build and flash:

```sh
# in the container
cd apps/epd && idf.py set-target esp32c3 && idf.py build
# on macOS
tools/flash.sh epd && tools/monitor.sh epd
```

Through the hub: `config/nodes.yaml` and `config/bindings.yaml` in `hub/` have it as
`kitchen-epd`, its two fields bound to `kitchen-env` — for `lagun-sim epd`. A real board
has its own `Uid` (from eFuse, not printed in the log): let the hub accept it with joining
open, then move the `kitchen-epd` alias and endpoints onto the entry the hub wrote.
