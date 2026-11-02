/* -------------------------------------------------
   Copyright (c)
   Original Arduino project by Tech Talkies YouTube Channel.
   https://www.youtube.com/@techtalkies1

   SH1106 adaptation:
   - Display: 1.3-inch SH1106, 128x64, I2C
   - OLED SDA: XIAO ESP32S3 D4 / GPIO5
   - OLED SCL: XIAO ESP32S3 D5 / GPIO6
   - Encoder SW moved from GPIO6 to GPIO4 (D3) to avoid I2C conflict
   - Original I2S audio pins retained

   Improvements applied (v2):
   - Fix #1:  fetchStations offloaded to FreeRTOS task (non-blocking)
   - Fix #2:  DynamicJsonDocument increased to 32 KB (psRAM available)
   - Fix #3:  countryName() now loops COUNTRIES[] — no divergence risk
   - Fix #4:  Encoder debounce guard (4 ms min inter-event interval)
   - Fix #6:  Wi-Fi reconnect logic with 15 s cool-down
   - Fix #7:  streamTitle reset on station switch
   - Fix #8:  Preview state reset on browse timeout
   - Fix #9:  Volume bar uses monotonic height ramp
   - Fix #10: RB_RESULT_LIMIT used in API URL; renamed STATION_ARRAY_SIZE
   - Fix #11: "Loading..." screen shown during station fetch
   - Fix #12: shortText safe limit reduced to 18 chars
   - Fix #13: Named layout constants replace magic pixel values
   - Fix #14: audio_eof_mp3 comment clarified (no auto-advance)
   - Fix #15: .gitignore added for secrets.h
   UI improvements (v3):
   - UI#1: Station index (e.g. "3/15") right-aligned on NOW PLAYING row
   - UI#2: Two-line content mode — station name + stream title when metadata arrives
   - UI#3: Browse menu Station row shows current station name (not "Select")
   - UI#4: Edit screen — < value > arrows + mini progress bar for station list
   ------------------------------------------------- */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "secrets.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ---------------- OLED: SH1106 I2C ----------------
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDRESS 0x3C       // Try 0x3D if your module uses that address
#define OLED_SDA     5          // XIAO D4
#define OLED_SCL     6          // XIAO D5

// ---------------- I2S audio pins -------------------
#define I2S_DOUT 7              // XIAO D8
#define I2S_BCLK 8              // XIAO D9
#define I2S_LRCK 9              // XIAO D10

// ---------------- Rotary encoder ------------------
#define ENC_CLK 43              // XIAO D6
#define ENC_DT  44              // XIAO D7
#define ENC_SW  4               // XIAO D3 (moved from GPIO6; GPIO6 is OLED SCL)

// ---------------- Radio settings ------------------
#define RB_HOST          "http://de1.api.radio-browser.info"
// Fix #10: STATION_ARRAY_SIZE controls the stations[] array AND the API limit.
#define STATION_ARRAY_SIZE 40
#define VOL_MAX          21
#define INITIAL_VOLUME   18

// ---------------- Wi-Fi reconnect -----------------
#define WIFI_RECONNECT_INTERVAL_MS 15000UL

// ---------------- Layout constants (Fix #13) ------
// Normal screen — single-line mode
#define NS_HEADER_Y    0
#define NS_DIVIDER1_Y  13
#define NS_LABEL_Y     17
#define NS_TITLE_Y     29    // single-line: only title shown here
#define NS_META_Y      40    // single-line: country | bitrate
// Normal screen — two-line mode (when stream metadata differs from station name)
#define NS_STNAME_Y    27    // two-line: station name
#define NS_STMETA_Y    36    // two-line: stream title / song metadata
#define NS_META2_Y     46    // two-line: country | bitrate
#define NS_DIVIDER2_Y  53
#define NS_HINT_Y      56
// Browse screen
#define BS_DIVIDER_Y   12
#define BS_ROW_Y_START 17
#define BS_ROW_STEP    13
#define BS_DIVIDER2_Y  56
#define BS_HINT_Y      58
// Edit screen
#define ES_DIVIDER_Y   12
#define ES_CENTER1_Y   20    // station name (slightly higher for progress bar)
#define ES_CENTER2_Y   33    // station index X/N
#define ES_PROGBAR_Y   42    // station progress bar
#define ES_CENTER3_Y   49    // bitrate
#define ES_COUNTRY_Y   28    // country / genre value
#define ES_LABEL_Y     43
#define ES_DIVIDER2_Y  56
#define ES_HINT_Y      58
// Volume bar
#define VOL_LABEL_X    88
#define VOL_BAR_X      88
#define VOL_BAR_Y      10
#define VOL_BARS       8

// ---------------- Safe short-text limit (Fix #12) -
#define SHORT_TEXT_MAX 18

// ----------- Encoder debounce (Fix #4) -------------
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

enum EncEvent { EV_CW, EV_CCW, EV_PRESS, EV_LONG };
enum UiMode { MODE_NORMAL, MODE_BROWSE, MODE_EDIT };
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

// Fix #1: Async fetch state
volatile bool fetchInProgress = false;
volatile bool fetchRequested = false;
TaskHandle_t fetchTaskHandle = nullptr;

// Fix #6: Wi-Fi reconnect state
uint32_t lastWifiRetryMs = 0;

// --------------------------------------------------
// Fix #3: countryName() now loops COUNTRIES[]
//         No divergence risk if entries are added.
// --------------------------------------------------
String countryName(String c) {
  c.toUpperCase();
  for (int i = 0; i < COUNTRY_COUNT; i++) {
    String code = String(COUNTRIES[i].code);
    code.toUpperCase();
    if (c == code) return String(COUNTRIES[i].name);
  }
  if (c.length() == 0) return "All";
  return c; // fallback: return raw code
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

// Fix #12: default maxChars capped to SHORT_TEXT_MAX (18)
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

// Fix #9: Monotonic volume bar ramp (heights increase left→right)
void drawVolume() {
  int vol = audio.getVolume();
  display.setTextSize(1);
  display.setCursor(VOL_LABEL_X, NS_HEADER_Y);
  display.print(muted ? "MUTE" : "VOL");
  for (int i = 0; i < VOL_BARS; i++) {
    // Monotonic ramp: bar heights 2,3,3,4,4,5,5,6
    int h = 2 + (i / 2) + (i % 2);
    int x = VOL_BAR_X + i * 5;
    int y = VOL_BAR_Y + (8 - h);
    if (!muted && i < (vol * VOL_BARS / VOL_MAX)) {
      display.fillRect(x, y, 3, h, SH110X_WHITE);
    } else {
      display.drawRect(x, y, 3, h, SH110X_WHITE);
    }
  }
}

// Fix #11: Loading screen
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

  // UI#1: "NOW PLAYING" label + station index right-aligned on the same row
  display.setCursor(0, NS_LABEL_Y);
  display.print("NOW PLAYING");
  if (stationCount > 0) {
    String idx = String(currentStation + 1) + "/" + String(stationCount);
    // Each char is 6 px wide at textSize 1
    display.setCursor(OLED_WIDTH - (int)idx.length() * 6, NS_LABEL_Y);
    display.print(idx);
  }

  if (stationCount == 0) {
    drawCentered("No stations loaded", 31);
    drawCentered("Check Wi-Fi / API", 43);
  } else {
    String stName  = stations[currentStation].name;
    String stTitle = (streamTitle.length() > 0) ? streamTitle : stName;
    // UI#2: Two-line mode — show station name AND stream metadata
    //        when the station is sending a song/show title different from its name.
    bool hasMeta = (streamTitle.length() > 0 && streamTitle != stName);
    String country = countryName(stations[currentStation].country);
    String meta    = shortText(country, 9) + "|" + String(stations[currentStation].bitrate) + "k";

    display.setTextSize(1);
    if (hasMeta) {
      // Two lines: station name → stream title → country|bitrate
      display.setCursor(0, NS_STNAME_Y);
      display.print(shortText(stName, SHORT_TEXT_MAX));
      display.setCursor(0, NS_STMETA_Y);
      display.print(shortText(stTitle, SHORT_TEXT_MAX));
      display.setCursor(0, NS_META2_Y);
      display.print(meta);
    } else {
      // Single line: title only → country|bitrate
      display.setCursor(0, NS_TITLE_Y);
      display.print(shortText(stTitle, SHORT_TEXT_MAX));
      display.setCursor(0, NS_META_Y);
      display.print(meta);
    }
  }

  display.drawLine(0, NS_DIVIDER2_Y, 127, NS_DIVIDER2_Y, SH110X_WHITE);
  display.setCursor(0, NS_HINT_Y);
  display.print("Turn: Volume  Press: Mute");
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
    // UI#3: Station row shows current station name instead of static "Select"
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
  display.print("Press: Edit  Hold: Exit");
}

void drawEditScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (focusIndex == F_NOWPLAYING) display.print("SELECT STATION");
  else if (focusIndex == F_COUNTRY) display.print("SELECT COUNTRY");
  else display.print("SELECT GENRE");
  display.drawLine(0, ES_DIVIDER_Y, 127, ES_DIVIDER_Y, SH110X_WHITE);

  if (focusIndex == F_NOWPLAYING) {
    if (stationCount > 0) {
      drawCentered(shortText(stations[previewStation].name, 20), ES_CENTER1_Y);
      // Station index
      drawCentered(String(previewStation + 1) + "/" + String(stationCount), ES_CENTER2_Y);
      // UI#4a: Mini progress bar showing position in station list
      {
        const int barW = 80;
        const int barX = (OLED_WIDTH - barW) / 2;
        const int barH = 3;
        display.drawRect(barX, ES_PROGBAR_Y, barW, barH, SH110X_WHITE);
        int filled = (stationCount > 1)
                     ? (previewStation * (barW - 2)) / (stationCount - 1)
                     : (barW - 2);
        display.fillRect(barX + 1, ES_PROGBAR_Y, filled, barH, SH110X_WHITE);
      }
      drawCentered(String(stations[previewStation].bitrate) + " kbps", ES_CENTER3_Y);
    } else {
      drawCentered("No stations available", ES_COUNTRY_Y);
    }
  } else if (focusIndex == F_COUNTRY) {
    // UI#4b: < value > arrows signal the item is scrollable
    drawCentered("< " + countryName(previewCountry) + " >", ES_COUNTRY_Y, 1);
    drawCentered("Turn to change country", ES_LABEL_Y);
  } else {
    drawCentered("< " + previewGenreName() + " >", ES_COUNTRY_Y, 1);
    drawCentered("Turn to change genre", ES_LABEL_Y);
  }

  display.drawLine(0, ES_DIVIDER2_Y, 127, ES_DIVIDER2_Y, SH110X_WHITE);
  display.setCursor(0, ES_HINT_Y);
  display.print("Turn: Change  Press: OK");
}

void drawUI() {
  // Fix #11: Show loading screen while fetch is in progress
  if (fetchInProgress) {
    drawLoadingScreen();
    return;
  }

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  if (uiMode == MODE_NORMAL) drawNormalScreen();
  else if (uiMode == MODE_BROWSE) drawBrowseScreen();
  else drawEditScreen();

  if (buttonHolding) {
    int progress = min(12, (int)((millis() - holdStartMs) / 60));
    display.drawRect(116, 0, 12, 12, SH110X_WHITE);
    display.fillRect(118, 10 - progress, 8, progress, SH110X_WHITE);
  }

  display.display();
  uiDirty = false;
}

// --------------------------------------------------
// Fix #1: Asynchronous station fetch task
// Runs on Core 0 so audio (Core 1) is never blocked.
// --------------------------------------------------
static String fetchGenreArg;

bool fetchStationsInternal(String tag) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  // Fix #10: STATION_ARRAY_SIZE controls both array size and API limit.
  String url = String(RB_HOST) +
    "/json/stations/search?limit=" + String(STATION_ARRAY_SIZE) +
    "&hidebroken=true&order=clickcount&reverse=true&codec=AAC&bitrateMax=" +
    String(bitrateCap);

  if (tag != "all") url += "&tag=" + tag;
  if (selectedCountry != "all") url += "&countrycode=" + selectedCountry;

  http.begin(url);
  http.setTimeout(12000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Station request failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  // Fix #2: 32 KB buffer — safe with 8 MB psRAM.
  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) {
    Serial.print("JSON error: ");
    Serial.println(error.c_str());
    return false;
  }

  stationCount = 0;
  for (JsonObject s : doc.as<JsonArray>()) {
    String u = s["url_resolved"] | "";
    // ESP32-audioI2S supports HTTP and HTTPS streams; retain either URL.
    if (!u.startsWith("http://") && !u.startsWith("https://")) continue;

    stations[stationCount].name = s["name"] | "Unknown station";
    stations[stationCount].url = u;
    stations[stationCount].bitrate = s["bitrate"] | 0;
    stations[stationCount].country = s["countrycode"] | "";
    stationCount++;
    if (stationCount >= STATION_ARRAY_SIZE) break;
  }

  Serial.printf("Stations loaded: %d\n", stationCount);
  return stationCount > 0;
}

void taskFetch(void* p) {
  bool ok = fetchStationsInternal(fetchGenreArg);
  if (ok) {
    currentStation = 0;
    // Fix #7: Clear stale title before new station connects
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
  fetchTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void applyChange() {
  if (fetchInProgress) return; // Prevent double-fetch
  fetchGenreArg = selectedGenre;
  fetchInProgress = true;
  uiDirty = true;
  xTaskCreatePinnedToCore(taskFetch, "fetch", 8192, nullptr, 1, &fetchTaskHandle, 0);
}

void playStation(int i) {
  if (i < 0 || i >= stationCount) return;
  currentStation = i;
  // Fix #7: Reset streamTitle so stale metadata from the previous station
  //         is never shown; drawNormalScreen() will fall back to station name.
  streamTitle = "";
  audio.stopSong();
  audio.connecttohost(stations[i].url.c_str());
  streamTitle = stations[i].name;
  uiDirty = true;
}

void handleEvent(uint8_t ev) {
  browseLastAction = millis();

  if (uiMode == MODE_NORMAL) {
    if (ev == EV_CW) {
      muted = false;
      audio.setVolume(min(VOL_MAX, audio.getVolume() + 1));
    } else if (ev == EV_CCW) {
      muted = false;
      audio.setVolume(max(0, audio.getVolume() - 1));
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
    if (ev == EV_CW) focusIndex = (focusIndex + 1) % 3;
    else if (ev == EV_CCW) focusIndex = (focusIndex + 2) % 3;
    else if (ev == EV_PRESS) uiMode = MODE_EDIT;
    else if (ev == EV_LONG) uiMode = MODE_NORMAL;
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

// Fix #4: Encoder task with debounce guard on CLK edges
void taskRotary(void* p) {
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);

  int lastClk = digitalRead(ENC_CLK);
  bool lastBtn = HIGH;
  uint32_t pressAt = 0;
  uint32_t lastEncMs = 0; // Debounce timestamp

  for (;;) {
    int clk = digitalRead(ENC_CLK);
    uint32_t now = millis();
    if (clk != lastClk && clk == LOW) {
      // Only accept edge if at least ENC_DEBOUNCE_MS has elapsed
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

void setup() {
  Serial.begin(115200);
  delay(200);

  // Start I2C on the selected XIAO ESP32S3 pins.
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
    Serial.println("Wi-Fi connection timed out. Check secrets.h and restart.");
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
    applyChange(); // Fix #1: initial load is also async
  }

  uiDirty = true;
}

void loop() {
  audio.loop();

  uint8_t event;
  while (encQueue != nullptr && xQueueReceive(encQueue, &event, 0) == pdTRUE) {
    // Ignore encoder input while a fetch is running to avoid state confusion
    if (!fetchInProgress) handleEvent(event);
  }

  // Fix #8: Browse timeout — reset preview state to committed values
  if ((uiMode == MODE_BROWSE || uiMode == MODE_EDIT) &&
      millis() - browseLastAction > 10000) {
    uiMode = MODE_NORMAL;
    previewStation = currentStation;
    previewTag = selectedGenre;
    previewCountry = selectedCountry;
    uiDirty = true;
  }

  // Fix #6: Wi-Fi reconnect with 15 s cool-down
  if (WiFi.status() != WL_CONNECTED) {
    uint32_t now = millis();
    if (now - lastWifiRetryMs > WIFI_RECONNECT_INTERVAL_MS) {
      lastWifiRetryMs = now;
      Serial.println("Wi-Fi lost. Attempting reconnect...");
      WiFi.reconnect();
      // Give the stack 5 s to reconnect before checking
      uint32_t t = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) delay(200);
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Wi-Fi reconnected.");
        applyChange(); // Reload stations after reconnect
      }
    }
  }

  if (uiDirty) drawUI();
  delay(1);
}

// Called by ESP32-audioI2S when stream metadata/title is received.
void audio_showstreamtitle(const char* info) {
  if (info != nullptr && strlen(info) > 0) {
    streamTitle = String(info);
    uiDirty = true;
  }
}

// Optional audio library callbacks for serial diagnostics.
void audio_info(const char* info) {
  Serial.print("Audio info: ");
  Serial.println(info);
}

// Fix #14: audio_eof_mp3 is called by ESP32-audioI2S for any stream end,
// not only MP3. Auto-advance is intentionally not implemented here;
// the user can manually select a new station via the Browse menu.
void audio_eof_mp3(const char* info) {
  Serial.print("Audio stream ended: ");
  Serial.println(info);
  streamTitle = "Stream ended";
  uiDirty = true;
}
