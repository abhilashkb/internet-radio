# Internet Radio: ESP32 WROOM-32 Edition

A DIY Wi-Fi internet radio built around an **ESP32 WROOM-32 dev board** (USB-C, CH340 USB-serial, dual core, Wi-Fi + Bluetooth), a **1.3-inch SH1106 I2C OLED**, a **MAX98357A I2S mono amplifier**, a **KY-040 rotary encoder** and an 8 Ω speaker. It streams live radio from the open [Radio Browser](https://www.radio-browser.info/) directory. No subscription, no app, no cloud account.

This is a port of the XIAO ESP32-S3 version in [abhilashkb/internet-radio](https://github.com/abhilashkb/internet-radio), which in turn adapts the original project by the [Tech Talkies](https://www.youtube.com/@techtalkies1) YouTube channel. The sketch for this board is `Internet_Radio_ESP32.ino`.

> **Validation status:** This firmware has not been compiled or tested on hardware by its author. Compile it first, then test each hardware section (see [Staged test procedure](#staged-hardware-test-procedure)) before soldering anything permanently. Library and board-package versions can introduce differences.

---

## Table of contents

1. [How it works](#how-it-works)
2. [Bill of materials](#bill-of-materials)
3. [Wiring](#wiring)
4. [Power and grounding](#power-and-grounding)
5. [Software setup](#software-setup)
6. [Arduino IDE configuration](#arduino-ide-configuration)
7. [Firmware customisation](#firmware-customisation)
8. [Operation manual](#operation-manual)
9. [Staged hardware test procedure](#staged-hardware-test-procedure)
10. [Troubleshooting](#troubleshooting)
11. [Firmware architecture](#firmware-architecture)
12. [Differences from the ESP32-S3 version](#differences-from-the-esp32-s3-version)
13. [Known limitations](#known-limitations)
14. [Credits](#credits)

---

## How it works

```
Wi-Fi ──► Radio Browser API ──► ESP32 ──► I2S ──► MAX98357A ──► Speaker
                                   │
                                   ▼
                             SH1106 OLED
                                   ▲
                             KY-040 Encoder
```

1. On boot the ESP32 connects to your home Wi-Fi (2.4 GHz).
2. It queries the public Radio Browser API for stations, filtered by country and genre.
3. The selected station URL is handed to the **ESP32-audioI2S** library, which fetches the stream over HTTP and decodes it in real time.
4. Decoded audio goes out over **I2S** to the MAX98357A, which drives the speaker.
5. The OLED shows playback status, volume and stream metadata. The encoder controls volume, mute and the browse menu.
6. A background FreeRTOS task (core 0) downloads and parses station lists so the UI and audio stay responsive. Playback itself is always started from the main loop.

Bluetooth is not used. Its controller memory is released at boot to give Wi-Fi and audio more heap.

---

## Bill of materials

| # | Part | Qty | Notes |
|---|------|----:|-------|
| 1 | ESP32 WROOM-32 dev board, USB-C, CH340 | 1 | 30-pin or 38-pin boards both expose every GPIO used here |
| 2 | SH1106 OLED module | 1 | 1.3-inch, 128×64, I2C, 4-pin (VCC GND SCL SDA). **Must be SH1106**, not SSD1306 |
| 3 | MAX98357A amplifier breakout | 1 | I2S input, class-D mono. Adafruit #3006 or a compatible clone |
| 4 | KY-040 rotary encoder module | 1 | Buy the module (PCB with pull-ups), not the bare encoder |
| 5 | Speaker | 1 | 4–8 Ω, 2 W or more; a 2–3 inch full-range driver works well |
| 6 | USB-C cable and 5 V / ≥ 2 A charger | 1 | Use a data-capable cable for programming |
| 7 | Breadboard or perfboard, Dupont wires, pin headers | as needed | Breadboard first, solder later |
| 8 | Solder, flux, heat shrink | as needed | |

Optional: a 470 µF / 10 V capacitor across the amplifier's VIN and GND if you see resets at high volume.

---

## Wiring

> Power everything off before wiring. Check each connection for continuity before applying power.

### Master pin assignment

| Signal | ESP32 GPIO | Connects to |
|--------|:----------:|-------------|
| I2C SDA | **21** | OLED SDA |
| I2C SCL | **22** | OLED SCL |
| I2S data | **25** | MAX98357A DIN |
| I2S bit clock | **26** | MAX98357A BCLK |
| I2S word select | **27** | MAX98357A LRC / WS |
| Encoder A | **32** | KY-040 CLK |
| Encoder B | **33** | KY-040 DT |
| Encoder switch | **13** | KY-040 SW |
| 3V3 | 3V3 | OLED VCC, KY-040 `+` |
| 5V (VIN) | 5V | MAX98357A VIN |
| GND | GND | OLED GND, KY-040 GND, MAX98357A GND |

**Why these pins?** GPIO6–11 are wired to the module's flash chip and must not be used. GPIO0, 2, 5, 12 and 15 are boot-strapping pins and can stop the board booting if pulled the wrong way. GPIO34–39 are input-only with no internal pull-up. The chosen pins are all safe for their roles; GPIO32/33 have internal pull-ups, which suits the encoder.

### SH1106 OLED (I2C)

```
OLED module          ESP32
───────────          ─────
VCC         ──────►  3V3
GND         ──────►  GND
SDA         ──────►  GPIO21
SCL         ──────►  GPIO22
```

- Default address is `0x3C`; some modules use `0x3D`.
- Keep SDA/SCL wires short and away from the I2S lines.

### MAX98357A I2S amplifier

```
MAX98357A            ESP32
─────────            ─────
VIN         ──────►  5V (VIN pin)
GND         ──────►  GND
DIN         ──────►  GPIO25
BCLK        ──────►  GPIO26
LRC / WS    ──────►  GPIO27
SD / EN     ──────►  see note
GAIN        ──────►  see note
SPK+ / SPK- ──────►  speaker terminals
```

- **SD / EN:** Many breakouts pull this high internally. If yours is exposed and the amp is silent, tie it to 3V3 (or via 100 kΩ). A floating pin can leave the amp in shutdown.
- **GAIN:** Unconnected is about 9 dB on Adafruit's board. Connect to VIN for more gain or GND for less. Check your board's datasheet.
- **Speaker warning:** Connect the speaker only between `SPK+` and `SPK-`. Neither terminal is ground. Never connect either to GND.

### KY-040 rotary encoder

```
KY-040 module        ESP32
─────────────        ─────
CLK         ──────►  GPIO32
DT          ──────►  GPIO33
SW          ──────►  GPIO13
+           ──────►  3V3
GND         ──────►  GND
```

- Power the encoder from **3.3 V**, not 5 V. ESP32 GPIOs are not 5 V tolerant.
- If rotation direction is reversed, swap the CLK and DT wires (not SW).

---

## Power and grounding

Power the whole radio from a **5 V / 2 A USB-C charger** plugged into the dev board's USB-C port.

```
   5 V / 2 A CHARGER
          │ USB-C
          ▼
   ┌───────────────┐
   │ ESP32 DevKit  │
   │               │
   │ 5V (VIN) ─────┼──── MAX98357A VIN
   │ 3V3 ──────────┼──── OLED VCC, KY-040 +
   │ GND ──────────┼──── common ground
   └───────────────┘
```

| Rail | Voltage | Source | Powers |
|------|:-------:|--------|--------|
| USB VBUS / 5V pin | 5 V | Charger via the board's USB-C | MAX98357A |
| 3V3 pin | 3.3 V | Board's on-board regulator | OLED, encoder |
| GND | 0 V | Common | Everything |

Notes:
- On most dev boards the `5V`/`VIN` pin is a passthrough of USB VBUS when powered over USB. Confirm this on your board's silkscreen or schematic.
- The on-board 3.3 V regulator has limited current. The OLED and encoder draw very little, so this is fine, but do not hang other loads on it.
- Use a short, good-quality cable. Long or thin cables cause voltage drop, which shows up as resets and distorted audio at high volume.
- All grounds must meet at a single common point.

**Safety rules**
- Never put 5 V on the OLED VCC or on any ESP32 GPIO.
- Disconnect power before changing wiring.

---

## Software setup

### Required libraries

| Library | Version | Source |
|---------|---------|--------|
| **esp32** board package (Espressif) | 2.0.x or 3.x | Arduino Boards Manager |
| **ESP32-audioI2S** | Same version your S3 build used (see note) | [github.com/schreibfaul1/ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) |
| **ArduinoJson** | **6.x** (not 7.x) | Library Manager |
| **Adafruit GFX Library** | Latest | Library Manager |
| **Adafruit SH110X** | Latest | Library Manager |

> **ArduinoJson:** The sketch uses `DynamicJsonDocument` / `StaticJsonDocument` (6.x API). Version 7.x will not compile without changes.
>
> **ESP32-audioI2S:** The sketch uses the older `audio_info(const char*)` style callbacks. If a very recent library release reports callback or API errors, install an earlier release or adapt the three callbacks at the bottom of the sketch to the new signatures.

---

## Arduino IDE configuration

1. **Add the ESP32 board package.** Preferences → Additional Boards Manager URLs:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   Then Boards Manager → search `esp32` → install.
2. **Select the board:** Tools → Board → ESP32 Arduino → **ESP32 Dev Module** (or "DOIT ESP32 DEVKIT V1").
3. **PSRAM:** Tools → PSRAM → **Disabled**. The WROOM-32 has none.
4. **Partition scheme:** Choose **"Minimal SPIFFS (1.9MB APP with OTA)"** or **"No OTA (2MB APP/2MB SPIFFS)"**. The default ~1.2 MB app partition is usually too small once Wi-Fi, HTTP and audio decoders are linked.
5. **Upload speed:** 460800, or 115200 if uploads are unreliable.
6. **Port:** Select the COM/tty port for the CH340. Windows may need the CH340 driver if no port appears. If upload stalls on `Connecting...`, hold the **BOOT** button until writing begins.
7. **Wi-Fi credentials:** Open `Internet_Radio_ESP32.ino` and edit near the top:
   ```cpp
   #define WIFI_SSID     "your_network_name"
   #define WIFI_PASSWORD "your_password"
   ```
   Do not commit real credentials to a public repository.
8. **Verify** (checkmark) first, then **Upload**.
9. **Serial Monitor** at **115200 baud**. A healthy start looks like:
   ```
   Free heap at boot: ...
   Connecting to Wi-Fi.......
   Wi-Fi connected: 192.168.1.42
   Stations loaded: 31 (free heap ...)
   Audio info: ...
   ```

---

## Firmware customisation

All tunables are `#define` constants near the top of `Internet_Radio_ESP32.ino`.

| Constant | Default | Effect |
|----------|:-------:|--------|
| `WIFI_SSID` / `WIFI_PASSWORD` | placeholders | Your 2.4 GHz network |
| `OLED_ADDRESS` | `0x3C` | Try `0x3D` if the display stays blank |
| `OLED_SDA` / `OLED_SCL` | `21` / `22` | I2C pins |
| `I2S_DOUT` / `I2S_BCLK` / `I2S_LRCK` | `25` / `26` / `27` | I2S pins |
| `ENC_CLK` / `ENC_DT` / `ENC_SW` | `32` / `33` / `13` | Encoder pins |
| `RB_HOST` | `http://de1.api.radio-browser.info` | Radio Browser mirror; alternatives `nl1`, `at1`, `fr1` |
| `STATION_ARRAY_SIZE` | `40` | Stations kept in RAM |
| `RB_FETCH_LIMIT` | `60` | Stations requested from the API (some are filtered out, e.g. https) |
| `RB_CODEC` | `"MP3"` | Stream codec filter. `"AAC"` also works, but HE-AAC is heavy without PSRAM |
| `ALLOW_HTTPS` | `0` | `1` accepts https streams; TLS needs a lot of RAM and may fail on a no-PSRAM board |
| `JSON_DOC_SIZE` | `20480` | Heap for the filtered JSON result; raise it if you see `NoMemory` JSON errors |
| `VOL_MAX` / `INITIAL_VOLUME` | `21` / `18` | Volume range and startup level |
| `bitrateCap` | `96` | Max stream bitrate (kbps) requested from the API |
| `WIFI_RECONNECT_INTERVAL_MS` | `15000` | Delay between reconnect attempts |
| `ENC_DEBOUNCE_MS` | `4` | Increase if the encoder skips or double-counts |

### Adding countries and genres

Edit the `COUNTRIES[]` array (ISO 3166-1 alpha-2 code, display name) or the `GENRES[]` array (Radio Browser tag, display label):

```cpp
{ "PT", "Portugal" },      // COUNTRIES[]
{ "ambient", "Ambient" },  // GENRES[]
```

No other code changes are needed.

---

## Operation manual

One control, the **KY-040 encoder**: rotate to adjust, press for actions. The display has three screens.

### Screen 1: Playback

```
+----------------------------------+
| INTERNET RADIO         VOL [|||] |
|----------------------------------|
| NOW PLAYING                3/15  |
| Station name                     |
| Song / show title                |
| UK|96k                            |
|----------------------------------|
| Turn:Vol  Press:Mute             |
+----------------------------------+
```

| Action | Result |
|--------|--------|
| Rotate clockwise | Volume up (unmutes) |
| Rotate counter-clockwise | Volume down (unmutes) |
| Short press (< 700 ms) | Mute / unmute |
| Hold ≥ 700 ms | Open the Browse menu |

When the broadcaster sends a song or show title that differs from the station name, a second line is shown automatically. While the button is held, a small fill indicator appears in the top-right corner.

### Screen 2: Browse menu

Hold the button on the playback screen.

| Row | Shows |
|-----|-------|
| Station | Current station |
| Country | Active country filter |
| Genre | Active genre filter |

| Action | Result |
|--------|--------|
| Rotate | Move the highlight |
| Short press | Edit the highlighted item |
| Hold ≥ 700 ms | Back to playback |
| No input for 10 s | Back to playback automatically |

### Screen 3: Edit

- **Station:** rotate to scroll the loaded list (with a position bar and bitrate); press to play it.
- **Country / Genre:** rotate to preview `< value >`; press to confirm and fetch a new station list; hold to cancel.
- Hold, or 10 s of inactivity, returns without applying the preview.

**Countries:** All, USA, India, UK, Germany, France, Japan, Canada, Australia, Italy, Spain, Brazil, Mexico, Netherlands, Sweden, Norway, South Africa, Singapore, UAE.
**Genres:** All, Music, News, Jazz, Classical, Rock.

### Loading screen

Shown while a new station list downloads. Encoder input is ignored during this time. When the list arrives, playback starts on the first station. If nothing matches, the radio shows "No stations found".

### Volume and mute

Volume is 0–21, shown as 8 graduated bars. Muted shows `MUTE` with outlined bars. Volume is not saved; it resets to `INITIAL_VOLUME` on power-up.

### Wi-Fi reconnection

If Wi-Fi drops, the firmware retries every 15 s. After reconnecting it reloads the station list.

---

## Staged hardware test procedure

Test each part separately before combining them.

1. **ESP32 alone:** Upload the `Blink` example and confirm Serial output at 115200 baud. This proves the board, cable, driver and port.
2. **OLED:** Wire 3V3, GND, GPIO21 (SDA), GPIO22 (SCL). Run an I2C scanner and confirm `0x3C` or `0x3D`. Run an Adafruit SH110X example and check the text is clean. Shifted or garbled pixels suggest an SSD1306 module mislabelled as SH1106.
3. **Encoder:** Wire CLK→32, DT→33, SW→13, `+`→3V3, GND. Use a small test sketch to print CW / CCW / PRESS. If CW and CCW are swapped, swap the CLK and DT wires.
4. **Amplifier and speaker:** Power off. Wire DIN→25, BCLK→26, LRC→27, VIN→5V, GND. Connect the speaker only across SPK+ and SPK-, and check with a multimeter that neither wire has continuity to GND. Power on and test at low volume first.
5. **Full firmware:** Set Wi-Fi credentials, upload `Internet_Radio_ESP32.ino`, watch the Serial Monitor for `Wi-Fi connected`, `Stations loaded: N` (N > 0) and `Audio info:` lines. Then test volume, mute, the Browse menu and a station change.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| No serial port appears | Charge-only cable or missing CH340 driver | Use a data cable; install the CH340 driver (Windows) |
| Upload hangs at `Connecting...` | Board not in download mode | Hold **BOOT** while uploading; lower upload speed |
| "Sketch too big" | Partition too small | Select *Minimal SPIFFS (1.9MB APP)* or *No OTA* |
| Compile error about `DynamicJsonDocument` / `DeserializationOption` | ArduinoJson 7.x installed | Install ArduinoJson 6.x |
| Compile errors in `audio_*` callbacks | Newer ESP32-audioI2S API | Install an earlier release or update the callbacks |
| OLED blank | Wrong address, wiring or module | Run an I2C scanner; try `0x3D`; check 3V3, SDA=21, SCL=22 |
| OLED pixels shifted or garbled | Module is SSD1306, not SH1106 | Check the chip marking and library choice |
| `SH1106 OLED initialization failed` | Wrong address or wiring | Change `OLED_ADDRESS`; check wires |
| Encoder skips or double-counts | Contact bounce | Raise `ENC_DEBOUNCE_MS` to 8–12; add 100 nF capacitors CLK→GND and DT→GND |
| Encoder direction reversed | CLK/DT swapped | Swap the CLK and DT wires |
| Encoder button does nothing | Wiring | Check SW on GPIO13 and GND |
| Board will not boot with parts attached | A wire on a strapping pin (0, 2, 5, 12, 15) | Keep to the pin map in this README |
| `Wi-Fi connection timed out` | Wrong credentials, out of range, or 5 GHz network | Check credentials; use a 2.4 GHz network |
| `Stations loaded: 0` | API unreachable, or all results were https | Try another `RB_HOST` mirror; set Country and Genre to All; raise `RB_FETCH_LIMIT`; try `RB_CODEC "AAC"` |
| `JSON error: NoMemory` | Not enough heap for the result | Raise `JSON_DOC_SIZE` modestly or lower `RB_FETCH_LIMIT` |
| Station listed but no audio | Stream codec or format issue | Try another station; read Serial `Audio info:` messages; check I2S wiring |
| No sound, no errors | Amp in shutdown or wrong wiring | Tie SD/EN to 3V3; recheck DIN/BCLK/LRC |
| Audio stutters or drops | Weak Wi-Fi or a heavy stream | Move closer to the router; lower `bitrateCap`; keep `RB_CODEC "MP3"` |
| Distorted or clipped audio | Volume too high or weak supply | Lower volume; use a ≥ 2 A supply and a short cable |
| Random resets under load | Supply cannot deliver peak current | Better charger/cable; add 470 µF across amp VIN/GND |
| Hiss or hum | Ground loop or supply noise | Single common ground point; better supply |
| Loading screen never clears | Fetch failed or timed out | Check Serial for HTTP errors; try another mirror |
| Stream stops | Stream ended or station offline | Choose another station (no auto-advance by design) |

---

## Firmware architecture

### Tasks (FreeRTOS)

| Task | Core | Stack | Role |
|------|:----:|------:|------|
| `loop()` (Arduino main) | 1 | default | `audio.loop()`, event dispatch, UI redraw, Wi-Fi watchdog, starts playback after a fetch |
| `taskRotary` | 1 | 4 KB | Polls CLK/DT/SW every 2 ms, debounces, queues events |
| `taskFetch` | 0 | 10 KB | Downloads and parses stations; created on demand, deletes itself |

The fetch task never touches the audio library. It sets a flag when finished, and `loop()` then calls `audio.stopSong()` / `audio.connecttohost()`. This avoids cross-core calls into a library that is not thread-safe.

### UI state machine

```
              Hold (≥ 700 ms)
 MODE_NORMAL ─────────────────► MODE_BROWSE ───► MODE_EDIT
     ▲                              │                │
     └──── Hold / 10 s timeout ─────┘                │
     ◄──────────── Hold / 10 s timeout ──────────────┘
```

### Memory strategy (no PSRAM)

The API response is parsed with an ArduinoJson **filter** that keeps only `name`, `url_resolved`, `bitrate` and `countrycode`. Everything else in each record is discarded while parsing, so a ~20 KB buffer is enough. The request uses HTTP/1.0 to avoid chunked transfer encoding while stream-parsing.

### Extending with a new filter (e.g. language)

1. Add a `LanguageEntry` struct and `LANGUAGES[]` array, following `COUNTRIES[]`.
2. Add a focus item and extend `drawBrowseScreen()` / `drawEditScreen()`.
3. Add preview and selected variables and handle them in `handleEvent()`.
4. Append `&language=XX` to the URL in `fetchStationsInternal()`.

---

## Differences from the ESP32-S3 version

| Area | XIAO ESP32-S3 | ESP32 WROOM-32 |
|------|---------------|----------------|
| Pins | I2C 5/6, I2S 7/8/9, encoder 43/44/4 | I2C 21/22, I2S 25/26/27, encoder 32/33/13 |
| PSRAM | 8 MB, 32 KB JSON buffer | None; filtered JSON parse |
| Default codec | AAC | MP3 (`RB_CODEC`) |
| HTTPS streams | Allowed | Skipped by default (`ALLOW_HTTPS`) |
| Audio calls | Made from the fetch task | Made only from `loop()` |
| Bluetooth | n/a | Controller memory released |
| Wi-Fi power save | Default | Disabled |
| Hint text | Longer strings | Shortened to fit 128 px |
| Board setting | XIAO_ESP32S3, OPI PSRAM | ESP32 Dev Module, PSRAM disabled |

---

## Known limitations

| Feature | Status |
|---------|--------|
| HE-AAC and https streams | Disabled or unreliable without PSRAM; adjustable via defines |
| Long title scrolling | Not implemented; text is truncated with `...` |
| Wi-Fi RSSI display, equalizer, NTP clock | Not implemented |
| Auto-advance when a stream ends | Not implemented (use the Browse menu) |
| Remembering the last station or volume | Not implemented (would need NVS / Preferences) |
| HTTPS certificate validation | Not performed (library default); fine for home use |
| 5 GHz Wi-Fi | Not supported; the ESP32 radio is 2.4 GHz only |
| Bluetooth audio | Not supported; Bluetooth is deliberately disabled to save memory |

---

## Credits

| | |
|---|---|
| **Original project** | Tech Talkies YouTube channel: https://www.youtube.com/@techtalkies1 |
| **SH1106 / S3 adaptation** | https://github.com/abhilashkb/internet-radio |
| **Audio library** | ESP32-audioI2S by schreibfaul1: https://github.com/schreibfaul1/ESP32-audioI2S |
| **Station data** | Radio Browser community: https://www.radio-browser.info/ |
| **OLED driver** | Adafruit SH110X: https://github.com/adafruit/Adafruit_SH110x |
| **JSON parsing** | ArduinoJson by Benoit Blanchon: https://arduinojson.org/ |
