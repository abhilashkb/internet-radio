/* -------------------------------------------------
   Internet Radio - ESP32 WROOM-32 edition

   Target board : ESP32 WROOM-32 dev board (USB-C, CH340, dual core,
                  Wi-Fi + Bluetooth). Bluetooth is not used; its
                  controller memory is released to give Wi-Fi and
                  audio more heap.
   Based on     : abhilashkb/internet-radio (XIAO ESP32S3 + SH1106)
                  Original project by Tech Talkies YouTube channel.
                  https://www.youtube.com/@techtalkies1

   Hardware
     - Display : 1.3" SH1106 128x64 I2C OLED
     - Amp     : MAX98357A I2S mono amplifier + 4-8 ohm speaker
     - Control : KY-040 rotary encoder with push switch

   WIRING (ESP32 WROOM-32 dev board)
     OLED SDA        -> GPIO21
     OLED SCL        -> GPIO22
     OLED VCC / GND  -> 3V3 / GND
     MAX98357A DIN   -> GPIO25
     MAX98357A BCLK  -> GPIO26
     MAX98357A LRC   -> GPIO27
     MAX98357A VIN   -> 5V (VIN pin of the dev board)
     MAX98357A GND   -> GND
     MAX98357A SD    -> leave unconnected, or tie to 3V3 if the
                        board has no built-in pull-up
     KY-040 CLK      -> GPIO32
     KY-040 DT       -> GPIO33
     KY-040 SW       -> GPIO13
     KY-040 +  / GND -> 3V3 / GND   (3.3 V, never 5 V)

   Pins deliberately avoided: GPIO6-11 (flash), GPIO0/2/5/12/15
   (boot strapping), GPIO34-39 (input only, no internal pull-up).

   ARDUINO IDE SETTINGS
     Board            : ESP32 Dev Module
     PSRAM            : Disabled (WROOM-32 has none)
     Partition scheme : "Minimal SPIFFS (1.9MB APP with OTA)" or
                        "No OTA (2MB APP/2MB SPIFFS)" - the default
                        1.2 MB app partition is usually too small
     Upload speed     : 460800 or 115200 if uploads fail
     Serial monitor   : 115200 baud
     If upload hangs on "Connecting...", hold the BOOT button until
     writing starts.

   LIBRARIES
     ESP32-audioI2S (schreibfaul1), ArduinoJson 6.x (NOT 7.x),
     Adafruit GFX, Adafruit SH110X, ESP32 board package (Espressif)

   Changes from the ESP32-S3 version
     - Pin map moved to WROOM-32 safe GPIOs
     - No PSRAM: station JSON is parsed with an ArduinoJson filter so
       only the 4 needed fields are stored (small heap footprint)
     - HTTP/1.0 request avoids chunked encoding while stream-parsing
     - Default stream codec is MP3 (HE-AAC is heavy without PSRAM)
     - HTTPS streams skipped by default (TLS needs lots of RAM)
     - audio.* calls are made only from loop(), never from the fetch
       task on the other core (the audio library is not thread-safe)
     - Wi-Fi power save disabled for steadier streaming
     - Hint texts shortened so they fit 128 px (21 characters)

   Credentials: edit WIFI_SSID / WIFI_PASSWORD below. Do not commit
   them to a public repository.
------------------------------------------------- */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#ifdef CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif

// ---------------- Wi-Fi credentials ----------------
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---------------- OLED: SH1106 I2C ----------------
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDRESS 0x3C   // Try 0x3D if your module uses that address
#define OLED_SDA     21
#define OLED_SCL     22

// ---------------- I2S audio pins ------------------
#define I2S_DOUT 25   // MAX98357A DIN
#define I2S_BCLK 26   // MAX98357A BCLK
#define I2S_LRCK 27   // MAX98357A LRC / WS

// ---------------- Rotary encoder ------------------
#define ENC_CLK 32
#define ENC_DT  33
#define ENC_SW  13

// ---------------- Radio settings ------------------
#define RB_HOST "http://de1.api.radio-browser.info"

#define STATION_ARRAY_SIZE 40   // stations kept in RAM
#define RB_FETCH_LIMIT     60   // asked from the API (some get filtered out)
#define RB_CODEC           "MP3" // "MP3" is safest without PSRAM; "AAC" also possible
#define ALLOW_HTTPS        0    // 1 = also accept https:// streams (needs lots of RAM)
#define JSON_DOC_SIZE      20480 // heap for the filtered JSON result

#define VOL_MAX        21
#define INITIAL_VOLUME 18

// ---------------- Wi-Fi reconnect -----------------
#define WIFI_RECONNECT_INTERVAL_MS 15000UL

// ---------------- Layout constants ----------------
// Normal screen - single-line mode
#define NS_HEADER_Y   0
#define NS_DIVIDER1_Y 13
#define NS_LABEL_Y    17
#define NS_TITLE_Y    29
#define NS_META_Y     40

// Normal screen - two-line mode
#define NS_STNAME_Y   27
#define NS_STMETA_Y   36
#define NS_META2_Y    46
#define NS_DIVIDER2_Y 53
#define NS_HINT_Y     56

// Browse screen
#define BS_DIVIDER_Y   12
#define BS_ROW_Y_START 17
#define BS_ROW_STEP    13
#define BS_DIVIDER2_Y  56
#define BS_HINT_Y      58

// Edit screen
#define ES_DIVIDER_Y  12
#define ES_CENTER1_Y  20
#define ES_CENTER2_Y  33
#define ES_PROGBAR_Y  42
#define ES_CENTER3_Y  49
#define ES_COUNTRY_Y  28
#define ES_LABEL_Y    43
#define ES_DIVIDER2_Y 56
#define ES_HINT_Y     58

// Volume bar
#define VOL_LABEL_X 88
#define VOL_BAR_X   88
#define VOL_BAR_Y   10
#define VOL_BARS    8

#define SHORT_TEXT_MAX  18
#define ENC_DEBOUNCE_MS 4

struct CountryEntry {
  const char* code;
  const char* name;
};

static const CountryEntry COUNTRIES[] = {
  { "all", "All" }, { "US", "USA" }, { "IN", "India" },
  { "GB", "UK" }, { "DE", "Germany" }, { "FR", "France" },
  { "JP", "Japan" }, { "CA", "Canada" }, { "AU", "Australia" },
  { "IT", "Italy" }, { "ES", "Spain" }, { "BR", "Brazil" },
  { "MX", "Mexico" }, { "NL", "Netherlands" }, { "SE", "Sweden" },
  { "NO", "Norway" }, { "ZA", "South Africa" }, { "SG", "Singapore" },
  { "AE", "UAE" }
};
static const int COUNTRY_COUNT = sizeof(COUNTRIES) / sizeof(COUNTRIES[0]);

struct GenreEntry {
  const char* tag;
  const char* label;
};

static const GenreEntry GENRES[] = {
  { "all", "All" }, { "music", "Music" }, { "news", "News" },
  { "jazz", "Jazz" }, { "classical", "Classical" }, { "rock", "Rock" }
};
static const int GENRE_COUNT = sizeof(GENRES) / sizeof(GENRES[0]);

Audio audio;
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
QueueHandle_t encQueue = nullptr;

enum EncEvent  { EV_CW, EV_CCW, EV_PRESS, EV_LONG };
enum UiMode    { MODE_NORMAL, MODE_BROWSE, MODE_EDIT };
enum FocusItem { F_NOWPLAYING, F_COUNTRY, F_TYPE };

struct Station {
  String name;
  String url;
  int bitrate;
  String country;
};

Station stations[STATION_ARRAY_SIZE];
int stationCount = 0;
int currentStation = 0;
int focusIndex = F_NOWPLAYING;

UiMode uiMode = MODE_NORMAL;
bool uiDirty = true;
String streamTitle = "Connecting...";

int previewStation = 0;
String previewTag = "all";
String previewCountry = "all";
String selectedGenre = "all";
String selectedCountry = "all";

int bitrateCap = 96;
bool muted = false;
int lastVol = INITIAL_VOLUME;

uint32_t browseLastAction = 0;
volatile uint32_t holdStartMs = 0;
volatile bool buttonHolding = false;

// Async fetch state
volatile bool fetchInProgress = false;  // true from request until loop() finishes handling
volatile bool fetchDone = false;        // set by the fetch task when it has finished
volatile bool fetchOk = false;          // result of the fetch
TaskHandle_t fetchTaskHandle = nullptr;
static String fetchGenreArg;
static String fetchCountryArg;

uint32_t lastWifiRetryMs = 0;

// --------------------------------------------------
// Helpers
// --------------------------------------------------
String countryName(String c) {
  c.toUpperCase();
  for (int i = 0; i < COUNTRY_COUNT; i++) {
    String code = String(COUNTRIES[i].code);
    code.toUpperCase();
    if (c == code) return String(COUNTRIES[i].name);
  }
  if (c.length() == 0) return "All";
  return c;
}

String typeName() {
  for (int i = 0; i < GENRE_COUNT; i++) {
    if (selectedGenre == GENRES[i].tag) return String(GENRES[i].label);
  }
  return selectedGenre;
}

String previewGenreName() {
  for (int i = 0; i < GENRE_COUNT; i++) {
    if (previewTag == GENRES[i].tag) return String(GENRES[i].label);
  }
  return previewTag;
}

String shortText(String value, uint8_t maxChars = SHORT_TEXT_MAX) {
  if (value.length() <= maxChars) return value;
  if (maxChars <= 3) return value.substring(0, maxChars);
  return value.substring(0, maxChars - 3) + "...";
}

void drawCentered(const String& text, int y, uint8_t textSize = 1) {
  display.setTextSize(textSize);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  int x = (OLED_WIDTH - (int)w) / 2;
  if (x < 0) x = 0;
  display.setCursor(x, y);
  display.print(text);
}

// --------------------------------------------------
// Drawing
// --------------------------------------------------
void drawVolume() {
  int vol = audio.getVolume();
  display.setTextSize(1);
  display.setCursor(VOL_LABEL_X, NS_HEADER_Y);
  display.print(muted ? "MUTE" : "VOL");

  for (int i = 0; i < VOL_BARS; i++) {
    int h = 2 + (i / 2) + (i % 2);   // bar heights 2,3,3,4,4,5,5,6
    int x = VOL_BAR_X + i * 5;
    int y = VOL_BAR_Y + (8 - h);
    if (!muted && i < (vol * VOL_BARS / VOL_MAX)) {
      display.fillRect(x, y, 3, h, SH110X_WHITE);
    } else {
      display.drawRect(x, y, 3, h, SH110X_WHITE);
    }
  }
}

void drawLoadingScreen() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  drawCentered("INTERNET RADIO", 10);
  display.drawLine(0, NS_DIVIDER1_Y, 127, NS_DIVIDER1_Y, SH110X_WHITE);
  drawCentered("Loading stations...", 28);
  drawCentered("Please wait", 42);
  display.display();
}

void drawNormalScreen() {
  display.setTextSize(1);
  display.setCursor(0, NS_HEADER_Y);
  display.print("INTERNET RADIO");
  drawVolume();
  display.drawLine(0, NS_DIVIDER1_Y, 127, NS_DIVIDER1_Y, SH110X_WHITE);

  display.setCursor(0, NS_LABEL_Y);
  display.print("NOW PLAYING");
  if (stationCount > 0) {
    String idx = String(currentStation + 1) + "/" + String(stationCount);
    display.setCursor(OLED_WIDTH - (int)idx.length() * 6, NS_LABEL_Y);
    display.print(idx);
  }

  if (stationCount == 0) {
    drawCentered("No stations loaded", 31);
    drawCentered("Check Wi-Fi / API", 43);
  } else {
    String stName = stations[currentStation].name;
    String stTitle = (streamTitle.length() > 0) ? streamTitle : stName;
    bool hasMeta = (streamTitle.length() > 0 && streamTitle != stName);
    String country = countryName(stations[currentStation].country);
    String meta = shortText(country, 9) + "|" + String(stations[currentStation].bitrate) + "k";

    display.setTextSize(1);
    if (hasMeta) {
      display.setCursor(0, NS_STNAME_Y);
      display.print(shortText(stName, SHORT_TEXT_MAX));
      display.setCursor(0, NS_STMETA_Y);
      display.print(shortText(stTitle, SHORT_TEXT_MAX));
      display.setCursor(0, NS_META2_Y);
      display.print(meta);
    } else {
      display.setCursor(0, NS_TITLE_Y);
      display.print(shortText(stTitle, SHORT_TEXT_MAX));
      display.setCursor(0, NS_META_Y);
      display.print(meta);
    }
  }

  display.drawLine(0, NS_DIVIDER2_Y, 127, NS_DIVIDER2_Y, SH110X_WHITE);
  display.setCursor(0, NS_HINT_Y);
  display.print("Turn:Vol  Press:Mute");
}

void drawBrowseScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("BROWSE MENU");
  display.drawLine(0, BS_DIVIDER_Y, 127, BS_DIVIDER_Y, SH110X_WHITE);

  const char* labels[] = { "Station", "Country", "Genre" };
  for (int i = 0; i < 3; i++) {
    int y = BS_ROW_Y_START + i * BS_ROW_STEP;
    if (i == focusIndex) {
      display.fillRect(0, y - 1, 128, 11, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    } else {
      display.setTextColor(SH110X_WHITE);
    }
    display.setCursor(3, y);
    display.print(labels[i]);
    display.setCursor(70, y);

    if (i == F_NOWPLAYING) {
      if (stationCount > 0) display.print(shortText(stations[currentStation].name, 9));
      else display.print("Empty");
    } else if (i == F_COUNTRY) {
      display.print(shortText(countryName(selectedCountry), 9));
    } else {
      display.print(shortText(typeName(), 9));
    }
  }
  display.setTextColor(SH110X_WHITE);

  display.drawLine(0, BS_DIVIDER2_Y, 127, BS_DIVIDER2_Y, SH110X_WHITE);
  display.setCursor(0, BS_HINT_Y);
  display.print("Press:Edit Hold:Exit");
}

void drawEditScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (focusIndex == F_NOWPLAYING)      display.print("SELECT STATION");
  else if (focusIndex == F_COUNTRY)    display.print("SELECT COUNTRY");
  else                                 display.print("SELECT GENRE");

  display.drawLine(0, ES_DIVIDER_Y, 127, ES_DIVIDER_Y, SH110X_WHITE);

  if (focusIndex == F_NOWPLAYING) {
    if (stationCount > 0) {
      drawCentered(shortText(stations[previewStation].name, 20), ES_CENTER1_Y);
      drawCentered(String(previewStation + 1) + "/" + String(stationCount), ES_CENTER2_Y);

      const int barW = 80;
      const int barX = (OLED_WIDTH - barW) / 2;
      const int barH = 3;
      display.drawRect(barX, ES_PROGBAR_Y, barW, barH, SH110X_WHITE);
      int filled = (stationCount > 1)
                     ? (previewStation * (barW - 2)) / (stationCount - 1)
                     : (barW - 2);
      display.fillRect(barX + 1, ES_PROGBAR_Y, filled, barH, SH110X_WHITE);

      drawCentered(String(stations[previewStation].bitrate) + " kbps", ES_CENTER3_Y);
    } else {
      drawCentered("No stations available", ES_COUNTRY_Y);
    }
  } else if (focusIndex == F_COUNTRY) {
    drawCentered("< " + countryName(previewCountry) + " >", ES_COUNTRY_Y, 1);
    drawCentered("Turn to pick country", ES_LABEL_Y);
  } else {
    drawCentered("< " + previewGenreName() + " >", ES_COUNTRY_Y, 1);
    drawCentered("Turn to pick genre", ES_LABEL_Y);
  }

  display.drawLine(0, ES_DIVIDER2_Y, 127, ES_DIVIDER2_Y, SH110X_WHITE);
  display.setCursor(0, ES_HINT_Y);
  display.print("Turn:Change Press:OK");
}

void drawUI() {
  if (fetchInProgress) {
    drawLoadingScreen();
    return;
  }

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  if (uiMode == MODE_NORMAL)      drawNormalScreen();
  else if (uiMode == MODE_BROWSE) drawBrowseScreen();
  else                            drawEditScreen();

  if (buttonHolding) {
    int progress = min(12, (int)((millis() - holdStartMs) / 60));
    display.drawRect(116, 0, 12, 12, SH110X_WHITE);
    display.fillRect(118, 10 - progress, 8, progress, SH110X_WHITE);
  }

  display.display();
  uiDirty = false;
}

// --------------------------------------------------
// Station fetch (runs on core 0 in a short-lived task)
// The task ONLY downloads and parses. All audio.* calls are done
// afterwards from loop(), because the audio library is not
// thread-safe.
// --------------------------------------------------
bool fetchStationsInternal(const String& tag, const String& country) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;

  String url = String(RB_HOST) +
               "/json/stations/search?limit=" + String(RB_FETCH_LIMIT) +
               "&hidebroken=true&order=clickcount&reverse=true&codec=" RB_CODEC
               "&bitrateMax=" + String(bitrateCap);
  if (tag != "all")     url += "&tag=" + tag;
  if (country != "all") url += "&countrycode=" + country;

  http.begin(url);
  http.useHTTP10(true);        // no chunked encoding, safe to stream-parse
  http.setTimeout(12000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Station request failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  // Keep only the 4 fields we use; the rest of each record is discarded
  // while parsing, so a small heap buffer is enough (no PSRAM needed).
  StaticJsonDocument<192> filter;
  filter[0]["name"] = true;
  filter[0]["url_resolved"] = true;
  filter[0]["bitrate"] = true;
  filter[0]["countrycode"] = true;

  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error =
    deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (error) {
    Serial.print("JSON error: ");
    Serial.println(error.c_str());
    return false;
  }

  stationCount = 0;
  for (JsonObject s : doc.as<JsonArray>()) {
    String u = s["url_resolved"] | "";
    bool okScheme = u.startsWith("http://");
#if ALLOW_HTTPS
    okScheme = okScheme || u.startsWith("https://");
#endif
    if (!okScheme) continue;

    stations[stationCount].name    = s["name"] | "Unknown station";
    stations[stationCount].url     = u;
    stations[stationCount].bitrate = s["bitrate"] | 0;
    stations[stationCount].country = s["countrycode"] | "";
    stationCount++;
    if (stationCount >= STATION_ARRAY_SIZE) break;
  }

  Serial.printf("Stations loaded: %d (free heap %u)\n", stationCount, ESP.getFreeHeap());
  return stationCount > 0;
}

void taskFetch(void* p) {
  fetchOk = fetchStationsInternal(fetchGenreArg, fetchCountryArg);
  fetchDone = true;            // loop() picks this up and starts playback
  fetchTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void applyChange() {
  if (fetchInProgress) return;   // prevent double-fetch
  fetchGenreArg = selectedGenre;
  fetchCountryArg = selectedCountry;
  fetchDone = false;
  fetchInProgress = true;
  uiDirty = true;
  xTaskCreatePinnedToCore(taskFetch, "fetch", 10240, nullptr, 1, &fetchTaskHandle, 0);
}

// Called from loop() once the fetch task has finished.
void finishFetch() {
  if (fetchOk) {
    currentStation = 0;
    streamTitle = "";
    audio.stopSong();
    audio.connecttohost(stations[0].url.c_str());
    streamTitle = stations[0].name;
  } else {
    stationCount = 0;
    currentStation = 0;
    streamTitle = "No stations found";
    audio.stopSong();
  }
  fetchInProgress = false;
  uiDirty = true;
}

void playStation(int i) {
  if (i < 0 || i >= stationCount) return;
  currentStation = i;
  streamTitle = "";
  audio.stopSong();
  audio.connecttohost(stations[i].url.c_str());
  streamTitle = stations[i].name;
  uiDirty = true;
}

// --------------------------------------------------
// Encoder events
// --------------------------------------------------
void handleEvent(uint8_t ev) {
  browseLastAction = millis();

  if (uiMode == MODE_NORMAL) {
    if (ev == EV_CW) {
      muted = false;
      audio.setVolume(min(VOL_MAX, (int)audio.getVolume() + 1));
    } else if (ev == EV_CCW) {
      muted = false;
      audio.setVolume(max(0, (int)audio.getVolume() - 1));
    } else if (ev == EV_PRESS) {
      muted = !muted;
      if (muted) {
        lastVol = audio.getVolume();
        audio.setVolume(0);
      } else {
        audio.setVolume(lastVol);
      }
    } else if (ev == EV_LONG) {
      uiMode = MODE_BROWSE;
      previewStation = currentStation;
      previewTag = selectedGenre;
      previewCountry = selectedCountry;
    }

  } else if (uiMode == MODE_BROWSE) {
    if (ev == EV_CW)         focusIndex = (focusIndex + 1) % 3;
    else if (ev == EV_CCW)   focusIndex = (focusIndex + 2) % 3;
    else if (ev == EV_PRESS) uiMode = MODE_EDIT;
    else if (ev == EV_LONG)  uiMode = MODE_NORMAL;

  } else if (uiMode == MODE_EDIT) {
    if (focusIndex == F_NOWPLAYING) {
      if (ev == EV_CW && stationCount > 0)
        previewStation = (previewStation + 1) % stationCount;
      else if (ev == EV_CCW && stationCount > 0)
        previewStation = (previewStation - 1 + stationCount) % stationCount;
      else if (ev == EV_PRESS && stationCount > 0) {
        playStation(previewStation);
        uiMode = MODE_BROWSE;
      } else if (ev == EV_LONG) {
        uiMode = MODE_BROWSE;
      }

    } else if (focusIndex == F_COUNTRY) {
      if (ev == EV_CW || ev == EV_CCW) {
        int ci = 0;
        for (int i = 0; i < COUNTRY_COUNT; i++) {
          if (previewCountry == COUNTRIES[i].code) { ci = i; break; }
        }
        int dir = (ev == EV_CW) ? 1 : -1;
        ci = (ci + dir + COUNTRY_COUNT) % COUNTRY_COUNT;
        previewCountry = COUNTRIES[ci].code;
      } else if (ev == EV_PRESS) {
        selectedCountry = previewCountry;
        applyChange();
        uiMode = MODE_BROWSE;
      } else if (ev == EV_LONG) {
        uiMode = MODE_BROWSE;
      }

    } else if (focusIndex == F_TYPE) {
      if (ev == EV_CW || ev == EV_CCW) {
        int gi = 0;
        for (int i = 0; i < GENRE_COUNT; i++) {
          if (previewTag == GENRES[i].tag) { gi = i; break; }
        }
        int dir = (ev == EV_CW) ? 1 : -1;
        gi = (gi + dir + GENRE_COUNT) % GENRE_COUNT;
        previewTag = GENRES[gi].tag;
      } else if (ev == EV_PRESS) {
        selectedGenre = previewTag;
        applyChange();
        uiMode = MODE_BROWSE;
      } else if (ev == EV_LONG) {
        uiMode = MODE_BROWSE;
      }
    }
  }

  uiDirty = true;
}

// Encoder polling task with debounce guard on CLK edges
void taskRotary(void* p) {
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);

  int lastClk = digitalRead(ENC_CLK);
  bool lastBtn = HIGH;
  uint32_t pressAt = 0;
  uint32_t lastEncMs = 0;

  for (;;) {
    int clk = digitalRead(ENC_CLK);
    uint32_t now = millis();

    if (clk != lastClk && clk == LOW) {
      if (now - lastEncMs >= ENC_DEBOUNCE_MS) {
        uint8_t event = digitalRead(ENC_DT) ? EV_CW : EV_CCW;
        xQueueSend(encQueue, &event, 0);
        lastEncMs = now;
      }
    }
    lastClk = clk;

    bool btn = digitalRead(ENC_SW);
    if (btn == LOW && lastBtn == HIGH) {
      pressAt = millis();
      holdStartMs = pressAt;
      buttonHolding = true;
    }
    if (btn == HIGH && lastBtn == LOW) {
      buttonHolding = false;
      uint8_t event = (millis() - pressAt > 700) ? EV_LONG : EV_PRESS;
      xQueueSend(encQueue, &event, 0);
    }
    lastBtn = btn;

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// --------------------------------------------------
// Setup / loop
// --------------------------------------------------
void setup() {
#ifdef CONFIG_BT_ENABLED
  // Bluetooth is unused: hand its controller memory back to the heap.
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif

  Serial.begin(115200);
  delay(200);
  Serial.printf("Free heap at boot: %u\n", ESP.getFreeHeap());

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);

  if (!display.begin(OLED_ADDRESS, true)) {
    Serial.println("SH1106 OLED initialization failed. Check wiring/address.");
    while (true) delay(1000);
  }

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  drawCentered("INTERNET RADIO", 18);
  drawCentered("Starting...", 34);
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // steadier streaming
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  display.clearDisplay();
  drawCentered("Connecting Wi-Fi", 25);
  display.display();

  Serial.print("Connecting to Wi-Fi");
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection timed out. Check WIFI_SSID / WIFI_PASSWORD (2.4 GHz only) and restart.");
    streamTitle = "Wi-Fi unavailable";
  }

  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audio.setVolume(INITIAL_VOLUME);

  encQueue = xQueueCreate(16, sizeof(uint8_t));
  if (encQueue == nullptr) {
    Serial.println("Failed to create encoder queue");
    while (true) delay(1000);
  }
  xTaskCreatePinnedToCore(taskRotary, "rotary", 4096, nullptr, 1, nullptr, 1);

  if (WiFi.status() == WL_CONNECTED) {
    applyChange();   // initial load is also async
  }

  uiDirty = true;
}

void loop() {
  audio.loop();

  // Fetch task finished: start playback from this (audio) context.
  if (fetchDone) {
    fetchDone = false;
    finishFetch();
  }

  uint8_t event;
  while (encQueue != nullptr && xQueueReceive(encQueue, &event, 0) == pdTRUE) {
    if (!fetchInProgress) handleEvent(event);   // ignore input while loading
  }

  // Browse/edit timeout - reset preview state to committed values
  if ((uiMode == MODE_BROWSE || uiMode == MODE_EDIT) &&
      millis() - browseLastAction > 10000) {
    uiMode = MODE_NORMAL;
    previewStation = currentStation;
    previewTag = selectedGenre;
    previewCountry = selectedCountry;
    uiDirty = true;
  }

  // Wi-Fi reconnect with 15 s cool-down
  if (WiFi.status() != WL_CONNECTED) {
    uint32_t now = millis();
    if (now - lastWifiRetryMs > WIFI_RECONNECT_INTERVAL_MS) {
      lastWifiRetryMs = now;
      Serial.println("Wi-Fi lost. Attempting reconnect...");
      WiFi.reconnect();

      uint32_t t = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) delay(200);

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Wi-Fi reconnected.");
        applyChange();   // reload stations after reconnect
      }
    }
  }

  if (uiDirty) drawUI();

  delay(1);
}

// --------------------------------------------------
// ESP32-audioI2S callbacks
// --------------------------------------------------
void audio_showstreamtitle(const char* info) {
  if (info != nullptr && strlen(info) > 0) {
    streamTitle = String(info);
    uiDirty = true;
  }
}

void audio_info(const char* info) {
  Serial.print("Audio info: ");
  Serial.println(info);
}

// Called for any stream end, not only MP3. No auto-advance by design;
// pick another station from the Browse menu.
void audio_eof_mp3(const char* info) {
  Serial.print("Audio stream ended: ");
  Serial.println(info);
  streamTitle = "Stream ended";
  uiDirty = true;
}
