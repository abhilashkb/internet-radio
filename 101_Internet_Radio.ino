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
#define OLED_SDA     5           // XIAO D4
#define OLED_SCL     6           // XIAO D5

// ---------------- I2S audio pins -------------------
#define I2S_DOUT 7               // XIAO D8
#define I2S_BCLK 8               // XIAO D9
#define I2S_LRCK 9               // XIAO D10

// ---------------- Rotary encoder ------------------
#define ENC_CLK 43               // XIAO D6
#define ENC_DT  44               // XIAO D7
#define ENC_SW  4                // XIAO D3 (moved from GPIO6; GPIO6 is OLED SCL)

// ---------------- Radio settings ------------------
#define RB_HOST "http://de1.api.radio-browser.info"
#define RB_RESULT_LIMIT 40
#define VOL_MAX 21
#define INITIAL_VOLUME 18

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

Station stations[RB_RESULT_LIMIT];
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

// --------------------------------------------------
// Radio Browser station retrieval
// --------------------------------------------------
bool fetchStations(String tag) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = String(RB_HOST) +
    "/json/stations/search?limit=20&hidebroken=true&order=clickcount&reverse=true&codec=AAC&bitrateMax=" +
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

  DynamicJsonDocument doc(16000);
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
    if (stationCount >= RB_RESULT_LIMIT) break;
  }

  Serial.printf("Stations loaded: %d\n", stationCount);
  return stationCount > 0;
}

void playStation(int i) {
  if (i < 0 || i >= stationCount) return;

  currentStation = i;
  streamTitle = stations[i].name;
  audio.stopSong();
  audio.connecttohost(stations[i].url.c_str());
  uiDirty = true;
}

String countryName(String c) {
  c.toUpperCase();
  if (c == "US") return "USA";
  if (c == "GB") return "UK";
  if (c == "IN") return "India";
  if (c == "DE") return "Germany";
  if (c == "FR") return "France";
  if (c == "JP") return "Japan";
  if (c == "CA") return "Canada";
  if (c == "AU") return "Australia";
  if (c == "IT") return "Italy";
  if (c == "ES") return "Spain";
  if (c == "BR") return "Brazil";
  if (c == "MX") return "Mexico";
  if (c == "NL") return "Netherlands";
  if (c == "SE") return "Sweden";
  if (c == "NO") return "Norway";
  if (c == "ZA") return "South Africa";
  if (c == "SG") return "Singapore";
  if (c == "AE") return "UAE";
  if (c == "ALL" || c.length() == 0) return "All";
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

String shortText(String value, uint8_t maxChars) {
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

void drawVolume() {
  int vol = audio.getVolume();
  display.setTextSize(1);
  display.setCursor(88, 0);
  display.print(muted ? "MUTE" : "VOL");
  int barX = 88;
  int barY = 10;
  for (int i = 0; i < 8; i++) {
    int h = 3 + (i % 3) * 2;
    int x = barX + i * 5;
    int y = barY + (10 - h);
    if (!muted && i < (vol * 8 / VOL_MAX)) {
      display.fillRect(x, y, 3, h, SH110X_WHITE);
    } else {
      display.drawRect(x, y, 3, h, SH110X_WHITE);
    }
  }
}

void drawNormalScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("INTERNET RADIO");
  drawVolume();
  display.drawLine(0, 13, 127, 13, SH110X_WHITE);

  display.setCursor(0, 17);
  display.print("NOW PLAYING");

  if (stationCount == 0) {
    drawCentered("No stations loaded", 31);
    drawCentered("Check Wi-Fi / API", 43);
  } else {
    String title = streamTitle;
    if (title.length() == 0) title = stations[currentStation].name;
    display.setTextSize(1);
    display.setCursor(0, 29);
    display.println(shortText(title, 21));
    display.setCursor(0, 40);
    String country = countryName(stations[currentStation].country);
    display.print(shortText(country, 12));
    display.print(" | ");
    display.print(stations[currentStation].bitrate);
    display.print(" kbps");
  }

  display.drawLine(0, 53, 127, 53, SH110X_WHITE);
  display.setCursor(0, 56);
  display.print("Turn: Volume  Press: Mute");
}

void drawBrowseScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("BROWSE MENU");
  display.drawLine(0, 12, 127, 12, SH110X_WHITE);

  const char* labels[] = { "Station", "Country", "Genre" };
  for (int i = 0; i < 3; i++) {
    int y = 17 + i * 13;
    if (i == focusIndex) {
      display.fillRect(0, y - 1, 128, 11, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    } else {
      display.setTextColor(SH110X_WHITE);
    }
    display.setCursor(3, y);
    display.print(labels[i]);
    display.setCursor(76, y);
    if (i == F_NOWPLAYING) display.print(stationCount > 0 ? "Select" : "Empty");
    else if (i == F_COUNTRY) display.print(shortText(countryName(selectedCountry), 8));
    else display.print(shortText(typeName(), 8));
  }
  display.setTextColor(SH110X_WHITE);
  display.drawLine(0, 56, 127, 56, SH110X_WHITE);
  display.setCursor(0, 58);
  display.print("Press: Edit  Hold: Exit");
}

void drawEditScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (focusIndex == F_NOWPLAYING) display.print("SELECT STATION");
  else if (focusIndex == F_COUNTRY) display.print("SELECT COUNTRY");
  else display.print("SELECT GENRE");
  display.drawLine(0, 12, 127, 12, SH110X_WHITE);

  if (focusIndex == F_NOWPLAYING) {
    if (stationCount > 0) {
      drawCentered(shortText(stations[previewStation].name, 20), 24);
      drawCentered(String(previewStation + 1) + "/" + String(stationCount), 39);
      drawCentered(String(stations[previewStation].bitrate) + " kbps", 49);
    } else {
      drawCentered("No stations available", 30);
    }
  } else if (focusIndex == F_COUNTRY) {
    drawCentered(countryName(previewCountry), 30, 1);
    drawCentered("Choose country", 43);
  } else {
    drawCentered(previewGenreName(), 30, 1);
    drawCentered("Choose genre", 43);
  }

  display.drawLine(0, 56, 127, 56, SH110X_WHITE);
  display.setCursor(0, 58);
  display.print("Turn: Change  Press: OK");
}

void drawUI() {
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

void applyChange() {
  if (fetchStations(selectedGenre)) {
    playStation(0);
  } else {
    stationCount = 0;
    currentStation = 0;
    streamTitle = "No stations found";
    audio.stopSong();
  }
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

void taskRotary(void* p) {
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);

  int lastClk = digitalRead(ENC_CLK);
  bool lastBtn = HIGH;
  uint32_t pressAt = 0;

  for (;;) {
    int clk = digitalRead(ENC_CLK);
    if (clk != lastClk && clk == LOW) {
      uint8_t event = digitalRead(ENC_DT) ? EV_CW : EV_CCW;
      xQueueSend(encQueue, &event, 0);
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
    if (fetchStations(selectedGenre)) {
      playStation(0);
    } else {
      streamTitle = "No stations found";
    }
  }

  uiDirty = true;
}

void loop() {
  audio.loop();

  uint8_t event;
  while (encQueue != nullptr && xQueueReceive(encQueue, &event, 0) == pdTRUE) {
    handleEvent(event);
  }

  if ((uiMode == MODE_BROWSE || uiMode == MODE_EDIT) &&
      millis() - browseLastAction > 10000) {
    uiMode = MODE_NORMAL;
    uiDirty = true;
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

void audio_eof_mp3(const char* info) {
  Serial.print("Audio stream ended: ");
  Serial.println(info);
}
