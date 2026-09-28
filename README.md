# Węzły LagunBus

Oprogramowanie urządzeń fizycznych: **klawisz** i **wyjście**, na ESP32-C3 Super Mini
z zewnętrznym nadajnikiem CAN. Do tego biblioteka w C, wspólna dla obu i **nie wiedząca
nic o sprzęcie**.

Protokół opisuje [`../lagunbus-spec/`](../lagunbus-spec/), drugą stroną kontraktu jest
[`../hub/`](../hub/).

**Stan: potwierdzony POC.** Klawisz przyłączył się do huba przez prawdziwą magistralę
(2026-09-24): węzeł zgłasza się sam, przyjmuje adres, odpowiada na odczyty i zapisy,
odsyła `Uid` segmentami i ogłasza `Booted`. Czego jeszcze nie ma — [niżej](#czego-jeszcze-nie-ma).

---

## Jedna biblioteka, trzy sposoby uruchomienia

To jest sedno układu i warto je przeczytać, zanim cokolwiek się buduje.

```
                    components/lagunbus/         ← te same pliki .c
                       protokół, zero sprzętu
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
   host/                 host/                ports/esp32c3
   TESTY                 WĘZEŁ na SocketCAN    WĘZEŁ na TWAI
   cmake + ctest         lagun-node, Linux     obraz na płytce
   macOS albo Linux      gada z hubem          ESP-IDF, kontener
```

```sh
python3 tools/generate_registry.py      # rejestr z lagunbus-spec/
cd host && cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

Testy sprawdzają kodek **tymi samymi wektorami, co hub**
([`PRZYKLADY.md`](../lagunbus-spec/PRZYKLADY.md)). Jeśli obie strony przechodzą te same
wektory, nie mogą rozjechać się ze sobą, nie rozjeżdżając się ze specyfikacją. Każda
linia sprawdzona tutaj jest linią, której nie trzeba będzie szukać analizatorem
magistrali — na płytce zostaje do rozstrzygnięcia TWAI, nadajnik, zasilanie i GPIO,
a nie to, czy adres siedzi na właściwych bitach.

Na Linuksie powstaje przy okazji `lagun-node` — **węzeł**, nie symulator: ten sam kod,
który jedzie na ESP32, wpięty w gniazdo SocketCAN zamiast w kontroler TWAI.

```sh
./build/lagun-node --bus can0            # zgłasza się i czeka na adres
./build/lagun-node --bus can0 --sniff    # tylko podgląd ruchu
```

Hub musi mieć wtedy **otwarte przyjmowanie** — inaczej odłoży zgłoszenie i nic się nie
wydarzy. Podgląd jest osobnym trybem, bo `candump` pokazuje bajty, a to pokazuje
znaczenie — przez tę samą bibliotekę, która jedzie na płytkę. Jeśli tu coś wygląda
inaczej niż w `candump`, usterka jest w bibliotece, a nie na magistrali.

---

## Układ

```
devices/
├── components/lagunbus/        BIBLIOTEKA — protokół, zero sprzętu
│   ├── include/lagunbus/
│   │   ├── frame.h             ramka, kodek 29-bit, bajt nagłówka
│   │   ├── registry.h          GENEROWANY z lagunbus-spec/registry/
│   │   ├── values.h            kodek wartości: u8…i32, map, enum, bool
│   │   ├── endpoint.h          endpoint: atrybuty, komendy, zdarzenia
│   │   ├── node.h              pętla węzła, przyłączanie, segmenty
│   │   ├── key.h, output.h     logika typów KeyInput i Output
│   │   ├── transport.h         ◄── JEDYNE miejsce, które wie o magistrali
│   │   └── platform.h          ◄── i o czasie, pamięci trwałej, losowości
│   └── src/
├── ports/
│   ├── esp32c3/                TWAI + NVS + esp_timer + Uid z eFuse
│   └── posix/                  SocketCAN + plik + clock_gettime
├── apps/
│   ├── key/                    klawisz
│   └── output/                 wyjście (przekaźnik)
├── host/                       build natywny: testy + lagun-node
├── tools/                      generator rejestru, flash.sh, monitor.sh
└── .devcontainer/              ESP-IDF v5.3.1 w kontenerze
```

Podział jest taki jak po stronie huba i z tego samego powodu: **`components/lagunbus/`
jest odpowiednikiem `protocol/`** — specyfikacja w kodzie, nic o urządzeniu. Logika
klawisza i wyjścia też jest w bibliotece, nie w aplikacji, więc sprawdza się ją na
komputerze (`host/test_key.c`, `host/test_output.c`).

---

## Decyzje

### Dwa porty, choć docelowo ACK nie jest problemem

CAN wymaga, żeby ramkę potwierdził ktoś inny w szczelinie `ACK`; samotny nadajnik
powtarza i schodzi w `bus-off`. Ale **odbiór działa zawsze**, a **potwierdza kontroler,
nie aplikacja** — ESP32 z podniesionym TWAI potwierdza ramki huba, choćby firmware nie
umiał nic więcej. Na linii są docelowo trzy węzły, więc problem dotyczy tylko okna, zanim
na którejkolwiek płytce wstanie TWAI.

Port SocketCAN jest potrzebny z dwóch innych powodów:

- **rozdzielenie usterek.** Gdy pierwsza ramka nie dojdzie, podejrzanych jest sześciu:
  protokół, TWAI, nadajnik, okablowanie, sterownik `mcp251xfd` i hub. Węzeł zbudowany
  natywnie i wpięty w `vcan0` wyklucza pięciu naraz. `vcan` to `modprobe vcan` i nic więcej.
- **tempo pracy.** Cykl „popraw, zbuduj, uruchom" na `vcan0` trwa sekundy i nie wymaga
  wtykania kabla USB.

### Dwa interfejsy i nic więcej

Biblioteka rozmawia ze światem przez [`transport.h`](components/lagunbus/include/lagunbus/transport.h)
(wyślij, odbierz bez czekania) i [`platform.h`](components/lagunbus/include/lagunbus/platform.h)
(czas, pamięć trwała, losowość). Losowość jest z konkretnego powodu: zwłoka przed
zgłoszeniem musi być **losowa**, inaczej czterdzieści węzłów po powrocie zasilania zgłasza
się w tej samej milisekundzie ([`03-adresacja.md`](../lagunbus-spec/03-adresacja.md) §5).

| Czego w bibliotece nie ma | Dlaczego |
|---|---|
| wątków i zadań | pętla węzła to **jedna funkcja `lb_node_tick()`**; kto ją woła i z czego, należy do portu |
| przydziału pamięci | wszystko w strukturach podanych przez aplikację; brak `malloc` w ścieżce roboczej jest na ESP32 wart więcej niż wygoda |
| `printf` | dziennik przez wskaźnik na funkcję albo wcale |

### Rejestr się generuje

```sh
python3 tools/generate_registry.py --check
```

`registry.h` powstaje z `lagunbus-spec/registry/` i **nie wolno go poprawiać ręcznie**.
To trzecia implementacja tych samych numerów, po hubie i jego atrapie węzła, i pierwsza,
w której przepisanie ich z ręki byłoby naprawdę kosztowne: błąd w stałej po stronie węzła
widać dopiero analizatorem magistrali. Nazwy w rejestrze są po polsku, a identyfikator C
po polsku nie istnieje — tablica tłumaczeń siedzi w generatorze i jest tą samą, co po
stronie huba; nieznana nazwa psuje generowanie celowo.

Katalog typów (`types/*.md`) maszynowo czytelny **nie jest**, więc definicje endpointów są
przepisane z ręki po raz trzeci. To najmocniejszy argument za rozstrzygnięciem punktu
**H8** huba.

### `ShortId` to CRC-32 z całego `Uid`

Tak każe opis typu `Device`. Wcześniej firmware brał cztery starsze bajty `Uid`, co
działało tylko dlatego, że port ESP32 **odwraca MAC** (pierwsze trzy bajty MAC to OUI
Espressifa, identyczny na każdej płytce). Poprawność zależała więc od tego, jak jeden port
składa `Uid`. Odwrócenie MAC zostaje mimo to: zmiana zmieniłaby `Uid` każdej wgranej
płytki, a `Uid` jest tożsamością na całe życie urządzenia. Szczegóły —
[`node.c`](components/lagunbus/src/node.c) i [`ports/esp32c3/platform.c`](ports/esp32c3/platform.c).

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
w którym i tak będzie się szukać usterek sprzętowych. Build natywny (`host/`) kontenera
nie potrzebuje wcale — a tam zapada większość rozstrzygnięć.

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

**Klawisz:** `GPIO3` — przycisk do `GND`, podciąganie wewnętrzne włączone programowo.
Spoczynek to stan wysoki, więc endpoint ma `Invert = true`.

**Wyjście:** `GPIO10` — wejście `IN` modułu przekaźnikowego. Typowy moduł załącza się
stanem **niskim** — stąd `Invert = true`. Pomyłka objawia się przekaźnikiem załączonym
przez cały czas poza chwilą, gdy ma być załączony. Przekaźnik zasilaj **osobno**, nie
z 3V3 płytki — cewka przy przełączaniu potrafi ściągnąć zasilanie tak, że procesor się
restartuje.

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
./tools/monitor.sh output            # dziennik; wyjście: Ctrl-]
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

## Czego jeszcze nie ma

**Transakcje po stronie węzła:**

- `Tid` z bajtu nagłówka i bufor deduplikacji: **8 pozycji, 300 ms**; trafiają do niego
  **wyłącznie komendy wykonane z wynikiem `OK`** — odmowa w buforze sprawiłaby, że
  powtórzenie po `BUSY` dostaje `BUSY` w kółko,
- `BUSY`, gdy węzeł nie może teraz — z prawem do powtórzenia,
- kolejka nadawcza z priorytetem: przy przepełnieniu ginie najstarszy `RAPORT`, nigdy
  `ODPOWIEDŹ` ani `ZDARZENIE`.

**Trwałość i cykl życia** (adres i stan wyjścia są już w NVS):

- atrybuty trwałe i licznik restartów w NVS — nastawy mają przeżyć zanik zasilania,
- `Reboot` (dziś `UNSUPPORTED_ELEMENT`), `Identify` — która to puszka.
  `FactoryReset` is done: acknowledged from the old address, carried out 50 ms later —
  the address stored as none, then `lb_platform_t.restart` (on the board `esp_restart()`).
  Without a restart hook (host, tests) the node only drops the address and system
  settings and rejoins; endpoint settings stay until the next power cycle,
- wersja firmware w `Booted`: jej podniesienie ma powodować ponowną enumerację, restart bez
  zmiany — nie.

**Grupy:** węzeł deklaruje `GroupCapacity = 0` i nie obsługuje ramek rozgłoszeniowych.

## Czego świadomie nie robimy

| Co | Dlaczego |
|---|---|
| Aktualizacji przez magistralę | segmenty jej nie uniosą; osobny mechanizm, poza v1 |
| Grup i scen w węźle | `GroupCapacity = 0` jest poprawną odpowiedzią i hub sobie z nią radzi |
| `ZAPYTANIE` | węzeł dostanie to razem z termostatem, gdy hub będzie je obsługiwał |
| Własnej płytki | dwa moduły na stole wystarczą, żeby dowiedzieć się, co zaprojektować |
| Oszczędzania energii | te węzły są zasilane z linii, nie z baterii |
