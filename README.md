# Węzły LagunBus

Oprogramowanie urządzeń fizycznych: **klawisz** i **wyjście**, na ESP32-C3 Super Mini
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
  apps/key, apps/output             read a pin, drive a relay
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
├── apps/
│   ├── key/                    klawisz
│   └── output/                 wyjście (przekaźnik)
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

### Build w kontenerze, wgrywanie z gospodarza

Docker Desktop na macOS nie przepuszcza USB i nie ma na to obejścia prostszego niż to.
Most szeregowy `rfc2217` rozważony i odrzucony: jedna ruchoma część więcej w miejscu,
w którym i tak będzie się szukać usterek sprzętowych. The native build lives in
`lagunbus-node` and needs no container at all — and that is where most things get settled.

### The log never makes the node wait (2026-10-06)

The console is USB Serial/JTAG, and a write to it waits when its 64-byte FIFO is full —
up to 50 ms when nobody reads the port. The node logs on the way to its answer, so one
command could hold the `RESPONSE` past the hub's 100 ms; the hub then repeats it, and
until the library deduplicates commands (`zadania/BUG-02.md`) a `Toggle` runs twice. Seen
on `out8`: a response after 108 ms.

Every application therefore calls `lb_esp_log_init()` first: `ESP_LOGx` only puts the
line in a ring buffer, and a task of its own writes it to USB. A full buffer drops lines
and says how many (`[log: N lines dropped]`). Logging less was rejected — the frame log
is the window onto a bring-up, and any single line can wait. Details in
[`ports/esp32c3/log.c`](ports/esp32c3/log.c).

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

| Czego nie ruszać | Dlaczego |
|---|---|
| GPIO2, GPIO8, GPIO9 | wybór trybu startu — stan w chwili resetu decyduje, czy płytka wystartuje |
| GPIO18, GPIO19 | USB Serial/JTAG, czyli wgrywanie i dziennik |
| GPIO20, GPIO21 | UART0 |

GPIO8 jest wyjątkiem w jedną stronę: siedzi tam dioda i **po starcie** wolno nią sterować.
GPIO9 (przycisk BOOT) tak samo — po starcie to zwykłe wejście. Oba są dziś przyciskiem
serwisowym i diodą stanu (niżej).

**Klawisz:** `GPIO3` — przycisk do `GND`, podciąganie wewnętrzne włączone programowo.
Spoczynek to stan wysoki, więc `main.c` czyta stan niski jako naciśnięcie. Polaryzacja
to sprawa budowy urządzenia, nie atrybut — typ `KeyInput` nie ma już `Invert`.

**Wyjście:** `GPIO10` — wejście `IN` modułu przekaźnikowego. Typowy moduł załącza się
stanem **niskim** — stąd `Invert = true`. Pomyłka objawia się przekaźnikiem załączonym
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
się z powrotem. Dwie płytki to dwa przebiegi — `key` i `output` to osobne obrazy.

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
