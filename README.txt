Internet Radio V2 - SH1106 OLED adaptation

Files:
- 101_Internet_Radio.ino
- secrets.h

Hardware:
- Seeed XIAO ESP32S3
- 1.3 inch SH1106 128x64 I2C OLED
- MAX98357A I2S amplifier
- 8 ohm speaker
- KY-040 rotary encoder

Libraries to install:
- ESP32-audioI2S (Audio.h)
- ArduinoJson
- Adafruit GFX Library
- Adafruit SH110X

Arduino IDE:
1. Keep 101_Internet_Radio.ino and secrets.h in the same sketch folder.
2. Open 101_Internet_Radio.ino.
3. Edit WIFI_SSID and WIFI_PASSWORD in secrets.h.
4. Select the Seeed XIAO ESP32S3 board and the correct USB port.
5. Compile and upload.

Pin map:
OLED: VCC=3V3, GND=GND, SDA=D4/GPIO5, SCL=D5/GPIO6
Encoder: CLK=D6/GPIO43, DT=D7/GPIO44, SW=D3/GPIO4, +=3V3, GND=GND
MAX98357A: DIN=D8/GPIO7, BCLK=D9/GPIO8, LRC=D10/GPIO9, VIN=5V/VBUS, GND=GND
Speaker: connect only between MAX98357A SPK+ and SPK-. Do not connect either speaker lead to GND.

Notes:
- If OLED is not detected, try OLED_ADDRESS 0x3D instead of 0x3C.
- The code uses HTTP and HTTPS station stream URLs returned by Radio Browser.
- This adaptation has not been compiled against your exact installed Arduino core/library versions or tested on physical hardware. Library API differences may require minor adjustments.
