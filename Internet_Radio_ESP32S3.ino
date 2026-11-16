/* -------------------------------------------------
   Internet Radio - ESP32-S3 (ESP32-audioI2S version)

   Based on : abhilashkb/internet-radio

   WIRING (matches the working AudioTools test sketch)
     MAX98357A BCLK -> GPIO4
     MAX98357A LRC  -> GPIO5
     MAX98357A DIN  -> GPIO6
     MAX98357A VIN  -> 5V,  GND -> GND
     MAX98357A SD   -> leave unconnected (or 3V3)

   OPTIONAL (sketch runs fine without them)
     OLED SH1106 SDA -> GPIO8, SCL -> GPIO9
     KY-040 CLK -> GPIO15, DT -> GPIO16, SW -> GPIO17

   ARDUINO IDE SETTINGS
     Board  : ESP32S3 Dev Module
     PSRAM  : OPI PSRAM (N8R8 / N16R8) - recommended
     Serial : 115200 baud

   LIBRARIES
     ESP32-audioI2S (schreibfaul1), ArduinoJson 6.x (NOT 7.x),
     Adafruit GFX, Adafruit SH110X
     (do NOT include AudioTools in this sketch)

   Behaviour
     - If the station list cannot be loaded, BBC World Service
       is played as a fallback so you always get sound.
     - If no OLED is found, the radio still plays (serial log only).
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

// ---------------- Wi-Fi credentials ----------------
#define WIFI_SSID     "Asianet Abilash"
#define WIFI_PASSWORD "1234512345"

// ---------------- Fallback stream ------------------
#define FALLBACK_NAME "BBC World Service"
#define FALLBACK_URL  "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service"

// ---------------- OLED: SH1106 I2C ----------------
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDRESS 0x3C   // Try 0x3D if your module uses that address
#define OLED_SDA     8
#define OLED_SCL     9

// ---------------- I2S audio pins ------------------
#define I2S_BCLK 4   // MAX98357A BCLK
#define I2S_LRCK 5   // MAX98357A LRC / WS
#define I2S_DOUT 6   // MAX98357A DIN

// ---------------- Rotary encoder ------------------
#define ENC_CLK 15
#define ENC_DT  16
#define ENC_SW  17

// ---------------- Radio settings ------------------
#define RB_HOST            "http://de1.api.radio-browser.info"
#define STATION_ARRAY_SIZE 60
#define RB_FETCH_LIMIT     100   // asked from API; some are filtered out (non-http, etc.)
#define RB_CODEC           "AAC"   // "AAC" or "MP3"
#define ALLOW_HTTPS        1
#define VOL_MAX            21
#define INITIAL_VOLUME     18

// ---------------- Wi-Fi reconnect -----------------
#define WIFI_RECONNECT_INTERVAL_MS 15000UL

// ---------------- Layout constants ----------------
#define NS_HEADER_Y   0
#define NS_DIVIDER1_Y 13
#define NS_LABEL_Y    17
#define NS_TITLE_Y    29
#define NS_META_Y     40
#define NS_STNAME_Y   27
#define NS_STMETA_Y   36
#define NS_META2_Y    46
#define NS_DIVIDER2_Y 53
#define NS_HINT_Y     56

#define BS_DIVIDER_Y  12
#define BS_ROW_Y_START 15
#define BS_ROW_STEP   10
#define BS_DIVIDER2_Y 56
#define BS_HINT_Y     58

#define ES_DIVIDER_Y  12
#define ES_CENTER1_Y  20
#define ES_CENTER2_Y  33
#define ES_PROGBAR_Y  42
#define ES_CENTER3_Y  49
#define ES_COUNTRY_Y  28
#define ES_LABEL_Y    43
#define ES_DIVIDER2_Y 56
#define ES_HINT_Y     58

#define VOL_LABEL_X 88
#define VOL_BAR_X   88
#define VOL_BAR_Y   10
#define VOL_BARS    8

#define WIFI_ICON_X  72
#define WIFI_ICON_Y  NS_LABEL_Y
#define WIFI_CHECK_MS 2000UL
#define WIFI_RSSI_4BARS -55
#define WIFI_RSSI_3BARS -65
#define WIFI_RSSI_2BARS -75
#define WIFI_RSSI_1BAR  -85

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
  { "all", "All" }, { "kerala fm", "Kerala FM" },
  { "music", "Music" }, { "news", "News" },
  { "jazz", "Jazz" }, { "classical", "Classical" }, { "rock", "Rock" },
  { "international", "Intl News" }, { "world news", "World" },
  { "technology", "Tech" },{ "meditation", "Mindful" }
};
static const int GENRE_COUNT = sizeof(GENRES) / sizeof(GENRES[0]);

// Language names as used by radio-browser.info (lower case).
// Add or remove lines here to change the list.
struct LanguageEntry {
  const char* tag;
  const char* label;
};

static const LanguageEntry LANGUAGES[] = {
  { "all", "All" }, { "malayalam", "Malayalam" }, { "tamil", "Tamil" },
  { "hindi", "Hindi" }, { "english", "English" }, { "telugu", "Telugu" },
  { "kannada", "Kannada" }
};
static const int LANGUAGE_COUNT = sizeof(LANGUAGES) / sizeof(LANGUAGES[0]);

Audio audio;
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool hasDisplay = false;

QueueHandle_t encQueue = nullptr;

enum EncEvent { EV_CW, EV_CCW, EV_PRESS, EV_LONG };
enum UiMode { MODE_NORMAL, MODE_BROWSE, MODE_EDIT };
enum FocusItem { F_NOWPLAYING, F_COUNTRY, F_TYPE, F_LANG };

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
bool bbcOnly = false;   // true after boot: only BBC World Service is loaded
String selectedLanguage = "malayalam";   // default filter, loaded on first press
String previewLanguage = "malayalam";

int bitrateCap = 128;
bool muted = false;
int lastVol = INITIAL_VOLUME;

uint32_t browseLastAction = 0;
volatile uint32_t holdStartMs = 0;
volatile bool buttonHolding = false;

// Async fetch state
volatile bool fetchInProgress = false;
volatile bool fetchDone = false;
volatile bool fetchOk = false;
TaskHandle_t fetchTaskHandle = nullptr;
static String fetchGenreArg;
static String fetchCountryArg;
static String fetchLanguageArg;

uint32_t lastWifiRetryMs = 0;

// Wi-Fi signal indicator: -1 = not connected, 0..4 = bars
int wifiLevel = -1;
float wifiRssiAvg = 0;
bool wifiRssiValid = false;
uint32_t lastWifiCheckMs = 0;

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

String languageLabel(const String& tag) {
  for (int i = 0; i < LANGUAGE_COUNT; i++) {
    if (tag == LANGUAGES[i].tag) return String(LANGUAGES[i].label);
  }
  return tag;
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

// The built-in OLED font only has English characters. Station names or
// song titles in Malayalam/Tamil/Hindi script would show as garbage symbols,
// so everything that is not plain ASCII is dropped before display.
String asciiOnly(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c >= 32 && c < 127) out += c;
  }
  out.trim();
  return out;
}

// Marquee: text that fits is printed normally; longer text scrolls
// sideways one character at a time. Returns true if it is scrolling,
// so the main loop knows to keep redrawing the screen.
#define SCROLL_CHARS   21     // characters visible on one line
#define SCROLL_STEP_MS 300UL  // scroll speed (lower = faster)
bool scrollActive = false;
uint32_t lastScrollMs = 0;

bool drawScrollText(const String& text, int y, int maxChars = SCROLL_CHARS) {
  display.setCursor(0, y);
  if ((int)text.length() <= maxChars) {
    display.print(text);
    return false;
  }
  String padded = text + "   ";
  int len = padded.length();
  int offset = (millis() / SCROLL_STEP_MS) % len;
  String view;
  view.reserve(maxChars);
  for (int i = 0; i < maxChars; i++) view += padded[(offset + i) % len];
  display.print(view);
  return true;
}

// Puts the fallback stream into slot 0 so there is always something to play.
void loadFallbackStation() {
  stations[0].name = FALLBACK_NAME;
  stations[0].url = FALLBACK_URL;
  stations[0].bitrate = 128;
  stations[0].country = "GB";
  stationCount = 1;
  currentStation = 0;
}

// --------------------------------------------------
// Drawing
// --------------------------------------------------

// Speaker icon + horizontal level bar, drawn only inside the header row
// (y = 0..11) so it never touches the divider or the "NOW PLAYING" row.
void drawVolume() {
  int vol = audio.getVolume();

  const int sx = 88;   // speaker left edge
  const int sy = 1;    // speaker top edge

  // speaker body + cone
  display.fillRect(sx, sy + 3, 3, 3, SH110X_WHITE);
  display.fillTriangle(sx + 2, sy + 4, sx + 8, sy, sx + 8, sy + 8, SH110X_WHITE);

  // level bar
  const int bx = 100;  // bar left edge
  const int by = 2;    // bar top edge
  const int bw = 27;   // bar total width
  const int bh = 7;    // bar height
  display.drawRect(bx, by, bw, bh, SH110X_WHITE);
  int filled = (vol * (bw - 4)) / VOL_MAX;
  if (filled > 0) display.fillRect(bx + 2, by + 2, filled, bh - 4, SH110X_WHITE);

  // volume 0: strike through the speaker
  if (vol == 0) display.drawLine(sx, sy + 9, sx + 9, sy - 1, SH110X_WHITE);
}

void drawLoadingScreen() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  drawCentered("MUTHU'S RADIO", 10);
  display.drawLine(0, NS_DIVIDER1_Y, 127, NS_DIVIDER1_Y, SH110X_WHITE);
  drawCentered("Loading stations...", 28);
  drawCentered("Please wait", 42);
  display.display();
}

void drawWifiIcon() {
  const int base = WIFI_ICON_Y + 6;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    int x = WIFI_ICON_X + i * 4;
    if (i < wifiLevel) {
      display.fillRect(x, base - h + 1, 3, h, SH110X_WHITE);
    } else {
      display.drawFastHLine(x, base, 3, SH110X_WHITE);
    }
  }
  if (wifiLevel < 0) {
    int x0 = WIFI_ICON_X + 18;
    display.drawLine(x0, base - 5, x0 + 5, base, SH110X_WHITE);
    display.drawLine(x0, base, x0 + 5, base - 5, SH110X_WHITE);
  }
}

void updateWifiLevel() {
  uint32_t now = millis();
  if (now - lastWifiCheckMs < WIFI_CHECK_MS) return;
  lastWifiCheckMs = now;

  int newLevel;
  if (WiFi.status() != WL_CONNECTED) {
    wifiRssiValid = false;
    newLevel = -1;
  } else {
    int r = WiFi.RSSI();
    wifiRssiAvg = wifiRssiValid ? (wifiRssiAvg * 0.7f + r * 0.3f) : (float)r;
    wifiRssiValid = true;
    if (wifiRssiAvg >= WIFI_RSSI_4BARS) newLevel = 4;
    else if (wifiRssiAvg >= WIFI_RSSI_3BARS) newLevel = 3;
    else if (wifiRssiAvg >= WIFI_RSSI_2BARS) newLevel = 2;
    else if (wifiRssiAvg >= WIFI_RSSI_1BAR) newLevel = 1;
    else newLevel = 0;
  }

  if (newLevel != wifiLevel) {
    wifiLevel = newLevel;
    if (uiMode == MODE_NORMAL && !fetchInProgress) uiDirty = true;
    Serial.printf("Wi-Fi signal: %d bar(s)", wifiLevel);
    if (wifiRssiValid) Serial.printf(" (%d dBm)", (int)wifiRssiAvg);
    Serial.println();
  }
}

void drawNormalScreen() {
  display.setTextSize(1);
  display.setCursor(0, NS_HEADER_Y);
  display.print("MUTHU'S RADIO");
  drawVolume();
  display.drawLine(0, NS_DIVIDER1_Y, 127, NS_DIVIDER1_Y, SH110X_WHITE);

  display.setCursor(0, NS_LABEL_Y);
  display.print("NOW PLAYING");
  drawWifiIcon();

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
    scrollActive = false;
    if (hasMeta) {
      // station name (static) + song title (scrolls if too long)
      display.setCursor(0, NS_STNAME_Y);
      display.print(shortText(stName, SCROLL_CHARS));
      if (drawScrollText(stTitle, NS_STMETA_Y)) scrollActive = true;
      display.setCursor(0, NS_META2_Y);
      display.print(meta);
    } else {
      // only the station name is known: scroll it if it is long
      if (drawScrollText(stTitle, NS_TITLE_Y)) scrollActive = true;
      display.setCursor(0, NS_META_Y);
      display.print(meta);
    }
  }

  display.drawLine(0, NS_DIVIDER2_Y, 127, NS_DIVIDER2_Y, SH110X_WHITE);
  display.setCursor(0, NS_HINT_Y);
  display.print("Tap:Next Hold:Menu");
}

void drawBrowseScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("BROWSE MENU");
  display.drawLine(0, BS_DIVIDER_Y, 127, BS_DIVIDER_Y, SH110X_WHITE);

  const char* labels[] = { "Station", "Country", "Genre", "Language" };
  for (int i = 0; i < 4; i++) {
    int y = BS_ROW_Y_START + i * BS_ROW_STEP;
    if (i == focusIndex) {
      display.fillRect(0, y - 1, 128, 10, SH110X_WHITE);
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
    } else if (i == F_TYPE) {
      display.print(shortText(typeName(), 9));
    } else {
      display.print(shortText(languageLabel(selectedLanguage), 9));
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
  if (focusIndex == F_NOWPLAYING) display.print("SELECT STATION");
  else if (focusIndex == F_COUNTRY) display.print("SELECT COUNTRY");
  else if (focusIndex == F_TYPE) display.print("SELECT GENRE");
  else display.print("SELECT LANGUAGE");
  display.drawLine(0, ES_DIVIDER_Y, 127, ES_DIVIDER_Y, SH110X_WHITE);

  if (focusIndex == F_NOWPLAYING) {
    if (stationCount > 0) {
      // position counter in the header, e.g. "3/40"
      String cnt = String(previewStation + 1) + "/" + String(stationCount);
      display.setCursor(OLED_WIDTH - (int)cnt.length() * 6, 0);
      display.print(cnt);

      // scrolling list: 4 rows visible, selected station highlighted
      const int rows = 4;
      int maxTop = stationCount - rows;
      if (maxTop < 0) maxTop = 0;
      int top = previewStation - 1;      // keep one station visible above the selection
      if (top > maxTop) top = maxTop;
      if (top < 0) top = 0;

      for (int r = 0; r < rows && (top + r) < stationCount; r++) {
        int idx = top + r;
        int y = BS_ROW_Y_START + r * BS_ROW_STEP;
        if (idx == previewStation) {
          display.fillRect(0, y - 1, 122, 10, SH110X_WHITE);
          display.setTextColor(SH110X_BLACK);
        } else {
          display.setTextColor(SH110X_WHITE);
        }
        display.setCursor(2, y);
        display.print(shortText(stations[idx].name, 19));
      }
      display.setTextColor(SH110X_WHITE);

      // scrollbar on the right edge (only when the list is longer than the screen)
      if (stationCount > rows) {
        const int trackY = 14, trackH = 40;
        display.drawFastVLine(126, trackY, trackH, SH110X_WHITE);
        int thumbH = (trackH * rows) / stationCount;
        if (thumbH < 4) thumbH = 4;
        int thumbY = trackY + ((trackH - thumbH) * previewStation) / (stationCount - 1);
        display.fillRect(125, thumbY, 3, thumbH, SH110X_WHITE);
      }
    } else {
      drawCentered("No stations available", ES_COUNTRY_Y);
    }
  } else if (focusIndex == F_COUNTRY) {
    drawCentered("< " + countryName(previewCountry) + " >", ES_COUNTRY_Y, 1);
    drawCentered("Turn to pick country", ES_LABEL_Y);
  } else if (focusIndex == F_TYPE) {
    drawCentered("< " + previewGenreName() + " >", ES_COUNTRY_Y, 1);
    drawCentered("Turn to pick genre", ES_LABEL_Y);
  } else {
    drawCentered("< " + languageLabel(previewLanguage) + " >", ES_COUNTRY_Y, 1);
    drawCentered("Turn to pick language", ES_LABEL_Y);
  }

  display.drawLine(0, ES_DIVIDER2_Y, 127, ES_DIVIDER2_Y, SH110X_WHITE);
  display.setCursor(0, ES_HINT_Y);
  display.print("Turn:Change Press:OK");
}

void drawUI() {
  if (!hasDisplay) {          // no OLED attached: nothing to draw
    uiDirty = false;
    return;
  }
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
// Station fetch (runs on core 0 in a short-lived task)
// The task ONLY downloads and parses. All audio.* calls are made
// from loop(), because the audio library is not thread-safe.
// --------------------------------------------------

// Kerala FM: runs several station-name searches (Rainbow, Mango, Club FM,
// Akashvani / AIR, city names ...) and merges the results into one list
// without duplicates. Edit the keyword list to add or remove searches.
static const char* KERALA_KEYWORDS[] = {
  "mango", "rainbow", "club fm", "akashvani", "all india radio",
  "kochi", "kozhikode", "thrissur", "trivandrum", "kannur",
  "red fm", "big fm", "kerala", "malayalam"
};
static const int KERALA_KEYWORD_COUNT = sizeof(KERALA_KEYWORDS) / sizeof(KERALA_KEYWORDS[0]);
#define KERALA_PER_KEYWORD 6   // max stations taken from each keyword search

bool fetchKeralaFM() {
  if (WiFi.status() != WL_CONNECTED) return false;

  stationCount = 0;
  for (int k = 0; k < KERALA_KEYWORD_COUNT && stationCount < STATION_ARRAY_SIZE; k++) {
    String kw = KERALA_KEYWORDS[k];
    kw.replace(" ", "%20");
    String url = String(RB_HOST) + "/json/stations/search?limit=" + String(KERALA_PER_KEYWORD) +
                 "&hidebroken=true&order=votes&reverse=true&countrycode=IN&bitrateMax=192&name=" + kw;

    HTTPClient http;
    http.begin(url);
    http.useHTTP10(true);
    http.setTimeout(8000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      Serial.printf("Kerala FM '%s': HTTP %d\n", KERALA_KEYWORDS[k], code);
      http.end();
      continue;
    }

    StaticJsonDocument<192> filter;
    filter[0]["name"] = true;
    filter[0]["url_resolved"] = true;
    filter[0]["bitrate"] = true;
    filter[0]["countrycode"] = true;

    DynamicJsonDocument doc(8192);
    DeserializationError error =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (error) {
      Serial.printf("Kerala FM '%s': JSON error %s\n", KERALA_KEYWORDS[k], error.c_str());
      continue;
    }

    for (JsonObject s : doc.as<JsonArray>()) {
      String u = s["url_resolved"] | "";
      bool okScheme = u.startsWith("http://");
#if ALLOW_HTTPS
      okScheme = okScheme || u.startsWith("https://");
#endif
      if (!okScheme) continue;

      bool dup = false;                      // skip streams already in the list
      for (int i = 0; i < stationCount; i++) {
        if (stations[i].url == u) { dup = true; break; }
      }
      if (dup) continue;

      String nm = asciiOnly(String(s["name"] | ""));
      if (nm.length() == 0) nm = "Station " + String(stationCount + 1);
      stations[stationCount].name = nm;
      stations[stationCount].url = u;
      stations[stationCount].bitrate = s["bitrate"] | 0;
      stations[stationCount].country = s["countrycode"] | "";
      stationCount++;
      if (stationCount >= STATION_ARRAY_SIZE) break;
    }
    Serial.printf("Kerala FM '%s': %d stations so far\n", KERALA_KEYWORDS[k], stationCount);
  }
  return stationCount > 0;
}

bool fetchStationsInternal(const String& tag, const String& country, const String& language) {
  if (tag == "kerala fm") return fetchKeralaFM();
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = String(RB_HOST) +
               "/json/stations/search?limit=" + String(RB_FETCH_LIMIT) +
               "&hidebroken=true&order=clickcount&reverse=true"
               "&bitrateMax=" + String(bitrateCap);
  // Regional-language stations are often MP3, so the AAC-only filter
  // is applied only when no language is chosen. The audio library plays both.
  // The AAC-only filter is used only for plain "all stations" browsing.
  // Language, news and tech searches also accept MP3 so more stations match.
  if (language == "all" && tag == "all") url += "&codec=" RB_CODEC;
  if (language != "all") url += "&language=" + language;
  if (tag != "all") {
    String t = tag;
    t.replace(" ", "%20");           // tags such as "world news" contain a space
    url += "&tag=" + t;
  }
  if (country != "all") url += "&countrycode=" + country;

  http.begin(url);
  http.useHTTP10(true);
  http.setTimeout(12000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Station request failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  StaticJsonDocument<192> filter;
  filter[0]["name"] = true;
  filter[0]["url_resolved"] = true;
  filter[0]["bitrate"] = true;
  filter[0]["countrycode"] = true;

  // Smaller buffer when there is no PSRAM
  DynamicJsonDocument doc(psramFound() ? 49152 : 24576);
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

    String nm = asciiOnly(String(s["name"] | ""));
    if (nm.length() == 0) nm = "Station " + String(stationCount + 1);
    stations[stationCount].name = nm;
    stations[stationCount].url = u;
    stations[stationCount].bitrate = s["bitrate"] | 0;
    stations[stationCount].country = s["countrycode"] | "";
    stationCount++;
    if (stationCount >= STATION_ARRAY_SIZE) break;
  }

  // For international / world news, always offer BBC World Service first,
  // even if the directory has not tagged it with this genre.
  if (tag == "international" || tag == "world news") {
    bool hasBBC = false;
    for (int i = 0; i < stationCount; i++) {
      if (stations[i].url == FALLBACK_URL) { hasBBC = true; break; }
    }
    if (!hasBBC) {
      int n = (stationCount < STATION_ARRAY_SIZE - 1) ? stationCount : STATION_ARRAY_SIZE - 1;
      for (int i = n; i > 0; i--) stations[i] = stations[i - 1];
      stations[0].name = FALLBACK_NAME;
      stations[0].url = FALLBACK_URL;
      stations[0].bitrate = 128;
      stations[0].country = "GB";
      stationCount = n + 1;
    }
  }

  Serial.printf("Stations loaded: %d (heap %u, PSRAM %u free)\n",
                stationCount, ESP.getFreeHeap(), ESP.getFreePsram());
  return stationCount > 0;
}

void taskFetch(void* p) {
  fetchOk = fetchStationsInternal(fetchGenreArg, fetchCountryArg, fetchLanguageArg);
  fetchDone = true;
  fetchTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void applyChange() {
  if (fetchInProgress) return;
  bbcOnly = false;
  fetchGenreArg = selectedGenre;
  fetchCountryArg = selectedCountry;
  fetchLanguageArg = selectedLanguage;
  fetchDone = false;
  fetchInProgress = true;
  uiDirty = true;
  xTaskCreatePinnedToCore(taskFetch, "fetch", 10240, nullptr, 1, &fetchTaskHandle, 0);
}

// Called from loop() once the fetch task has finished.
void finishFetch() {
  audio.stopSong();
  if (fetchOk) {
    currentStation = 0;
  } else {
    Serial.println("Station list unavailable - playing fallback stream.");
    loadFallbackStation();
  }
  streamTitle = "";
  audio.connecttohost(stations[0].url.c_str());
  streamTitle = stations[0].name;
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
      // First press after boot (or if the list is empty): load the default
      // filter (Malayalam). After that, short press = next station.
      if (bbcOnly || stationCount <= 1) {
        applyChange();
      } else {
        playStation((currentStation + 1) % stationCount);
      }
    } else if (ev == EV_LONG) {
      uiMode = MODE_BROWSE;
      previewStation = currentStation;
      previewTag = selectedGenre;
      previewCountry = selectedCountry;
      previewLanguage = selectedLanguage;
    }
  } else if (uiMode == MODE_BROWSE) {
    if (ev == EV_CW) focusIndex = (focusIndex + 1) % 4;
    else if (ev == EV_CCW) focusIndex = (focusIndex + 3) % 4;
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
        // International / tech streams are mostly English, so drop the
        // Malayalam language and country filters or nothing would match.
        if (selectedGenre == "international" || selectedGenre == "world news" ||
            selectedGenre == "technology" || selectedGenre == "kerala fm") {
          selectedLanguage = "all";
          selectedCountry = "all";
          previewLanguage = "all";
          previewCountry = "all";
        }
        applyChange();
        uiMode = MODE_BROWSE;
      } else if (ev == EV_LONG) {
        uiMode = MODE_BROWSE;
      }
    } else if (focusIndex == F_LANG) {
      if (ev == EV_CW || ev == EV_CCW) {
        int li = 0;
        for (int i = 0; i < LANGUAGE_COUNT; i++) {
          if (previewLanguage == LANGUAGES[i].tag) { li = i; break; }
        }
        int dir = (ev == EV_CW) ? 1 : -1;
        li = (li + dir + LANGUAGE_COUNT) % LANGUAGE_COUNT;
        previewLanguage = LANGUAGES[li].tag;
      } else if (ev == EV_PRESS) {
        selectedLanguage = previewLanguage;
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
  Serial.begin(115200);
  delay(500);

  Serial.printf("Free heap at boot: %u PSRAM: %u of %u bytes free\n",
                ESP.getFreeHeap(), ESP.getFreePsram(), ESP.getPsramSize());

  if (!psramFound()) {
    Serial.println("Warning: PSRAM not found. Set Tools > PSRAM > OPI PSRAM for best results.");
  }

  // OLED is optional: continue without it if it is not found
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  hasDisplay = display.begin(OLED_ADDRESS, true);
  if (hasDisplay) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    drawCentered("MUTHU'S RADIO", 18);
    drawCentered("Starting...", 34);
    display.display();
  } else {
    Serial.println("OLED not found - continuing without display.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // steadier streaming
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (hasDisplay) {
    display.clearDisplay();
    drawCentered("Connecting Wi-Fi", 25);
    display.display();
  }

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
    Serial.println("Wi-Fi connection timed out. Check WIFI_SSID / WIFI_PASSWORD (2.4 GHz only).");
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

  // Boot: play BBC World Service. The first encoder press loads the
  // default filter (Malayalam stations); later presses go to the next station.
  loadFallbackStation();
  bbcOnly = true;
  if (WiFi.status() == WL_CONNECTED) {
    streamTitle = FALLBACK_NAME;
    audio.connecttohost(FALLBACK_URL);
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
    if (!fetchInProgress) handleEvent(event);
  }

  // Browse/edit timeout - reset preview state to committed values
  if ((uiMode == MODE_BROWSE || uiMode == MODE_EDIT) &&
      millis() - browseLastAction > 10000) {
    uiMode = MODE_NORMAL;
    previewStation = currentStation;
    previewTag = selectedGenre;
    previewCountry = selectedCountry;
    previewLanguage = selectedLanguage;
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
        if (bbcOnly) playStation(0);   // resume BBC World Service
        else applyChange();            // reload the current filter
      }
    }
  }

  updateWifiLevel();

  // Keep the marquee moving while long text is scrolling
  if (hasDisplay && scrollActive && uiMode == MODE_NORMAL && !fetchInProgress &&
      millis() - lastScrollMs >= SCROLL_STEP_MS) {
    lastScrollMs = millis();
    uiDirty = true;
  }

  if (uiDirty) drawUI();

  delay(1);
}

// --------------------------------------------------
// ESP32-audioI2S callbacks
// --------------------------------------------------

void audio_showstreamtitle(const char* info) {
  if (info != nullptr && strlen(info) > 0) {
    String t = asciiOnly(String(info));
    if (t.length() > 0) {          // keep the old title if nothing printable is left
      streamTitle = t;
      uiDirty = true;
    }
  }
}

void audio_info(const char* info) {
  Serial.print("Audio info: ");
  Serial.println(info);
}

// Called for any stream end. No auto-advance by design.
void audio_eof_mp3(const char* info) {
  Serial.print("Audio stream ended: ");
  Serial.println(info);
  streamTitle = "Stream ended";
  uiDirty = true;
}
