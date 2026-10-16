# Węzły LagunBus

Oprogramowanie urządzeń fizycznych — **klawisz** i **wyjście**, a od FEAT-09 **trzy
klawisze** (`key3`), **osiem wyjść na MCP23017** (`out8`), a od DEVICE-01 **czujnik
pokojowy** (`env`: temperatura, wilgotność, światło), a od DEVICE-04 **wyświetlacz
e-papierowy** (`epd`: temperatura i wilgotność) — na ESP32-C3 Super Mini
z zewnętrznym nadajnikiem CAN.

The protocol itself is **not here**. The node library — C for this firmware, Python for
simulators — lives in its own repository, [`../lagunbus-node/`](../lagunbus-node/), and
comes in as a submodule. What this repository keeps is the glue between that library and
a board: the ESP32-C3 port, the two applications, the ESP-IDF container and the flashing
scripts (zadania/zrobione/FEAT-03.md).

Protokół opisuje [`../lagunbus-spec/`](../lagunbus-spec/), drugą stroną kontraktu jest
[`../hub/`](../hub/).

**Stan: potwierdzony POC.** Klawisz przyłączył się do huba przez prawdziwą magistralę
(2026-09-24): węzeł zgłasza się sam, przyjmuje adres, odpowiada na odczyty i zapisy,
odsyła `Uid` segmentami i ogłasza `Booted`. What the library still lacks is listed in
[`../lagunbus-node/README.md`](../lagunbus-node/README.md).

---

## The library, as a submodule

```sh
git submodule update --init          # once after cloning: fills lib/lagunbus-node
```

> **TODO:** the library has no GitHub repository yet, so `git submodule update` works only
> on the machine that has the local clone. See the TODO at the top of
> `lib/lagunbus-node/README.md`.

```
  lib/lagunbus-node/c/lagunbus/     ← the library, an ESP-IDF component
            │
            ▼
  ports/esp32c3/                    TWAI + NVS + esp_timer + Uid from eFuse
  apps/key, key3, output, out8, env, epd   read pins, drive outputs, read sensors, draw
```

Both applications point `EXTRA_COMPONENT_DIRS` at `lib/lagunbus-node/c/lagunbus` and at
`ports/esp32c3`. A submodule rather than a path to the sibling directory: the ESP-IDF
container mounts only this repository, so `../lagunbus-node` does not exist where the
image is built — and the submodule pins the exact library commit a firmware was built
against.

### After changing the library — two steps, both needed

This repository records **a commit** of the library, not "the latest". A change pushed
to `lagunbus-node` does not reach the firmware until the pointer here moves:

```sh
# 1. in lagunbus-node: commit and push the change
# 2. here: fetch it into the submodule AND record the new pointer
git -C lib/lagunbus-node pull
git add lib/lagunbus-node && git commit -m "Biblioteka węzła: …"
```

- without step 2 the image keeps building from the old library — deliberately: nothing
  changes the firmware by accident, and the history here says which library a build used;
- without the `git add` + commit the update exists only on this machine; a fresh clone's
  `git submodule update` puts the old commit back;
- a plain `git pull` here does **not** update the submodule's files. After pulling, or on
  a fresh clone, run `git submodule update --init` — or set it once with
  `git config submodule.recurse true`.

Working on the library for the firmware? Edit it right in `lib/lagunbus-node` — it is a
full clone: commit and push from there, then step 2 without the `pull`. The sibling
`../lagunbus-node` then falls behind until it pulls.

The native build, the unit tests, `lagun-node` on SocketCAN and the registry generator
all moved with the library — see its README. Everything that can be checked without a
board is checked there, **before** anything is flashed: the key and output logic lives in
the library for exactly that reason. What is left to settle on the board is TWAI, the
transceiver, power and GPIO.

---

## Układ

```
devices/
├── lib/lagunbus-node/          SUBMODULE — the node library (C + Python)
├── ports/
│   └── esp32c3/                TWAI + NVS + esp_timer + Uid z eFuse
├── components/
│   ├── mcp23017/               ekspander I²C — rejestry, nie endpointy (apps/out8)
│   ├── sht4x/                  temperatura i wilgotność SHT40 (apps/env)
│   ├── veml7700/               natężenie światła VEML7700, sam dobiera zakres (apps/env)
│   ├── epd1in9/                e-papier segmentowy Waveshare 1,9″ (IST7134), bez czekania (apps/epd)
│   └── ws2812/                 łańcuch WS2812 na RMT — bajty i czasy przebiegu, nie barwy (apps/rgb8)
├── apps/
│   ├── key/                    klawisz
│   ├── key3/                   trzy klawisze (zadania/zrobione/FEAT-09.md)
│   ├── dial/                   pokrętło: enkoder EC11 + klawisz w osi (zadania/zrobione/DEVICE-02.md)
│   ├── output/                 wyjście (przekaźnik)
│   ├── out8/                   osiem wyjść na MCP23017 (zadania/zrobione/FEAT-09.md)
│   ├── env/                    czujnik: SHT40 + VEML7700 (zadania/zrobione/DEVICE-01.md)
│   ├── epd/                    wyświetlacz: e-papier, dwa pola Display (zadania/zrobione/DEVICE-04.md)
│   └── rgb8/                   osiem WS2812 jako jedno światło kolorowe (zadania/devices/DEVICE-03.md)
├── tools/                      flash.sh, monitor.sh
└── .devcontainer/              ESP-IDF v5.3.1 w kontenerze
```

---

## Decyzje

### Dwa porty, choć docelowo ACK nie jest problemem

CAN wymaga, żeby ramkę potwierdził ktoś inny w szczelinie `ACK`; samotny nadajnik
powtarza i schodzi w `bus-off`. Ale **odbiór działa zawsze**, a **potwierdza kontroler,
nie aplikacja** — ESP32 z podniesionym TWAI potwierdza ramki huba, choćby firmware nie
umiał nic więcej. Na linii są docelowo trzy węzły, więc problem dotyczy tylko okna, zanim
na którejkolwiek płytce wstanie TWAI.

Port SocketCAN (now in `lagunbus-node/c/ports/posix`) jest potrzebny z dwóch innych powodów:

- **rozdzielenie usterek.** Gdy pierwsza ramka nie dojdzie, podejrzanych jest sześciu:
  protokół, TWAI, nadajnik, okablowanie, sterownik `mcp251xfd` i hub. Węzeł zbudowany
  natywnie i wpięty w `vcan0` wyklucza pięciu naraz. `vcan` to `modprobe vcan` i nic więcej.
- **tempo pracy.** Cykl „popraw, zbuduj, uruchom" na `vcan0` trwa sekundy i nie wymaga
  wtykania kabla USB.

### Dwa interfejsy i nic więcej

Biblioteka rozmawia ze światem przez [`transport.h`](lib/lagunbus-node/c/lagunbus/include/lagunbus/transport.h)
(wyślij, odbierz bez czekania) i [`platform.h`](lib/lagunbus-node/c/lagunbus/include/lagunbus/platform.h)
(czas, pamięć trwała, losowość). Losowość jest z konkretnego powodu: zwłoka przed
zgłoszeniem musi być **losowa**, inaczej czterdzieści węzłów po powrocie zasilania zgłasza
się w tej samej milisekundzie ([`03-adresacja.md`](../lagunbus-spec/03-adresacja.md) §5).

| Czego w bibliotece nie ma | Dlaczego |
|---|---|
| wątków i zadań | pętla węzła to **jedna funkcja `lb_node_tick()`**; kto ją woła i z czego, należy do portu |
| przydziału pamięci | wszystko w strukturach podanych przez aplikację; brak `malloc` w ścieżce roboczej jest na ESP32 wart więcej niż wygoda |
| `printf` | dziennik przez wskaźnik na funkcję albo wcale |

### The registry is generated — in the library

`registry.h` is generated from `lagunbus-spec/registry/` by `lagunbus-node/tools/generate_registry.py`,
together with its Python twin, and is never edited by hand. Nothing in this repository
generates anything any more.

### `ShortId` to CRC-32 z całego `Uid`

Tak każe opis typu `Device`. Wcześniej firmware brał cztery starsze bajty `Uid`, co
działało tylko dlatego, że port ESP32 **odwraca MAC** (pierwsze trzy bajty MAC to OUI
Espressifa, identyczny na każdej płytce). Poprawność zależała więc od tego, jak jeden port
składa `Uid`. Odwrócenie MAC zostaje mimo to: zmiana zmieniłaby `Uid` każdej wgranej
płytki, a `Uid` jest tożsamością na całe życie urządzenia. Szczegóły —
[`node.c`](lib/lagunbus-node/c/lagunbus/src/node.c) i [`ports/esp32c3/platform.c`](ports/esp32c3/platform.c).

### Klucze NVS po angielsku

Przed wersją 0.5 klucze w pamięci trwałej zmieniły nazwy: `adres` → `address`,
`wyjscie` → `output`. **Skutek dla płytki wgranej starszym obrazem:** nie znajduje swojego
adresu, zgłasza się od nowa, a hub wpuszcza ją sam (znany `ShortId` dostaje adres bez
trybu przyjmowania); zapamiętany stan wyjścia przepada. Po tej zmianie obrazów ESP-IDF
nie przebudowano — pierwszy build w kontenerze pokaże, czy po zmianie nazw aplikacji
(`apps/key`, `apps/output`) nic nie zostało.

**Settings in NVS (since `zadania/zrobione/TASK-01.md`).** Every writable persistent
attribute — the common ones included — is stored under its own key `a<endpoint>.<element
in hex>` (`a4.13` is `PowerOnBehaviour` of ep4) when the hub writes it, and loaded before
`Booted`. A board flashed with an older image has none of these keys and starts from its
defaults, as before; the hub's reconciling writes them once. A factory reset erases them
through `nvs_erase_key` (`esp_erase` in `ports/esp32c3/platform.c`); the boot counter,
`PressCount` and the outputs' remembered state stay, and the address is overwritten with
"none", as before.
`ReportMinGap` of `apps/output` and `apps/out8` moved from the attribute rows to the
endpoint's `.common`; a row `LB_COMMON_*` would now be ignored.

### Build w kontenerze, wgrywanie z gospodarza

Docker Desktop na macOS nie przepuszcza USB i nie ma na to obejścia prostszego niż to.
Most szeregowy `rfc2217` rozważony i odrzucony: jedna ruchoma część więcej w miejscu,
w którym i tak będzie się szukać usterek sprzętowych. The native build lives in
`lagunbus-node` and needs no container at all — and that is where most things get settled.

### The log never makes the node wait (2026-10-06)

The console is USB Serial/JTAG, and a write to it waits when its 64-byte FIFO is full —
up to 50 ms when nobody reads the port. The node logs on the way to its answer, so one
command could hold the `RESPONSE` past the hub's 100 ms; the hub then repeats it, and
before the library deduplicated commands (`zadania/zrobione/BUG-02.md`, in a firmware only once
the `lib/lagunbus-node` pointer has moved past it) a `Toggle` ran twice. Seen on `out8`:
a response after 108 ms.

Every application therefore calls `lb_esp_log_init()` first: `ESP_LOGx` only puts the
line in a ring buffer, and a task of its own writes it to USB. A full buffer drops lines
and says how many (`[log: N lines dropped]`). Logging less was rejected — the frame log
is the window onto a bring-up, and any single line can wait. Details in
[`ports/esp32c3/log.c`](ports/esp32c3/log.c).

For the same reason the MCP23017 write in `out8` gives up after 10 ms, not 100 ms: it
also runs on the way to the answer.

### Co z tego wynika dla huba

Nic. Węzeł fizyczny wygląda dla huba dokładnie tak, jak atrapa w jego testach, bo obie
strony trzymają ten sam kontrakt. Jeśli okaże się inaczej, to usterka po tej stronie albo
dziura w specyfikacji, a nie powód do zmian w hubie.

---

## Sprzęt

### Nadajnik musi być na 3,3 V

To jedyna rzecz w tym dokumencie, której pomyłka kosztuje układ. ESP32-C3 **nie ma nóżek
tolerujących 5 V**; nadajnik zasilany z 5 V wystawia na `RXD` 3,4–5 V wprost na nóżkę
procesora.

| nadajnik | zasilanie | czy wolno |
|---|---|---|
| **SN65HVD230 / SN65HVD232** | 3,3 V | tak — domyślny wybór |
| **TJA1051T/3** | 3,3 V | tak, ale **musi być wersja `/3`** |
| **MCP2562FD** | 5 V + osobne 3,3 V na `VIO` | tak, o ile `VIO` naprawdę podłączone |
| TJA1050 | 5 V | **nie** |
| MCP2551 | 5 V | **nie** |

Oznaczenie sprawdza się na samym układzie, nie na opisie aukcji — moduły „CAN dla
Arduino" mają często TJA1050.

### The Super Mini from the top

The pin labels are printed on the board's **underside**, so in a breadboard they are
out of sight. Seen from the **top** — components up, USB-C away from you — the two rows
are these (`●` a pin with a note). Every application's README draws the same view
with its own connections.

```
                                       ┌─┤ USB-C ├─┐
        TWAI RX (every app) ──  GPIO5 ●┤           ├● 5V     ── 5 V in (not with USB)
                                GPIO6 ○┤           ├● GND    ── ground
                                GPIO7 ○┤           ├● 3V3    ── 3.3 V out
      status LED, strapping ──  GPIO8 ●┤           ├● GPIO4  ── TWAI TX (every app)
            BOOT, strapping ──  GPIO9 ●┤           ├○ GPIO3
                               GPIO10 ○┤           ├● GPIO2  ── strapping — avoid
       UART RX — input only ── GPIO20 ●┤           ├○ GPIO1
UART TX — ROM talks at boot ── GPIO21 ●┤           ├○ GPIO0
                                       │  antenna  │
                                       └───────────┘
```

Check it once on a new batch of boards: the pin drawn as `GND` rings through to the
USB-C shell, and the one under it measures 3.3 V with USB plugged in. A board seen
from below has the two rows the other way round — the mistake this drawing is here
to prevent.

### Połączenia

```
ESP32-C3 Super Mini          nadajnik CAN (3,3 V)
───────────────────          ────────────────────
GPIO4  ──────────────────────  TXD
GPIO5  ──────────────────────  RXD
3V3    ──────────────────────  VCC
GND    ──────────────────────  GND
                               CANH ──┐
                               CANL ──┤  skrętka do reszty magistrali
```

GPIO wybiera się w `idf.py menuconfig` → *LagunBus*; powyższe to wartości domyślne.

**Pin-to-pin wiring of each device** — every connection, power and transceiver included —
is in the application's own README: [`apps/key`](apps/key/README.md),
[`apps/key3`](apps/key3/README.md), [`apps/output`](apps/output/README.md),
[`apps/out8`](apps/out8/README.md), [`apps/env`](apps/env/README.md),
[`apps/epd`](apps/epd/README.md), [`apps/dial`](apps/dial/README.md),
[`apps/rgb8`](apps/rgb8/README.md).

| Czego nie ruszać | Dlaczego |
|---|---|
| GPIO2, GPIO8, GPIO9 | wybór trybu startu — stan w chwili resetu decyduje, czy płytka wystartuje |
| GPIO18, GPIO19 | USB Serial/JTAG, czyli wgrywanie i dziennik |
| GPIO21 | UART0 TX — ROM nadaje na nim przy starcie |
| GPIO20 | UART0 RX — **wolny jako wejście**, bo konsola idzie przez USB Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` w każdej aplikacji); `apps/epd` czyta na nim `BUSY` (`zadania/zrobione/DEVICE-04.md`, N2). Nie jako wyjście |

GPIO8 jest wyjątkiem w jedną stronę: siedzi tam dioda i **po starcie** wolno nią sterować.
GPIO9 (przycisk BOOT) tak samo — po starcie to zwykłe wejście. Oba są dziś przyciskiem
serwisowym i diodą stanu (niżej).

**Klawisz:** `GPIO3` — przycisk do `GND`, podciąganie wewnętrzne włączone programowo.
Spoczynek to stan wysoki, więc `main.c` czyta stan niski jako naciśnięcie. Polaryzacja
to sprawa budowy urządzenia, nie atrybut — typ `KeyInput` nie ma już `Invert`.

**Wyjście:** `GPIO10` — wejście `IN` modułu przekaźnikowego. Typowy moduł załącza się
stanem **niskim** — stąd `LAGUN_RELAY_ACTIVE_LOW = y` w `menuconfig`; od CR-04 to nastawa
budowy, a nie atrybut `Invert`, który hub mógłby nadpisać. Pomyłka objawia się przekaźnikiem załączonym
przez cały czas poza chwilą, gdy ma być załączony. Przekaźnik zasilaj **osobno**, nie
z 3V3 płytki — cewka przy przełączaniu potrafi ściągnąć zasilanie tak, że procesor się
restartuje.

### Service button and status LED — every device has them

A requirement of the specification (`lagunbus-spec/types/0x0001-device.md` §5.4,
`zadania/zrobione/CR-05.md`): a **service button** for a local factory reset and a **status LED**
showing what the node is doing, both reachable with the module mounted and independent
of its function. The logic is the library's (`tools/service.h`, `tools/led.h`, tested on
a computer); the pins are the port's `lb_esp_status_t`, the same in every application.

```
ESP32-C3 Super Mini
───────────────────
GPIO9  ──o  o── GND        service button — BOOT on the board; an external one in parallel
GPIO8  ──|◀──[330Ω]── 3V3  status LED — on the board; an external one lit by the same LOW
```

Both are strapping pins and both are on the header. GPIO9 held at power-on enters the
download mode — nothing breaks, let go and power up again. GPIO8 must not be pulled to
ground at reset, which is why an external LED goes **from 3V3**, not to GND. Pins in
`idf.py menuconfig` → *LagunBus* (`LAGUN_SERVICE_GPIO`, `LAGUN_STATUS_LED_GPIO`,
`LAGUN_STATUS_LED_ACTIVE_LOW`); a target board may move them.

**The button — the project's rule** (a guideline, not the specification's):

- hold it **5 s**: the LED goes **dark** — "let go now";
- **let go**: factory reset, the same as `FactoryReset` from the hub — the address is
  forgotten, the node restarts and announces itself as a new device. Let go earlier and
  nothing happens;
- a button pressed at start-up does nothing until it has been seen released — a stuck
  button cannot reset the node over and over.

**The LED:**

| what you see | state |
|---|---|
| lit, dark for a moment every 3 s | working — an address, `Booted` sent |
| slow blink, 0.5 s on, 0.5 s off | no address — announcing; the hub needs joining open |
| two short flashes every second | bus fault — error passive or bus-off; also a node alone on the cable |
| fast blink | `Identify` — `i` in the hub's view, 5 s |
| dark | a reset on its way — or the button held past 5 s — or no power |

Several at once show one, by a fixed priority: reset, identify, bus fault, no address,
working. In `apps/output` the on-board LED used to mirror the relay; it is
the status LED now — a relay module has its own LED for the relay.

### `key3` and `out8` — on a breadboard for now

Two devices closer to what will hang in the installation (`zadania/zrobione/FEAT-09.md`). Built
on a breadboard for the time being: the schematics show the connections, not the
mounting — the service button and the status LED are the board's own (BOOT/GPIO9 and the
LED on GPIO8), and `out8` drives LEDs, not relays yet. The CAN transceiver is wired as
above (GPIO4/GPIO5) and left out of the drawings.

**`key3`** — three keys, endpoints 4, 5, 6 (`KeyInput`, channels 1–3). Features `RAW` and
`HOLD`, no multi-click: each key switches between `CLICK` and `RAW` on its own, and has
its own hold. The library's copy is `key3` in `lib/lagunbus-node/c/sim/devices.c`.

```
ESP32-C3 Super Mini
───────────────────
GPIO6  ──o  o── GND        key 1 (endpoint 4)
GPIO7  ──o  o── GND        key 2 (endpoint 5)
GPIO10 ──o  o── GND        key 3 (endpoint 6)
GPIO9 / GPIO8              service button / status LED — on the board
```

To ground, with the internal pull-up: a LOW pin is a press. Pins in `idf.py menuconfig`
→ *LagunBus — key3*.

**`out8`** — eight outputs, endpoints 4 … 11 (`Output`, channels 1–8), on port A of an
MCP23017 over I²C (400 kHz, address 0x20). Each output an LED lit by a HIGH pin; no
`Invert`. The library's copy is `out8`.

```
ESP32-C3 Super Mini              MCP23017
───────────────────              ────────────────────
3V3   ───────────────┬─────────  VDD (9), RESET (18)     + 100 nF VDD–GND
GND   ───────────────┼─────────  VSS (10), A0 A1 A2 (15–17)   → address 0x20
GPIO6 ──────┬────────┼─────────  SDA (13)
GPIO7 ──────┼──┬─────┼─────────  SCL (12)
          [4k7][4k7] │
            └──┴─────┘ 3V3       GPA0 (21) ──[330Ω]──▶|── GND    output 1 (endpoint 4)
                                 GPA1 (22) ──[330Ω]──▶|── GND    output 2
                                   …
                                 GPA7 (28) ──[330Ω]──▶|── GND    output 8 (endpoint 11)
GPIO9 / GPIO8                    service button / status LED — on the board
```

- **RESET to 3V3 for good.** A restart of the ESP32 alone does not reset the MCP23017, so
  `out8` writes the registers in full before every start: the latch first (every output
  off), then port A to outputs, then `PowerOnBehaviour`. After power-on the MCP23017's
  pins are inputs — the LEDs stay dark until then.
- **I²C is assumed to work.** A failed write is logged and nothing more.
- **The chip is a component of its own**, `components/mcp23017/`: direction and latch of
  either port, a pin or a whole port at a time. It knows registers, not endpoints; the
  application creates the I²C bus and hands it in, so another chip can share it.
- Pins and the address in `idf.py menuconfig` → *LagunBus — out8*.

### Models — `DeviceType`

`DeviceType` names the **model**, not the endpoint type (`lagunbus-spec/types/0x0001-device.md`
§2.6), unique within `VendorId`. This project's devices have `VendorId 0`; a new one takes
the next free number here:

| `DeviceType` | application | what |
|---|---|---|
| `0x0010` | `apps/key` | one key, multi-click |
| `0x0011` | `apps/key3` | three keys, click / raw, hold |
| `0x0012` | `apps/dial` | a knob: encoder value (`ValueInput`) and the key in its shaft |
| `0x0020` | `apps/output` | one relay on a GPIO |
| `0x0021` | `apps/out8` | eight outputs on an MCP23017 |
| `0x0022` | `apps/rgb8` | eight WS2812 as one colour light (`Output`, `LEVEL \| COLOR_RGB`) |
| `0x0023` | `apps/epd` | e-paper: temperature and humidity on a Waveshare 1.9″ segment panel |
| `0x0030` | `apps/env` | temperature, humidity (SHT40) and light (VEML7700) |

### Magistrala

- **500 kbit/s**, ta sama wartość u huba i u każdego węzła; ramki **rozszerzone** 29-bitowe,
- **120 Ω na obu KOŃCACH**, nie przy każdym węźle. Przy trzech węzłach w linii terminowane
  są dwa skrajne. Sprawdź multimetrem między CANH a CANL, przy wszystkim odłączonym od
  zasilania: **około 60 Ω**. 120 Ω znaczy jeden terminator, 40 Ω — trzy.

Terminatory to najczęstsza przyczyna magistrali, która „prawie działa". Wiele modułów ma
rezystor wlutowany na stałe albo na zworce.

### Budowa i wgrywanie

```sh
# w kontenerze (VS Code: Reopen in Container)
cd apps/output
idf.py set-target esp32c3
idf.py menuconfig        # → LagunBus, jeśli GPIO mają być inne
idf.py build

# na macOS — narzędzia zakłada się raz
python3 -m venv .venv && .venv/bin/pip install esptool esp-idf-monitor
./tools/flash.sh output
./tools/monitor.sh output            # dziennik; wyjście: Ctrl-C
```

**Nie używaj `screen`.** Port szeregowy robi sam procesor (USB Serial/JTAG), więc przy
każdym resecie urządzenie USB znika i wraca — razem z `/dev/cu.*`, które `screen` trzyma
otwarte. Dziennika startowego nie widać wtedy nigdy. `tools/monitor.sh` czeka i podłącza
się z powrotem. Dwie płytki to dwa przebiegi — każda aplikacja (`key`, `key3`, `output`,
`out8`) to osobny obraz.

### Pierwsze uruchomienie

Węzeł bez adresu **zgłasza się i czeka**; hub musi mieć otwarte przyjmowanie (w `tui`
klawisz **`a`**, na 15 minut). Najpierw `candump can0` na RPi, dopiero potem hub — jeśli
`candump` milczy, problem jest w elektryce albo w prędkości i żaden program tego nie
pokaże. Hub na Raspberry Pi — [`../hub/README.md`](../hub/README.md) § Raspberry Pi.

---

## Czego świadomie nie robimy

| Co | Dlaczego |
|---|---|
| Aktualizacji przez magistralę | segmenty jej nie uniosą; osobny mechanizm, poza v1 |
| Grup i scen w węźle | `GroupCapacity = 0` jest poprawną odpowiedzią i hub sobie z nią radzi |
| `ZAPYTANIE` | węzeł dostanie to razem z termostatem, gdy hub będzie je obsługiwał |
| Własnej płytki | dwa moduły na stole wystarczą, żeby dowiedzieć się, co zaprojektować |
| Oszczędzania energii | te węzły są zasilane z linii, nie z baterii |
