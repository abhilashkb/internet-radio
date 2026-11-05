# Internet Radio: ESP32-S3 Dev Board Edition

A DIY Wi-Fi internet radio built around a **generic ESP32-S3 development board** (dual core 240 MHz, 8 MB flash, 8 MB PSRAM, Wi-Fi + Bluetooth LE), a **1.3-inch SH1106 I2C OLED**, a **MAX98357A I2S mono amplifier**, a **KY-040 rotary encoder** and an 8 Ω speaker. It streams live radio from the open [Radio Browser](https://www.radio-browser.info/) directory. No subscription, no app, no cloud account.

This is a port of the Seeed XIAO ESP32-S3 version in [abhilashkb/internet-radio](https://github.com/abhilashkb/internet-radio), which adapts the original project by the [Tech Talkies](https://www.youtube.com/@techtalkies1) YouTube channel. The sketch for this board is `Internet_Radio_ESP32S3.ino`.

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
12. [Changes from the XIAO ESP32-S3 version](#changes-from-the-xiao-esp32-s3-version)
13. [Known limitations](#known-limitations)
14. [Credits](#credits)

---

## How it works

```
Wi-Fi ──► Radio Browser API ──► ESP32-S3 ──► I2S ──► MAX98357A ──► Speaker
                                    │
                                    ▼
                              SH1106 OLED
                                    ▲
                              KY-040 Encoder
```

1. On boot the board checks that PSRAM is enabled, then connects to your home Wi-Fi (2.4 GHz).
2. It queries the public Radio Browser API for stations, filtered by country and genre.
3. The selected station URL is handed to the **ESP32-audioI2S** library, which fetches the stream over HTTP or HTTPS and decodes it in real time.
4. Decoded audio goes out over **I2S** to the MAX98357A, which drives the speaker.
5. The OLED shows playback status, volume and stream metadata. The encoder controls volume, mute and the browse menu.
6. A background FreeRTOS task (core 0) downloads and parses station lists so the UI and audio stay responsive. Playback itself is always started from the main loop.

---

## Bill of materials

| # | Part | Qty | Notes |
|---|------|----:|-------|
| 1 | ESP32-S3 dev board, 8 MB flash, 8 MB PSRAM | 1 | e.g. ESP32-S3-DevKitC-1 N8R8 or a clone. Boards with two USB-C ports (UART + native USB) are common |
| 2 | SH1106 OLED module | 1 | 1.3-inch, 128×64, I2C, 4-pin (VCC GND SCL SDA). **Must be SH1106**, not SSD1306: the controllers differ and the driver matters |
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

| Signal | ESP32-S3 GPIO | Connects to |
|--------|:-------------:|-------------|
| I2C SDA | **8** | OLED SDA |
| I2C SCL | **9** | OLED SCL |
| I2S data | **4** | MAX98357A DIN |
| I2S bit clock | **5** | MAX98357A BCLK |
| I2S word select | **6** | MAX98357A LRC / WS |
| Encoder A | **15** | KY-040 CLK |
| Encoder B | **16** | KY-040 DT |
| Encoder switch | **17** | KY-040 SW |
| 3V3 | 3V3 | OLED VCC, KY-040 `+` |
| 5V | 5V | MAX98357A VIN |
| GND | GND | OLED GND, KY-040 GND, MAX98357A GND |

These are **GPIO numbers**, not header positions. Match them to the GPIO labels printed on your board's silkscreen or pinout diagram, since the physical order differs between boards.

**Why these pins?** The pin choices follow the DevKitC-1 pinout described in Last Minute Engineers' [ESP32-S3-DevKitC-1 pinout reference](https://lastminuteengineers.com/esp32-s3-devkitc-pinout-reference/) (see the [pinout summary](#devkitc-1-pinout-summary) below). The following GPIOs are unavailable or risky on an N8R8 board:

| GPIOs | Reason |
|-------|--------|
| 26–32 | In-package flash/PSRAM lines; not brought out to the headers |
| 33–37 | Reserved on octal-memory modules (R8 / R8V), which includes N8R8. GPIO35–37 are exposed on the headers but must not be used |
| 19, 20 | Native USB D− / D+; repurposing them cuts USB connectivity |
| 43, 44 | UART0 TX / RX (serial monitor on the UART port); reassigning is strongly discouraged |
| 0, 3, 45, 46 | Boot / strapping pins; external circuits can stop the board booting or change its boot mode |
| 39–42 | Default hardware JTAG pins; usable as GPIO only if you do not need JTAG, so avoided here |
| 48 (v1.0) or 38 (v1.1) | On-board WS2812 RGB LED; depends on the DevKitC-1 board revision |

The pins chosen above avoid all of these. The original XIAO sketch used GPIO43/44 for the encoder; those are the serial pins on a dev board, so they were moved. The article covers the DevKitC-1 board itself and does not describe the N8R2 / N8R8 / N16R8 module variants, so the memory-variant notes in this README come from Espressif's module documentation. Clones may differ from the DevKitC-1, so confirm against your board's own pinout.

### DevKitC-1 pinout summary

For quick reference, facts from the pinout article that matter to this build:

| Topic | Detail |
|-------|--------|
| Logic level | Strictly 3.3 V. Never connect a GPIO to 5 V |
| Exposed GPIOs | GPIO0–21 and GPIO35–48 (GPIO22–34 are not brought out) |
| I2C | No fixed default pins; any GPIO can be mapped through the GPIO matrix, which is why GPIO8/9 work here. The article recommends external pull-ups of about 4.7–10 kΩ on SDA and SCL |
| UART1 defaults | GPIO17 (TX) / GPIO18 (RX). This firmware does not use UART1, so using GPIO17 for the encoder switch is fine |
| ADC | ADC1 is GPIO1–10, ADC2 is GPIO11–20, and ADC2 cannot be used while Wi-Fi is on. This build has no analog inputs, so GPIO15/16/17 are used as plain digital inputs |
| 5V pin | Accepts an external 4.3–6.0 V supply, regulated down to 3.3 V on-board; the USB ports feed the same regulator |
| 3V3 pin | Normally an output for small 3.3 V modules; the chip itself can draw up to about 500 mA during Wi-Fi transmit bursts, so keep external loads small |

### SH1106 OLED (I2C)

```
OLED module          ESP32-S3
───────────          ────────
VCC         ──────►  3V3
GND         ──────►  GND
SDA         ──────►  GPIO8
SCL         ──────►  GPIO9
```

- Default address is `0x3C`; some modules use `0x3D`.
- The ESP32-S3 has no fixed I2C pins, so GPIO8/9 are simply a convenient choice and are set in the sketch with `Wire.begin(OLED_SDA, OLED_SCL)`.
- I2C needs pull-up resistors on SDA and SCL. Most 4-pin OLED modules already have them on the PCB. If yours does not (display unstable or not detected), add external 4.7 kΩ to 10 kΩ resistors from SDA and SCL to 3V3.
- Keep SDA/SCL wires short and away from the I2S lines.

### MAX98357A I2S amplifier

```
MAX98357A            ESP32-S3
─────────            ────────
VIN         ──────►  5V
GND         ──────►  GND
DIN         ──────►  GPIO4
BCLK        ──────►  GPIO5
LRC / WS    ──────►  GPIO6
SD / EN     ──────►  see note
GAIN        ──────►  see note
SPK+ / SPK- ──────►  speaker terminals
```

- **SD / EN:** Many breakouts pull this high internally. If yours is exposed and the amp is silent, tie it to 3V3 (or via 100 kΩ). A floating pin can leave the amp in shutdown.
- **GAIN:** Unconnected is about 9 dB on Adafruit's board. Connect to VIN for more gain or GND for less. Check your board's datasheet.
- **Speaker warning:** Connect the speaker only between `SPK+` and `SPK-`. Neither terminal is ground. Never connect either to GND.

### KY-040 rotary encoder

```
KY-040 module        ESP32-S3
─────────────        ────────
CLK         ──────►  GPIO15
DT          ──────►  GPIO16
SW          ──────►  GPIO17
+           ──────►  3V3
GND         ──────►  GND
```

- Power the encoder from **3.3 V**, not 5 V. ESP32-S3 GPIOs are not 5 V tolerant.
- If rotation direction is reversed, swap the CLK and DT wires (not SW).

---

## Power and grounding

Power the whole radio from a **5 V / 2 A USB-C charger** plugged into one of the dev board's USB-C ports.

```
   5 V / 2 A CHARGER
          │ USB-C
          ▼
   ┌─────────────────┐
   │  ESP32-S3 board │
   │                 │
   │ 5V ─────────────┼──── MAX98357A VIN
   │ 3V3 ────────────┼──── OLED VCC, KY-040 +
   │ GND ────────────┼──── common ground
   └─────────────────┘
```

| Rail | Voltage | Source | Powers |
|------|:-------:|--------|--------|
| USB VBUS / 5V pin | 5 V | Charger via the board's USB-C | MAX98357A |
| 3V3 pin | 3.3 V | Board's on-board regulator | OLED, encoder |
| GND | 0 V | Common | Everything |

Notes:
- The `5V` pin accepts 4.3 V to 6.0 V and feeds the on-board 3.3 V regulator; the USB ports feed the same regulator. Whether the `5V` pin also outputs USB power depends on the board, and some boards have a solder jumper that must be bridged for it to do so. Confirm this on your board's silkscreen or schematic before relying on it to power the amplifier.
- The 3.3 V supply is shared with the chip, which can draw up to about 500 mA during Wi-Fi transmit bursts. The OLED and encoder draw very little, so this is fine, but do not hang other loads on the 3V3 pin.
- Use a short, good-quality cable. Thin or long cables cause voltage drop, which shows up as resets and distorted audio at high volume.
- All grounds must meet at a single common point.

**Safety rules**
- Never put 5 V on the OLED VCC or on any ESP32-S3 GPIO.
- Disconnect power before changing wiring.

---

## Software setup

### Required libraries

| Library | Version | Source |
|---------|---------|--------|
| **esp32** board package (Espressif) | 2.0.x or 3.x | Arduino Boards Manager |
| **ESP32-audioI2S** | Same version your XIAO build used (see note) | [github.com/schreibfaul1/ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) |
| **ArduinoJson** | **6.x** (not 7.x) | Library Manager |
| **Adafruit GFX Library** | Latest | Library Manager |
| **Adafruit SH110X** | Latest (installs Adafruit BusIO automatically) | Library Manager |

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
2. **Select the board and set the Tools menu:**

   | Setting | Value |
   |---------|-------|
   | Board | **ESP32S3 Dev Module** |
   | PSRAM | **OPI PSRAM** (required) |
   | Flash Size | **8MB (64Mb)** |
   | Flash Mode | QIO 80MHz (use DIO if the board will not boot) |
   | Partition Scheme | **8MB with spiffs (3MB APP/1.5MB SPIFFS)**, or the default layout |
   | USB CDC On Boot | **Disabled** to use the UART port for Serial; **Enabled** to use the native USB port |
   | Upload Speed | 921600 (lower it if uploads fail) |

   Other module variants: for **N16R8** choose 16MB flash; for **N8R2** (quad PSRAM) choose **QSPI PSRAM**. A wrong PSRAM mode can stop the board booting, so match it to the module marking.
3. **Port:** Select the serial port. Boards with two USB-C ports label them `UART` (via a USB-serial chip) and `USB` (native). Pick the one that matches your *USB CDC On Boot* setting; use the `UART` port if unsure.
4. **Wi-Fi credentials:** Open `Internet_Radio_ESP32S3.ino` and edit near the top:
   ```cpp
   #define WIFI_SSID     "your_network_name"
   #define WIFI_PASSWORD "your_password"
   ```
   Do not commit real credentials to a public repository.
5. **Verify** (checkmark) first, then **Upload**.
6. **Serial Monitor** at **115200 baud**. A healthy start looks like:
   ```
   Free heap at boot: ...  PSRAM: ... of 8386295 bytes free
   Connecting to Wi-Fi.......
   Wi-Fi connected: 192.168.1.42
   Stations loaded: 31 (heap ..., PSRAM ... free)
   Audio info: ...
   ```
7. **Bootloader mode (if needed):** if the board is not detected, hold **BOOT**, tap **RST**, release **BOOT**, then upload. Press **RST** once afterwards to run the sketch.

---

## Firmware customisation

All tunables are `#define` constants near the top of `Internet_Radio_ESP32S3.ino`.

| Constant | Default | Effect |
|----------|:-------:|--------|
| `WIFI_SSID` / `WIFI_PASSWORD` | placeholders | Your 2.4 GHz network |
| `OLED_ADDRESS` | `0x3C` | Try `0x3D` if the display stays blank |
| `OLED_SDA` / `OLED_SCL` | `8` / `9` | I2C pins |
| `I2S_DOUT` / `I2S_BCLK` / `I2S_LRCK` | `4` / `5` / `6` | I2S pins |
| `ENC_CLK` / `ENC_DT` / `ENC_SW` | `15` / `16` / `17` | Encoder pins |
| `RB_HOST` | `http://de1.api.radio-browser.info` | Radio Browser mirror; alternatives `nl1`, `at1`, `fr1` |
| `STATION_ARRAY_SIZE` | `40` | Stations kept in RAM |
| `RB_FETCH_LIMIT` | `60` | Stations requested from the API (some are dropped, e.g. unsupported URLs) |
| `RB_CODEC` | `"AAC"` | Stream codec filter; `"MP3"` is also supported |
| `ALLOW_HTTPS` | `1` | `1` accepts https streams; `0` restricts to http |
| `JSON_DOC_SIZE` | `32768` | Buffer for the filtered JSON result (allocated from PSRAM) |
| `VOL_MAX` / `INITIAL_VOLUME` | `21` / `18` | Volume range and startup level |
| `bitrateCap` | `128` | Max stream bitrate (kbps) requested from the API |
| `WIFI_RECONNECT_INTERVAL_MS` | `15000` | Delay between reconnect attempts |
| `ENC_DEBOUNCE_MS` | `4` | Increase if the encoder skips or double-counts |
| `WIFI_ICON_X` | `72` | Horizontal position of the signal bars on the playback screen |
| `WIFI_CHECK_MS` | `2000` | How often the signal strength is re-read |
| `WIFI_RSSI_4BARS` … `WIFI_RSSI_1BAR` | `-55` / `-65` / `-75` / `-85` | RSSI thresholds (dBm) for 4 / 3 / 2 / 1 bars |

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
| NOW PLAYING  ▂▄▆█          3/15  |
| Station name                     |
| Song / show title                |
| UK|128k                           |
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

The four bars next to `NOW PLAYING` show the Wi-Fi signal strength. Solid bars are lit and a single dot row means an unlit bar:

| Bars | Signal (RSSI) | Meaning |
|:----:|---------------|---------|
| 4 | −55 dBm or stronger | Excellent |
| 3 | −65 dBm or stronger | Good |
| 2 | −75 dBm or stronger | Fair; fine for most streams |
| 1 | −85 dBm or stronger | Weak; expect dropouts |
| 0 | Weaker than −85 dBm | Very weak, connection may fail |
| ✕ | Not connected | Wi-Fi lost; the radio is retrying |

The signal is re-read every 2 seconds and smoothed, so the bars do not flicker. Each change is also printed on the serial monitor, for example `Wi-Fi signal: 3 bar(s) (-62 dBm)`, which helps when deciding where to place the radio. If the bars are usually at 1 or 0, move the radio closer to the router or use a Wi-Fi extender.

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

1. **Board alone:** Upload the `Blink` example (or any serial "hello" sketch) and confirm Serial output at 115200 baud. This proves the board, cable, port and *USB CDC On Boot* choice. Most S3 dev boards use an addressable RGB LED on GPIO48 or 38, so plain `Blink` may not show an LED; judge success by the serial output.
2. **PSRAM:** Upload a sketch that prints `ESP.getPsramSize()`. A value near 8,000,000 confirms OPI PSRAM is enabled and working. Zero means the PSRAM setting is wrong.
3. **OLED:** Wire 3V3, GND, GPIO8 (SDA), GPIO9 (SCL). Run an I2C scanner and confirm `0x3C` or `0x3D`. Run a simple text example from the Adafruit SH110X library (`File > Examples > Adafruit SH110X`), using the address you found. Check the text is clean, with no shifted pixels or noisy edge column.
4. **Encoder:** Wire CLK→15, DT→16, SW→17, `+`→3V3, GND. Use a small test sketch to print CW / CCW / PRESS. If CW and CCW are swapped, swap the CLK and DT wires.
5. **Amplifier and speaker:** Power off. Wire DIN→4, BCLK→5, LRC→6, VIN→5V, GND. Connect the speaker only across SPK+ and SPK-, and check with a multimeter that neither wire has continuity to GND. Power on and test at low volume first.
6. **Full firmware:** Set Wi-Fi credentials, upload `Internet_Radio_ESP32S3.ino`, watch the Serial Monitor for the PSRAM line, `Wi-Fi connected`, `Stations loaded: N` (N > 0) and `Audio info:` lines. Then test volume, mute, the Browse menu and a station change.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| No serial port appears | Charge-only cable, or using the wrong USB port | Use a data cable; try the other USB-C port; use bootloader mode (hold BOOT, tap RST) |
| Serial Monitor shows nothing | *USB CDC On Boot* does not match the port in use | Disabled + `UART` port, or Enabled + native `USB` port |
| Board boot-loops after upload | Wrong PSRAM or flash mode for the module | Match PSRAM to the module marking (OPI for N8R8, QSPI for N8R2); try Flash Mode DIO |
| OLED shows "PSRAM not found" | PSRAM disabled in Tools menu | Tools → PSRAM → OPI PSRAM, then re-upload |
| Upload fails or hangs | Not in download mode, or speed too high | Hold BOOT, tap RST, release BOOT; lower Upload Speed |
| Compile error about `DynamicJsonDocument` / `DeserializationOption` | ArduinoJson 7.x installed | Install ArduinoJson 6.x |
| Compile errors in `audio_*` callbacks | Newer ESP32-audioI2S API | Install an earlier release or update the callbacks |
| Compile error about `Adafruit_SH1106G` or `SH110X_WHITE` | Adafruit SH110X library missing | Install **Adafruit SH110X** (not only Adafruit SSD1306) from the Library Manager |
| OLED blank | Wrong address, wiring or module | Run an I2C scanner; try `0x3D`; check 3V3, SDA=8, SCL=9; if your module has no pull-ups, add 4.7–10 kΩ from SDA and SCL to 3V3 |
| OLED pixels shifted or garbled | Module uses an SSD1306, not an SH1106 | Check the controller marking or listing; use `Adafruit_SH1106G`, not `Adafruit_SSD1306` |
| `SH1106 OLED initialization failed` in Serial | Wrong I2C address or wiring | Change `OLED_ADDRESS` from `0x3C` to `0x3D` (or vice versa); check wires |
| Encoder skips or double-counts | Contact bounce | Raise `ENC_DEBOUNCE_MS` to 8–12; add 100 nF capacitors CLK→GND and DT→GND |
| Encoder direction reversed | CLK/DT swapped | Swap the CLK and DT wires |
| Encoder button does nothing | Wiring | Check SW on GPIO17 and GND |
| Pin has no effect or board misbehaves | A GPIO label does not match the number used | Confirm you wired GPIO numbers, not header positions |
| `Wi-Fi connection timed out` | Wrong credentials, out of range, or 5 GHz network | Check credentials; use a 2.4 GHz network |
| `Stations loaded: 0` | API unreachable | Try another `RB_HOST` mirror; set Country and Genre to All; raise `RB_FETCH_LIMIT`; try `RB_CODEC "MP3"` |
| `JSON error: NoMemory` | Buffer too small, or PSRAM not in use | Confirm PSRAM is enabled; lower `RB_FETCH_LIMIT` |
| Station listed but no audio | Stream codec or format issue | Try another station; read Serial `Audio info:` messages; check I2S wiring |
| No sound, no errors | Amp in shutdown or wrong wiring | Tie SD/EN to 3V3; recheck DIN/BCLK/LRC |
| Audio stutters or drops | Weak Wi-Fi or a heavy stream | Check the signal bars (1 bar or less is weak); move closer to the router; lower `bitrateCap` |
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

### Wi-Fi signal indicator

`updateWifiLevel()` runs from `loop()` every `WIFI_CHECK_MS`. It reads `WiFi.RSSI()`, smooths it with a small moving average, converts it to 0–4 bars using the `WIFI_RSSI_*` thresholds, and only marks the screen for redraw when the bar count changes, so it adds no flicker and almost no load. `drawWifiIcon()` draws the bars on the playback screen only.

### UI state machine

```
              Hold (≥ 700 ms)
 MODE_NORMAL ─────────────────► MODE_BROWSE ───► MODE_EDIT
     ▲                              │                │
     └──── Hold / 10 s timeout ─────┘                │
     ◄──────────── Hold / 10 s timeout ──────────────┘
```

### Memory strategy

With 8 MB of PSRAM, large allocations such as the 32 KB JSON buffer and TLS buffers for https streams are served from PSRAM, leaving internal RAM free for Wi-Fi and the audio pipeline. The API response is still parsed with an ArduinoJson **filter** that keeps only `name`, `url_resolved`, `bitrate` and `countrycode`, which keeps memory use low and parsing fast. The request uses HTTP/1.0 to avoid chunked transfer encoding while stream-parsing. At boot the sketch verifies PSRAM is present and stops with a clear message if it is not.

### Extending with a new filter (e.g. language)

1. Add a `LanguageEntry` struct and `LANGUAGES[]` array, following `COUNTRIES[]`.
2. Add a focus item and extend `drawBrowseScreen()` / `drawEditScreen()`.
3. Add preview and selected variables and handle them in `handleEvent()`.
4. Append `&language=XX` to the URL in `fetchStationsInternal()`.

---

## Changes from the XIAO ESP32-S3 version

| Area | XIAO ESP32-S3 sketch | This ESP32-S3 dev board sketch |
|------|----------------------|--------------------------------|
| Pins | I2C 5/6, I2S 7/8/9, encoder 43/44/4 | I2C 8/9, I2S 4/5/6, encoder 15/16/17 |
| Reserved-pin handling | XIAO pads | Avoids flash/PSRAM (26–37), USB (19/20), UART0 (43/44), strapping (0, 3, 45, 46) and RGB LED pins |
| Display driver | SH1106, 1.3-inch | Same (SH1106, 1.3-inch) |
| PSRAM | Assumed present | Checked at boot, with an on-screen message if missing |
| Codec / HTTPS / JSON buffer | AAC, https, 32 KB | Same (AAC, https, 32 KB), enabled by PSRAM |
| Bitrate cap | 96 kbps | 128 kbps |
| Audio calls | Made from the fetch task | Made only from `loop()` |
| Wi-Fi power save | Default | Disabled |
| Hint text | Longer strings | Shortened to fit 128 px |
| Diagnostics | Basic | Heap and PSRAM printed at boot and after each fetch |
| Wi-Fi signal indicator | None | 4-bar indicator on the playback screen, plus dBm on serial |
| Board setting | XIAO_ESP32S3, OPI PSRAM | ESP32S3 Dev Module, OPI PSRAM, 8MB flash |

---

## Known limitations

| Feature | Status |
|---------|--------|
| Long title scrolling | Not implemented; text is truncated with `...` |
| Equalizer, NTP clock | Not implemented |
| Numeric dBm on the display | Not shown on screen (bars only); the value is printed on the serial monitor |
| Auto-advance when a stream ends | Not implemented (use the Browse menu) |
| Remembering the last station or volume | Not implemented (would need NVS / Preferences) |
| HTTPS certificate validation | Not performed (library default); fine for home use |
| 5 GHz Wi-Fi | Not supported; the ESP32-S3 radio is 2.4 GHz only |
| Bluetooth | Not used by this firmware |
| Boards without PSRAM | Not supported by this sketch; use the WROOM-32 sketch instead |

---

## Credits

| | |
|---|---|
| **Original project** | Tech Talkies YouTube channel: https://www.youtube.com/@techtalkies1 |
| **XIAO ESP32-S3 adaptation** | https://github.com/abhilashkb/internet-radio |
| **Audio library** | ESP32-audioI2S by schreibfaul1: https://github.com/schreibfaul1/ESP32-audioI2S |
| **Pinout reference** | Last Minute Engineers, ESP32-S3-DevKitC-1 pinout: https://lastminuteengineers.com/esp32-s3-devkitc-pinout-reference/ |
| **Station data** | Radio Browser community: https://www.radio-browser.info/ |
| **OLED driver** | Adafruit SH110X: https://github.com/adafruit/Adafruit_SH110x |
| **JSON parsing** | ArduinoJson by Benoit Blanchon: https://arduinojson.org/ |
