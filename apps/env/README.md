# `env` — temperature, humidity and light

ESP32-C3 Super Mini + SN65HVD230 CAN transceiver + SHT40 (temperature, humidity) +
VEML7700 (light), both on one I²C bus, on a breadboard for now
(`zadania/zrobione/DEVICE-01.md`). `DeviceType` 0x0030, class SENSOR. Pins are the
defaults of `idf.py menuconfig` → *LagunBus — env*; power and the general rules are in
[`../../README.md`](../../README.md) § Sprzęt. The drivers are
[`components/sht4x`](../../components/sht4x/) and
[`components/veml7700`](../../components/veml7700/).

| ep | type | `Quantity` | on the bus | features |
|---|---|---|---|---|
| 4 | `Sense` | `TEMPERATURE` | `i16`, 0.01 °C | `MIN_MAX`, `THRESHOLD`, `Offset` |
| 5 | `Sense` | `HUMIDITY` | `u16`, 0.01 % | `MIN_MAX`, `THRESHOLD`, `Offset` |
| 6 | `Sense` | `ILLUMINANCE` | `u32`, 0.01 lx | `MIN_MAX`, `THRESHOLD`, `Offset` |

Reports go by the quantity's defaults: 0.20 °C, 1.00 %, 50 lx or 10 % (whichever is
more), and every 60 s regardless. The SHT40 is read every 2 s, the VEML7700 every second
and it picks its own range; a failed read is "absent" on the bus, three in a row reset
the chip. The same device runs on a computer as `lagun-sim env`.

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
       │             GPIO6 ├──┼──┼──────┬──────────────┬────────── SDA
       │             GPIO7 ├──┼──┼──────┼──────┬───────┼──────┬─── SCL
       │                   │  │  │    [4k7]  [4k7]     │      │    only if the modules
       │                   │  ├──┼──────┴──────┘       │      │    have no pull-ups
       │ BOOT (GPIO9)      │  │  │                     │      │
       │ LED  (GPIO8)      │  │  │    SHT40 (0x44)     │      │    VEML7700 (0x10)
       └───────────────────┘  │  │   ┌───────────┐     │      │   ┌───────────┐
                              ├──┼───┤ VIN   SDA ├─────┘      ├───┤ SCL   VIN ├──┐
                              │  ├───┤ GND   SCL ├────────────┘   │       SDA ├──┼── SDA
                              │  │   └───────────┘                │       GND ├──┼──┐
                              │  │                                └───────────┘  │  │
                              └──┼───────────────────────────────────────────────┘  │
                                 └──────────────────────────────────────────────────┘
```

- **Modules with a regulator and their own pull-ups** (Adafruit, Pimoroni, most
  breakout boards) go in as they are, without the 4.7 kΩ resistors.
- **Keep the VEML7700 away from the status LED** (GPIO8, on the board): otherwise it
  measures its own blinking. Facing up, not towards the ESP32.
- **Keep the SHT40 away from the ESP32 and the regulator**: they warm up, and a sensor
  next to them reads the board, not the room. A few centimetres of wire are enough.

Build and flash:

```sh
# in the container
cd apps/env && idf.py set-target esp32c3 && idf.py build
# on macOS
tools/flash.sh env && tools/monitor.sh env
```

The log prints the three readings every 10 s (`21.37 °C  48.20 %  312 lx`) and every
frame with its meaning.
