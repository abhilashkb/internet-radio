# Internet Radio — SH1106 OLED Edition

A DIY Wi-Fi internet radio built around the **Seeed XIAO ESP32S3**, a **1.3-inch SH1106 I2C OLED**, a **MAX98357A I2S mono amplifier**, a **KY-040 rotary encoder**, and an 8 Ω speaker. Streams live radio from the open [Radio Browser](https://www.radio-browser.info/) directory. No subscription, no app, no cloud account required.

> **Hardware adaptation note:** The original Tech Talkies project used an ST7735 SPI colour TFT. This edition replaces it with a monochrome SH1106 OLED and relocates the encoder switch pin to resolve an I2C conflict. The original TFT wiring diagram is **not** applicable here. Use only the wiring tables in this document.

> **Validation status:** Firmware has been reviewed and improved but must be compiled and tested on your specific hardware. Library versions and board-package revisions can introduce compatibility differences. Compile first, then test each hardware section before soldering permanently.

---

## Table of contents

1. [How it works](#how-it-works)
2. [Bill of materials](#bill-of-materials)
3. [Tools and consumables](#tools-and-consumables)
4. [Wiring](#wiring)
5. [Power and grounding](#power-and-grounding)
6. [Software setup](#software-setup)
7. [Arduino IDE configuration](#arduino-ide-configuration)
8. [Firmware customisation](#firmware-customisation)
9. [Operation manual](#operation-manual)
10. [Staged hardware test procedure](#staged-hardware-test-procedure)
11. [Troubleshooting](#troubleshooting)
12. [Firmware architecture](#firmware-architecture)
13. [Known limitations](#known-limitations)
14. [Enclosure notes](#enclosure-notes)
15. [Credits](#credits)

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

1. On boot the XIAO ESP32S3 connects to your home Wi-Fi network.
2. It queries the public **Radio Browser API** for a list of streaming stations, filtered by your chosen country and genre.
3. The selected station URL is passed to the **ESP32-audioI2S** library, which fetches the audio stream over HTTP/HTTPS and decodes it in real time.
4. Decoded audio is sent over **I2S** to the MAX98357A digital amplifier, which drives the speaker.
5. The SH1106 OLED shows playback status, volume, and station metadata. The KY-040 encoder controls volume, muting, and the browse menu.
6. A FreeRTOS background task handles station fetching so audio playback is never interrupted when you change filters.

---

## Bill of materials

| # | Part | Qty | Key specs | Notes |
|---|------|----:|-----------|-------|
| 1 | Seeed XIAO ESP32S3 | 1 | Xtensa LX7 dual-core, 8 MB psRAM, 8 MB flash, USB-C | Standard (non-Sense) version is sufficient; Sense camera/mic features are unused |
| 2 | SH1106 OLED module | 1 | 1.3-inch, 128 × 64, I2C, 4-pin (VCC GND SCL SDA) | **Must be SH1106**, not SSD1306 — the controllers are different and the driver matters |
| 3 | MAX98357A amplifier breakout | 1 | I2S input, class-D mono, 3.2 W into 4 Ω / 1.8 W into 8 Ω at 5 V | Adafruit #3006 or compatible clone; check which pins are broken out on your board |
| 4 | KY-040 rotary encoder module | 1 | Incremental, 20 pulses/rev, built-in push switch, 3.3–5 V | Buy the module (with PCB and pull-up resistors), not the bare encoder |
| 5 | Speaker | 1 | 8 Ω, 2 W or more | A small 2-inch or 3-inch full-range driver works well |
| 6 | USB-C power supply | 1 | 5 V, ≥ 2 A regulated | A phone charger rated 2 A or more; use a short, good-quality cable |
| 7 | Prototyping board or custom PCB | 1 | As needed | Perforated board or custom PCB for permanent assembly |
| 8 | Dupont jumper wires (for testing) | assorted | M-F and M-M | For initial breadboard testing before soldering |
| 9 | Pin headers | assorted | 2.54 mm | For socketing the XIAO and modules |
| 10 | Solder, flux, heat shrink | as needed | — | Standard electronics grade |

### Where to source

- **Seeed XIAO ESP32S3** — [seeedstudio.com](https://www.seeedstudio.com/) or distributors (Mouser, Digi-Key, LCSC)
- **SH1106 OLED** — AliExpress, Amazon (search "1.3 inch SH1106 I2C OLED"); confirm the controller chip before ordering
- **MAX98357A** — Adafruit #3006 (most reliable), or clone breakouts from AliExpress/Amazon
- **KY-040 encoder module** — Any electronics hobby supplier or AliExpress

---

## Tools and consumables

- Soldering iron (temperature-controlled, ≤ 350 °C tip)
- Rosin-core solder (0.6–0.8 mm)
- Flux pen or paste (optional but recommended for fine-pitch work)
- Wire stripper and cutters
- Multimeter (continuity and DC voltage)
- USB-C cable (data-capable, for programming)
- Computer with Arduino IDE 2.x installed
- (Optional) 3D printer for the enclosure

---

## Wiring

> **Before wiring:** Power off everything. Check each connection with a multimeter for continuity before applying power. Incorrect wiring can damage the XIAO, the OLED, or the amplifier.

### Master pin assignment

| XIAO label | ESP32-S3 GPIO | Connects to | Signal |
|-----------|:------------:|-------------|--------|
| D3 | GPIO4 | KY-040 SW | Encoder push switch |
| D4 | GPIO5 | OLED SDA | I2C data |
| D5 | GPIO6 | OLED SCL | I2C clock |
| D6 | GPIO43 | KY-040 CLK | Encoder channel A |
| D7 | GPIO44 | KY-040 DT | Encoder channel B |
| D8 | GPIO7 | MAX98357A DIN | I2S serial data |
| D9 | GPIO8 | MAX98357A BCLK | I2S bit clock |
| D10 | GPIO9 | MAX98357A LRC / WS | I2S word select / frame sync |
| 3V3 | 3.3 V rail | OLED VCC, KY-040 `+` | 3.3 V supply |
| GND | Ground | OLED GND, KY-040 GND, MAX98357A GND | Common ground |

> **Why is the encoder SW on D3 (GPIO4)?** The original Tech Talkies project used GPIO6 for the encoder switch. In this edition, GPIO6 is occupied by I2C SCL for the OLED. The switch is therefore moved to GPIO4 (D3), which is free. The firmware `ENC_SW` define reflects this.

---

### SH1106 OLED (I2C)

```
OLED module          XIAO ESP32S3
──────────           ────────────
VCC         ──────►  3V3
GND         ──────►  GND
SDA         ──────►  D4 (GPIO5)
SCL         ──────►  D5 (GPIO6)
```

- I2C address is `0x3C` by default. Some modules ship with `0x3D` — confirm with an I2C scanner sketch if the display stays blank.
- The firmware passes reset pin `-1` because this 4-wire module has no dedicated hardware reset line.
- Keep SDA/SCL wires short (< 20 cm) and away from the I2S lines to avoid noise pickup.

---

### MAX98357A I2S amplifier

```
MAX98357A module     XIAO ESP32S3
────────────         ────────────
VIN         ──────►  5V rail (see Power section)
GND         ──────►  GND (common with XIAO)
DIN         ──────►  D8 (GPIO7)
BCLK        ──────►  D9 (GPIO8)
LRC / WS    ──────►  D10 (GPIO9)
SD / EN     ──────►  See note below
GAIN        ──────►  See note below
SPK+        ──────►  Speaker terminal 1
SPK-        ──────►  Speaker terminal 2
```

**SD / EN pin:** Many breakout boards tie this high internally for always-on operation. If yours has the pin exposed, pull it high (to 3.3 V through a 100 kΩ resistor, or connect to 3V3) unless your board's documentation says otherwise. A floating SD/EN will leave the amplifier in shutdown.

**GAIN pin:** The default (GAIN unconnected) gives 9 dB gain on Adafruit's board. For louder output connect GAIN to VIN (+15 dB); for quieter connect GAIN to GND (+6 dB). Adjust only after reading your specific board's datasheet.

> **Speaker warning:** Connect the speaker only between `SPK+` and `SPK-`. **Neither terminal is ground.** Connecting either speaker wire to GND will short the amplifier output and may damage the IC.

---

### KY-040 rotary encoder

```
KY-040 module        XIAO ESP32S3
─────────────        ────────────
CLK         ──────►  D6 (GPIO43)
DT          ──────►  D7 (GPIO44)
SW          ──────►  D3 (GPIO4)
+           ──────►  3V3
GND         ──────►  GND
```

- Power the encoder from **3.3 V**, not 5 V. The XIAO ESP32S3 GPIO inputs are not 5 V tolerant.
- The firmware uses `INPUT_PULLUP` on all three pins. The KY-040 module already includes pull-up resistors, so the internal pull-ups serve as a safety net.
- If rotation direction is reversed, swap CLK and DT in the wiring (do not change the SW wire).

---

## Power and grounding

### How power flows

Power for the entire radio comes from a single **5 V / 2 A USB-C phone charger**. You plug it directly into the **built-in USB-C connector on the XIAO ESP32S3** — no soldering of USB wires to the PCB is required.

The XIAO acts as the central power hub:

```
     5V / 2A PHONE CHARGER
               |
            USB-C
               |
               v
        +----------------+
        | XIAO ESP32S3   |
        |                |
        | 5V / VBUS -----+---- MAX98357A VIN
        |                |
        | 3V3 -----------+---- OLED VCC
        |                +---- KY-040 +
        |                |
        | GND -----------+---- COMMON GND
        +----------------+
```

### Connection steps

1. Plug one end of your USB-C cable into the 5 V / 2 A phone charger.
2. Plug the other end into the **USB-C connector on the XIAO ESP32S3 board** (the board's own built-in port).
3. The XIAO receives 5 V through this connector and powers itself.
4. Wire the XIAO's `5V` / VBUS pin to the MAX98357A VIN for the amplifier's 5 V supply.
5. Wire the XIAO's `3V3` pin to the OLED VCC and the KY-040 `+` pin.
6. Connect all module GNDs to the XIAO GND to establish a common ground.

### Rail summary

| Rail | Voltage | Source | Powered components |
|------|:-------:|--------|-------------------|
| USB-C VBUS | 5 V | Phone charger | XIAO ESP32S3 (input) |
| XIAO 5V pin | 5 V | XIAO VBUS passthrough | MAX98357A VIN |
| XIAO 3V3 pin | 3.3 V | XIAO on-board LDO regulator | SH1106 OLED, KY-040 encoder |
| GND | 0 V | Common | All modules |

> **Note on XIAO 5V current capacity:** The XIAO's `5V` pin is a direct passthrough from the USB VBUS line. A 2 A charger provides ample headroom for the ESP32-S3 (~250 mA typical) and the MAX98357A at moderate volumes (~300–500 mA). Do not exceed the charger's rated current; do not use cheap cables that have high resistance.

### Permanent installation

You can solder the XIAO onto your permanent PCB using **pin headers**, leaving the board's USB-C connector fully accessible from the outside of the radio enclosure. This gives you:
- Power input from the phone charger
- Serial port access for firmware updates via the same connector

> **Important:** Do **not** solder the phone charger's USB-C cable wires directly to the XIAO's GPIO pins or power pads as a substitute for the USB-C connector. Always use the board's built-in USB-C port for power input.

### Safety rules

- Never apply 5 V to the OLED VCC — it is a 3.3 V device.
- Never apply 5 V to any XIAO GPIO pin — the ESP32-S3 is not 5 V tolerant.
- Ensure all GNDs (XIAO, OLED, encoder, amplifier) share a single common connection point.
- Disconnect power completely before changing any wiring.

---

## Software setup

### Required libraries

Install all of the following using **Arduino IDE > Sketch > Include Library > Manage Libraries**, or manually from the links provided.

| Library | Version guidance | Source |
|---------|-----------------|--------|
| **esp32** board package (Espressif) | 2.0.x or 3.x | Arduino IDE Board Manager — search `esp32` by Espressif |
| **ESP32-audioI2S** | Latest | [github.com/schreibfaul1/ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) |
| **ArduinoJson** | 6.x (not 7.x — API differs) | [arduinojson.org](https://arduinojson.org/) |
| **Adafruit GFX Library** | Latest | Arduino Library Manager |
| **Adafruit SH110X** | Latest | Arduino Library Manager |

> **ArduinoJson version note:** The sketch uses `DynamicJsonDocument`, which is the ArduinoJson 6.x API. If you have ArduinoJson 7.x installed, you will need to either downgrade to 6.x or migrate the JSON parsing code to the 7.x API (`JsonDocument` without `Dynamic`).

The **ST7735** display library used in the original TFT project is **not required** and should not be included.

---

## Arduino IDE configuration

1. **Install the board package:**
   - Open Arduino IDE > Preferences and add the Espressif boards URL to Additional Boards Manager URLs:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Open Boards Manager, search `esp32` by Espressif, and install.

2. **Select the board:**
   - Tools > Board > ESP32 Arduino > **XIAO_ESP32S3** (or the closest matching entry in your package version).

3. **Enable psRAM** *(important — required for the 32 KB JSON buffer):*
   - Tools > PSRAM > **OPI PSRAM**

4. **Partition scheme:** The default partition scheme is fine for this sketch.

5. **Upload speed:** 921600 baud (default) works for most USB-C cables.

6. **Enter Wi-Fi credentials:**
   - Open `secrets.h` in the same sketch folder.
   - Replace the placeholder values:
     ```cpp
     #define WIFI_SSID     "your_network_name"
     #define WIFI_PASSWORD "your_password"
     ```
   - Save the file. `secrets.h` is listed in `.gitignore` and will not be committed if you use Git.

7. **Compile first:** Press the Verify button (checkmark) without a connected board to catch library/version errors before uploading.

8. **Upload:** Connect the XIAO via USB-C, select the correct COM/serial port, and press Upload.

9. **Serial Monitor:** Open at **115200 baud** to read startup diagnostics:
   ```
   Connecting to Wi-Fi.......
   Wi-Fi connected: 192.168.1.42
   Stations loaded: 18
   Audio info: ...
   ```

---

## Firmware customisation

All tuneable values are `#define` constants near the top of `101_Internet_Radio.ino`. Edit them in Arduino IDE before uploading.

| Constant | Default | Effect |
|----------|:-------:|--------|
| `OLED_ADDRESS` | `0x3C` | I2C address of the SH1106 module; try `0x3D` if blank on boot |
| `STATION_ARRAY_SIZE` | `40` | Maximum stations fetched and stored; also sets the API `limit=` parameter |
| `VOL_MAX` | `21` | Maximum volume level (ESP32-audioI2S range 0–21) |
| `INITIAL_VOLUME` | `18` | Volume set at startup |
| `bitrateCap` | `96` | Maximum stream bitrate in kbps returned by the API — increase to 128 or 320 for higher-quality streams if your internet is fast enough |
| `WIFI_RECONNECT_INTERVAL_MS` | `15000` | Milliseconds between Wi-Fi reconnect attempts after a connection loss |
| `ENC_DEBOUNCE_MS` | `4` | Minimum milliseconds between accepted encoder pulses — increase if your encoder produces erratic jumps |
| `RB_HOST` | `de1.api.radio-browser.info` | Radio Browser mirror to query; alternatives: `nl1`, `at1`, `fr1` |

### Adding countries

Edit the `COUNTRIES[]` array in the sketch. Each entry is `{ "CODE", "Display Name" }` where `CODE` is the two-letter ISO 3166-1 alpha-2 country code:

```cpp
{ "KR", "South Korea" },
{ "PT", "Portugal" },
```

No other changes are needed — `countryName()` and the browse menu both loop over this array automatically.

### Adding genres

Edit the `GENRES[]` array. Each entry is `{ "api_tag", "Display Label" }` where `api_tag` is the Radio Browser tag string:

```cpp
{ "ambient",   "Ambient"   },
{ "hiphop",    "Hip-Hop"   },
```

---

## Operation manual

The radio has a single control: the **KY-040 rotary encoder**. Rotating it adjusts settings; pressing its built-in switch takes actions. The display has three screens.

### Screen 1 — Normal / playback

This is the default screen shown during playback.

```
+----------------------------------+
| INTERNET RADIO         VOL [|||] |  <- Header + volume bar
|----------------------------------|  <- Divider
| NOW PLAYING                3/15  |  <- Label + station index
| BBC Radio 4                      |  <- Station name
| In Our Time (BBC Radio 4)        |  <- Stream metadata (when available)
| UK|128k                          |  <- Country + bitrate
|----------------------------------|  <- Divider
| Turn: Volume  Press: Mute        |  <- Hint
+----------------------------------+
```

**Display elements:**

| Element | Description |
|---------|-------------|
| `VOL` / `MUTE` | Current audio state. Volume bar shows 8 graduated segments; all outlined (hollow) when muted |
| `NOW PLAYING` | Static label |
| `3/15` | Station index in the current list (current position / total loaded) |
| Station name | The name from the Radio Browser database, truncated to 18 characters |
| Stream metadata | Song or show title sent by the broadcaster. Only shown when it differs from the station name. Falls back to station name until real metadata arrives |
| `UK\|128k` | Country code and bitrate from the Radio Browser database |
| Hint bar | Reminder of encoder actions in this mode |

**Encoder actions in Normal mode:**

| Action | Result |
|--------|--------|
| Rotate clockwise | Increase volume (unmutes if muted) |
| Rotate counter-clockwise | Decrease volume (unmutes if muted) |
| Short press (< 700 ms) | Toggle mute / unmute |
| Hold >= 700 ms | Open Browse menu |

---

### Screen 2 — Browse menu

Hold the encoder button from the playback screen to open this menu.

```
+----------------------------------+
| BROWSE MENU                      |
|----------------------------------|
|[Station      BBC Radio 4        ]|  <- Highlighted (selected) row
| Country      UK                  |
| Genre        All                 |
|----------------------------------|
| Press: Edit  Hold: Exit          |
+----------------------------------+
```

**Display elements:**

| Row | Left column | Right column |
|-----|-------------|--------------|
| Station | `Station` | Current station name (truncated to 9 chars) |
| Country | `Country` | Currently active country filter |
| Genre | `Genre` | Currently active genre filter |

The highlighted row (white background, black text) is the selected item.

**Encoder actions in Browse mode:**

| Action | Result |
|--------|--------|
| Rotate CW / CCW | Move selection highlight through the three rows |
| Short press | Open edit screen for the highlighted item |
| Long press (>= 700 ms) | Return to playback without changes |
| No input for 10 s | Automatically returns to playback |

---

### Screen 3 — Edit / selection

Press the encoder in Browse mode to open the edit screen for the highlighted item. There are three variants.

#### 3a — Select station

```
+----------------------------------+
| SELECT STATION                   |
|----------------------------------|
|                                  |
|       BBC World Service          |  <- Station name
|            3/15                  |  <- Position in list
|    [====             ]           |  <- Progress bar
|          128 kbps                |  <- Bitrate
|----------------------------------|
| Turn: Change  Press: OK          |
+----------------------------------+
```

- The progress bar shows your position across the loaded station list visually.
- Rotating scrolls through all loaded stations without starting playback.
- Press to confirm — the selected station starts playing and the display returns to Browse.

#### 3b — Select country

```
+----------------------------------+
| SELECT COUNTRY                   |
|----------------------------------|
|                                  |
|           < India >              |  <- Preview value with scroll arrows
|                                  |
|      Turn to change country      |
|                                  |
|----------------------------------|
| Turn: Change  Press: OK          |
+----------------------------------+
```

- `< >` arrows show the value is scrollable with the encoder.
- Rotating previews the next/previous country without fetching yet.
- **Press** to confirm — a new station list is fetched in the background. A loading screen appears. The previous station continues playing until the new list is ready.
- **Long press** to cancel without changing the filter.

#### 3c — Select genre

Same layout as country selection, scrolling through the genre list.

**Available genres:** All · Music · News · Jazz · Classical · Rock

**Available countries:** All · USA · India · UK · Germany · France · Japan · Canada · Australia · Italy · Spain · Brazil · Mexico · Netherlands · Sweden · Norway · South Africa · Singapore · UAE

**Encoder actions in Edit mode:**

| Action | Result |
|--------|--------|
| Rotate CW | Next station / country / genre |
| Rotate CCW | Previous station / country / genre |
| Short press | Confirm selection |
| Long press | Cancel — return to Browse without applying |
| No input for 10 s | Auto-return to playback; preview discarded |

---

### Loading screen

Displayed automatically while fetching a new station list after a country or genre change.

```
+----------------------------------+
|                                  |
|        INTERNET RADIO            |
|----------------------------------|
|                                  |
|      Loading stations...         |
|          Please wait             |
|                                  |
+----------------------------------+
```

Audio continues playing during the fetch. The screen returns to Browse when the list is ready, or shows "No stations found" if the API returns no results for the selected filters.

---

### Volume and mute

| Volume level | Display |
|:---:|---------|
| 1–21 | `VOL` label + 8 bars (filled, increasing height left to right) |
| 0 / muted | `MUTE` label + all bars shown as outlines only |

Volume is stored in RAM only. On power cycle the radio starts at `INITIAL_VOLUME` (default: 18 out of 21).

### Wi-Fi reconnection (automatic)

If the Wi-Fi connection is lost after startup, the firmware automatically attempts to reconnect every 15 seconds. When reconnection succeeds, a fresh station list is fetched and playback resumes. No manual action is needed.

---

## Staged hardware test procedure

Test each hardware subsystem independently before combining. This isolates faults and avoids confusing multi-component failures.

### Stage 1 — XIAO ESP32S3 alone

1. Connect XIAO to PC via USB-C.
2. Open Arduino IDE, select the XIAO ESP32S3 board and the correct COM port.
3. Upload `File > Examples > 01.Basics > Blink`.
4. Confirm the LED blinks and Serial Monitor shows output at 115200 baud.

### Stage 2 — SH1106 OLED

1. Wire: XIAO 3V3 to OLED VCC, GND to GND, D4 to SDA, D5 to SCL.
2. Upload an I2C scanner sketch (available at [arduino.cc](https://playground.arduino.cc/Main/I2cScanner/)).
3. Open Serial Monitor — confirm `0x3C` or `0x3D` is detected.
4. Upload a simple SH1106 text example from the Adafruit SH110X library (`File > Examples > Adafruit SH110X`).
5. Confirm text appears without corruption or offset.

> If you see shifted or partially offset pixels, your module may be labelled SH1106 but uses a different controller. Check with the seller or try the SSD1306 library as a diagnostic step.

### Stage 3 — KY-040 rotary encoder

1. Keep the OLED connected.
2. Wire: KY-040 CLK to D6, DT to D7, SW to D3, `+` to 3V3, GND to GND.
3. Upload a simple encoder test sketch that prints CW, CCW, and PRESS to Serial Monitor.
4. Verify clockwise rotation prints CW, counter-clockwise prints CCW, short press prints PRESS.
5. If CW and CCW are swapped, swap the CLK and DT wires (not SW).

### Stage 4 — MAX98357A amplifier and speaker

1. **Power off before wiring the amplifier.**
2. Wire I2S: D8 to DIN, D9 to BCLK, D10 to LRC.
3. Wire power: 5 V to VIN, GND to GND.
4. Wire speaker between SPK+ and SPK- only. Confirm with a multimeter that neither speaker wire reads continuity to GND before powering on.
5. Power on. Upload a tone-generator or simple audio playback test sketch.
6. Confirm clear audio at low volume before increasing the level.

### Stage 5 — Full radio firmware

1. Ensure `secrets.h` has valid Wi-Fi credentials.
2. Ensure `Tools > PSRAM > OPI PSRAM` is selected.
3. Upload `101_Internet_Radio.ino`.
4. Open Serial Monitor at 115200 baud. Confirm:
   - `Connecting to Wi-Fi...` then `Wi-Fi connected: x.x.x.x`
   - `Stations loaded: N` (N > 0)
   - `Audio info: ...` lines appearing as the stream starts
5. Verify audio through the speaker.
6. Test the full encoder flow: adjust volume, mute, long-press to open Browse, select a different station, confirm.

---

## Troubleshooting

| Symptom | Likely cause | Checks and fixes |
|---------|-------------|-----------------|
| OLED stays blank | Wrong module, bad wiring, or wrong I2C address | Run I2C scanner; confirm address is `0x3C` or `0x3D`; check 3V3 and GND; verify SDA on D4/GPIO5 and SCL on D5/GPIO6 |
| OLED shows shifted or garbled pixels | Module uses SSD1306, not SH1106 | Check the controller chip marking; use `Adafruit_SH1106G`, not `Adafruit_SSD1306` |
| Sketch fails to compile | Library version mismatch | Confirm ArduinoJson is version 6.x; confirm Adafruit SH110X (not SH1306) is installed; update the ESP32 board package |
| `SH1106 OLED initialization failed` in Serial | Wrong I2C address | Change `OLED_ADDRESS` from `0x3C` to `0x3D` (or vice versa) in the sketch defines |
| Encoder rotation skips or double-counts | Bouncy encoder contacts | Increase `ENC_DEBOUNCE_MS` from 4 to 8 or 12; optionally add 100 nF capacitors between CLK/GND and DT/GND |
| Encoder direction reversed | CLK and DT wires swapped | Swap the CLK and DT wires only; do not change SW |
| Encoder button does nothing | SW wiring or pull-up issue | Check SW is on D3/GPIO4 and GND; firmware uses `INPUT_PULLUP` so the switch must pull the pin LOW when pressed |
| `Wi-Fi connection timed out` | Wrong credentials or out of range | Check `secrets.h`; confirm the network is 2.4 GHz (ESP32-S3 does not support 5 GHz); move closer to the router |
| `Stations loaded: 0` | API unreachable or no matching results | Check internet access; try a different `RB_HOST` mirror (nl1, at1, fr1); set Country and Genre both to "All" |
| Station listed but no audio | Stream URL or codec incompatible | Try a different station; check Serial Monitor for audio library error messages; confirm I2S wiring |
| Audio is distorted or clipped | Volume too high or PSU under-voltage | Reduce volume; use a better USB supply (>= 2 A); shorten USB cable |
| Audio has hiss or hum | Ground loop or PSU noise | Ensure all GNDs share a single connection; use a linear-regulated supply or quality SMPS |
| ESP32 resets randomly under load | PSU cannot supply peak current | Replace USB supply with a higher-rated unit; add a 470 uF / 10 V capacitor across the amplifier VIN and GND |
| `JSON error:` in Serial Monitor | JSON buffer overflow (psRAM not enabled) | Ensure `Tools > PSRAM > OPI PSRAM` is selected before compiling; the 32 KB `DynamicJsonDocument` requires psRAM heap |
| Loading screen never clears | Fetch task failed | Check Serial for HTTP error codes; verify internet connectivity; try a different `RB_HOST` mirror |
| Stream stops and nothing plays | Stream ended or station went offline | Select a new station from the Browse menu; the radio does not auto-advance by design |

---

## Firmware architecture

Understanding the code structure helps with customisation and debugging.

### Task layout (FreeRTOS)

| Task | Core | Priority | Stack | Role |
|------|:----:|:--------:|------:|------|
| `loop()` (Arduino main) | 1 | 1 | Default | `audio.loop()`, encoder event dispatch, UI redraw, Wi-Fi watchdog |
| `taskRotary` | 1 | 1 | 4 KB | Polls CLK/DT/SW at 2 ms intervals, applies debounce, enqueues events |
| `taskFetch` | 0 | 1 | 8 KB | HTTP fetch and JSON parse; spawned on demand, self-deletes on completion |

> **Why Core 0 for fetching?** `audio.loop()` runs on Core 1. HTTP requests can block for up to 12 seconds. Running the fetch on Core 0 ensures audio is never starved.

### UI state machine

```
              Long press (>= 700 ms)
 MODE_NORMAL ─────────────────────► MODE_BROWSE ───► MODE_EDIT
     ^                                   |                |
     |        Long press / 10 s timeout  |                | Long press / 10 s
     └───────────────────────────────────┘                |
     ◄────────────────────────────────────────────────────┘
                      Press OK (confirm selection)
```

### Key data flow

```
audio_showstreamtitle()  ──►  streamTitle  ──►  drawNormalScreen()
taskFetch()              ──►  stations[]   ──►  drawBrowseScreen() / drawEditScreen()
taskRotary()             ──►  encQueue     ──►  handleEvent()
                                                     |
                                          ┌──────────┴──────────┐
                                     applyChange()        playStation()
                                    (async fetch)         (immediate)
```

### Extending with a new filter (e.g. language)

1. Add a `LanguageEntry` struct and `LANGUAGES[]` array, following the `COUNTRIES[]` pattern.
2. Add `F_LANGUAGE` to the `FocusItem` enum and extend `drawBrowseScreen()` and `drawEditScreen()`.
3. Add a `previewLanguage` / `selectedLanguage` variable and handle it in `handleEvent()`.
4. Append `&language=XX` to the URL in `fetchStationsInternal()`.

---

## Known limitations

| Feature | Status |
|---------|--------|
| Long title scrolling (marquee) | Not implemented — text truncated with `...` at 18 characters |
| Wi-Fi signal strength / RSSI display | Not implemented |
| Animated equalizer on playback screen | Not implemented |
| NTP clock display | Not implemented |
| Auto-advance to next station on stream end | Not implemented (intentional — use Browse menu) |
| Saving last-played station across power cycles | Not implemented — requires NVS / Preferences |
| HTTPS certificate validation | Disabled (ESP32-audioI2S default) — acceptable for home use |
| 5 GHz Wi-Fi | Not supported — ESP32-S3 radio is 2.4 GHz only |

---

## Enclosure notes

The `3D/` folder contains STL files from the **original ST7735 TFT project**. The TFT display opening will **not** match a 1.3-inch SH1106 OLED. Modifications needed before printing:

- Measure your OLED PCB dimensions precisely (typical: ~36 mm × 26 mm, visible area ~29 mm × 15 mm).
- Modify `Front.stl` to match your OLED cutout size and mounting hole pattern.
- Verify the speaker cutout size matches your chosen driver.
- Check that the USB-C port on the XIAO is accessible for reprogramming after assembly.
- Ensure the encoder shaft protrudes far enough through the panel.
- Allow clearance for wiring behind the OLED and around the amplifier board.

Parametric OLED enclosure templates are available on [Thingiverse](https://www.thingiverse.com) and [Printables](https://www.printables.com) — search "1.3 inch OLED enclosure".

---

## Credits

| | |
|---|---|
| **Original project** | Tech Talkies YouTube channel — https://www.youtube.com/@techtalkies1 |
| **Original repository** | https://github.com/TechTalkies/YouTube/tree/main/101%20Internet%20Radio%20V2 |
| **Audio library** | ESP32-audioI2S by Wolle (schreibfaul1) — https://github.com/schreibfaul1/ESP32-audioI2S |
| **Station data** | Radio Browser community — https://www.radio-browser.info/ |
| **OLED driver** | Adafruit SH110X — https://github.com/adafruit/Adafruit_SH110x |
| **JSON parsing** | ArduinoJson by Benoit Blanchon — https://arduinojson.org/ |
