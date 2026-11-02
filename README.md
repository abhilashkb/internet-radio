# Internet Radio V2 — SH1106 OLED Edition

A SH1106 OLED adaptation of the Tech Talkies Internet Radio V2 project. This edition is intended for the **Seeed XIAO ESP32S3**, a **1.3-inch 128×64 SH1106 I2C OLED**, a **MAX98357A I2S mono amplifier**, a **KY-040 rotary encoder**, and an **8 Ω speaker**.

> **Hardware adaptation:** The original project uses an ST7735 SPI TFT display. This edition replaces that display with an SH1106 I2C OLED. The original TFT circuit diagram is not the wiring reference for this edition. The original 3D files are included as reference; the front panel may need redesigning for the smaller OLED.
>
> **Validation status:** The source has been adapted, but has not been verified by compiling against every possible ESP32 Arduino core/library version or by testing on the user's physical hardware. Compile and test each section before final soldering.

## Contents

- `101_Internet_Radio.ino` — adapted Arduino firmware
- `secrets.h` — Wi-Fi credentials to edit
- `3D/` — original enclosure STL files (`Top`, `Bottom`, `Front`, `Dial`)
- `circuit_original_tft.png` — original project's TFT circuit image, retained for reference only; do not use it as the SH1106 wiring diagram

## Hardware required

| Part | Qty | Notes |
|---|---:|---|
| Seeed XIAO ESP32S3 | 1 | Standard XIAO ESP32S3 is sufficient; Sense camera/microphone features are not required by this radio firmware |
| SH1106 OLED | 1 | 1.3-inch, 128×64, I2C, four pins: VCC, GND, SCL, SDA |
| MAX98357A amplifier module | 1 | I2S digital input, mono speaker output |
| KY-040 rotary encoder module | 1 | CLK, DT, SW, +, GND |
| Speaker | 1 | 8 Ω, 2 W nominal |
| USB-C cable and regulated 5 V supply | 1 each | Use a good-quality supply rated for at least 2 A for the complete radio |
| Wires, headers, prototyping board, enclosure hardware | as needed | Use insulated connections and secure the boards mechanically |

## Wiring — final SH1106 edition

### XIAO ESP32S3 pin map

| XIAO pin | ESP32-S3 GPIO | Connects to |
|---|---:|---|
| D3 | GPIO4 | KY-040 SW |
| D4 | GPIO5 | OLED SDA |
| D5 | GPIO6 | OLED SCL |
| D6 | GPIO43 | KY-040 CLK |
| D7 | GPIO44 | KY-040 DT |
| D8 | GPIO7 | MAX98357A DIN |
| D9 | GPIO8 | MAX98357A BCLK |
| D10 | GPIO9 | MAX98357A LRC / WS |
| 3V3 | 3.3 V | OLED VCC and KY-040 `+` |
| GND | Ground | OLED GND, KY-040 GND, MAX98357A GND |

**Why is the encoder SW pin on D3?** In the original TFT project, the encoder switch used GPIO6. In this OLED edition, D5/GPIO6 is used as I2C SCL, so the encoder switch is moved to D3/GPIO4. The firmware definitions have been updated accordingly.

### SH1106 OLED (I2C)

| OLED pin | XIAO ESP32S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | D4 / GPIO5 |
| SCL | D5 / GPIO6 |

The firmware uses I2C address `0x3C` by default. If the display is not detected, confirm the module's address with an I2C scanner and change `OLED_ADDRESS` to `0x3D` if required. The display reset argument is `-1` because the four-wire module has no separate reset connection.

### MAX98357A I2S amplifier

| MAX98357A pin | Connection |
|---|---|
| VIN | Regulated 5 V rail (see power notes below) |
| GND | Common GND with XIAO |
| DIN | XIAO D8 / GPIO7 |
| BCLK | XIAO D9 / GPIO8 |
| LRC / WS | XIAO D10 / GPIO9 |
| SD / EN | Follow the breakout board's enable/default configuration; do not leave disabled if the board requires this pin to be pulled high |
| GAIN | Leave at module default initially; configure only after checking the specific breakout documentation |
| SPK+ / OUT+ | One speaker terminal |
| SPK− / OUT− | The other speaker terminal |

**Speaker warning:** Connect the speaker only between `SPK+` and `SPK−` (also labelled `OUT+` and `OUT−` on some boards). Neither speaker terminal is ground. Do not connect either speaker wire to GND.

### KY-040 rotary encoder

| KY-040 pin | XIAO ESP32S3 |
|---|---|
| CLK | D6 / GPIO43 |
| DT | D7 / GPIO44 |
| SW | D3 / GPIO4 |
| `+` | 3V3 |
| GND | GND |

Power the encoder from 3.3 V so its logic signals are suitable for the ESP32-S3 GPIO. Do not power it from 5 V.

## Power and grounding

- For initial testing, power the XIAO through its USB-C connector from a regulated 5 V USB supply.
- The amplifier VIN needs a suitable 5 V supply. If taking 5 V from the XIAO's USB 5 V/VBUS rail, verify that the board's power path and connector can safely supply the amplifier's current. For a separate regulated 5 V amplifier supply, connect its GND to XIAO GND so the I2S signals have a common reference.
- Do **not** connect two independent 5 V outputs together. Do not apply 5 V to the OLED VCC or any ESP32-S3 GPIO.
- Use a regulated supply rated for at least 5 V, 2 A for the overall prototype, with suitable wiring. Actual current demand depends on speaker volume and the specific modules.
- Disconnect power before changing wiring. Check polarity and continuity before first power-up.

## OLED screens and rotary-encoder operation

The 128×64 monochrome display has three UI modes.

### Normal / playback screen

Displays:
- `INTERNET RADIO` header
- `VOL` label and a small volume bar, or `MUTE`
- `NOW PLAYING`
- Station name or stream title metadata, shortened with `...` when it is too long
- Station country and listed bitrate
- Hint: `Turn: Volume  Press: Mute`

The stream title changes only when the station sends metadata supported by the audio library. The station name is used as the initial title/fallback. The current implementation does not scroll long titles.

### Browse menu

The menu contains:
- **Station** — browse the stations currently loaded
- **Country** — choose a country filter
- **Genre** — choose a genre/tag filter

The selected menu row is highlighted by inverting its foreground/background pixels.

### Selection screen

- Station selection shows a shortened station name, its position in the loaded list, and bitrate.
- Country selection shows the country being previewed.
- Genre selection shows the genre/tag being previewed.

### Encoder controls

| Current mode | Action | Result |
|---|---|---|
| Playback | Rotate clockwise | Increase volume |
| Playback | Rotate counter-clockwise | Decrease volume |
| Playback | Short press | Toggle mute/unmute |
| Playback | Hold for more than 700 ms | Open Browse menu |
| Browse menu | Rotate | Move between Station, Country, Genre |
| Browse menu | Short press | Open the highlighted selection screen |
| Browse menu | Long press | Return to playback |
| Selection | Rotate | Preview next/previous station, country, or genre |
| Selection | Short press | Confirm selection; station plays or filter is applied |
| Selection | Long press | Return to Browse menu without confirming the current preview |

The Browse/Selection UI automatically returns to playback after 10 seconds without an encoder action.

## Radio functionality

- Connects to Wi-Fi using credentials in `secrets.h`.
- Requests station listings from the Radio Browser API.
- Filters stations by selected country and genre/tag.
- Requests up to 20 stations per query, with broken stations hidden and an AAC/bitrate filter applied by the current source.
- Plays the selected stream through the MAX98357A I2S amplifier.
- Updates the displayed stream title when metadata is supplied by the station.

Station availability, stream format, metadata, and API responses are controlled by third-party services and individual broadcasters. Not every station provides song metadata or remains online.

## Software requirements

Install these libraries using Arduino IDE Library Manager (or the library's official installation instructions):

| Library | Purpose / source |
|---|---|
| ESP32 Arduino board package | XIAO ESP32S3 board support; install the Espressif `esp32` platform and select the XIAO ESP32S3-compatible board entry |
| ESP32-audioI2S | Audio streaming and I2S output — <https://github.com/schreibfaul1/ESP32-audioI2S> |
| ArduinoJson | Radio Browser JSON parsing — <https://arduinojson.org/> |
| Adafruit GFX Library | OLED graphics primitives — <https://github.com/adafruit/Adafruit-GFX-Library> |
| Adafruit SH110X | SH1106 OLED driver — <https://github.com/adafruit/Adafruit_SH110x> |

The original ST7735 display library is not needed for this SH1106 edition.

## Arduino IDE setup and upload

1. Extract this project folder.
2. Open the `101_Internet_Radio` sketch folder. The `.ino` file and `secrets.h` must be in the same Arduino sketch folder. If Arduino IDE asks to create a matching folder for the sketch, keep the filenames/folder consistent.
3. Open `101_Internet_Radio.ino`.
4. Edit `secrets.h` and enter your Wi-Fi details:

   ```cpp
   #define WIFI_SSID "your_network_name"
   #define WIFI_PASSWORD "your_password"
   ```

5. Install all required libraries listed above.
6. Select the XIAO ESP32S3 board entry and the correct USB port. Board menu labels can vary with the installed ESP32 board package.
7. Compile before connecting the amplifier/speaker. Resolve any library-version or board-package errors first.
8. Upload the firmware and open Serial Monitor at **115200 baud** for startup and connection diagnostics.
9. Test the OLED, encoder, Wi-Fi/station list, and audio separately before final soldering.

## Recommended staged hardware test

1. **Controller:** Upload a basic XIAO ESP32S3 sketch and confirm USB/serial works.
2. **OLED:** Connect only VCC, GND, SDA, SCL; run an I2C scanner; confirm `0x3C` or `0x3D`; test SH1106 text output.
3. **Encoder:** Connect the KY-040 and verify clockwise, counter-clockwise, short press, and long press.
4. **Amplifier:** With power off, wire I2S and speaker; check speaker is only across SPK+/SPK−; power up at low volume.
5. **Radio:** Add Wi-Fi credentials, confirm Wi-Fi and station retrieval in Serial Monitor, then test station playback and metadata.

## Known limitations / not yet implemented

The current adapted source does **not** implement:
- Wi-Fi signal-strength icon or RSSI display
- Scrolling marquee for long station/song titles (long text is shortened with `...`)
- Animated equalizer
- Clock / NTP time display
- Playback progress bar or elapsed-time display
- A dedicated connection/reconnection status icon on the normal playback screen

These can be added in a later firmware revision. Do not assume a feature is present just because it appeared in a concept/mock-up image.

## Enclosure note

The included STL files come from the original ST7735 TFT design. The original `Front.stl` opening and mounting points may not align with a 1.3-inch SH1106 OLED. Measure your exact OLED PCB and modify the front panel before printing. Check the board, speaker, encoder, USB-C access, and cable clearances against your actual components.

## Troubleshooting

| Symptom | Checks |
|---|---|
| OLED stays blank | Check 3.3 V/GND polarity, SDA on D4/GPIO5, SCL on D5/GPIO6, I2C address `0x3C`/`0x3D`, and that the module is SH1106-compatible |
| OLED shows corrupted/shifted graphics | Confirm the controller is SH1106 (not SSD1306) and use the Adafruit SH110X driver |
| Encoder direction is reversed | Swap CLK and DT in wiring or adjust direction handling; keep SW on D3/GPIO4 |
| Encoder button does nothing | Check SW-to-D3/GPIO4 and GND; the firmware uses `INPUT_PULLUP` and an active-low switch |
| No stations found | Check Wi-Fi, internet access, Radio Browser API availability, selected filters, and Serial Monitor output |
| Station listed but no audio | Try another station; verify stream URL/codec compatibility, amplifier VIN/GND, I2S pin mapping, SD/EN state, and speaker wiring |
| Audio is noisy or resets occur | Check 5 V supply capacity, common ground, wire quality, and power distribution; reduce volume during testing |

## Credits

Original project: Tech Talkies YouTube channel — <https://www.youtube.com/@techtalkies1>

Original project repository: <https://github.com/TechTalkies/YouTube/tree/main/101%20Internet%20Radio%20V2>

Radio Browser: <https://www.radio-browser.info/>
