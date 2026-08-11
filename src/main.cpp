/*
  Weather Station
  ---------------------------------
  Reads temperature, humidity, pressure, UV and light from sensors
  and shows the data on the little TFT screen, with two display
  modes: a friendly icon+text view and a raw numbers view.

  Board: ESP32-S3 Reverse TFT Feather
*/

#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_LTR390.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// ============================================================
//  PIN + HARDWARE CONSTANTS
// ============================================================

#define TFT_BACKLITE 45
#define BUTTON_D0    0
#define TFT_DARKGREY 0x4208

// ============================================================
//  WEATHER THRESHOLD CONSTANTS
//  Named instead of hardcoded so the logic in getWeather() is
//  actually readable, and so I can tune the numbers in one place.
// ============================================================

const float HOT_TEMP_C     = 28.0;
const float HOT_UVI        = 6.0;
const float HOT_LUX        = 1000.0;

const float WARM_TEMP_C    = 22.0;
const float WARM_UVI       = 3.0;
const float WARM_LUX       = 500.0;

const float MILD_TEMP_C    = 16.0;
const float MILD_LUX       = 300.0;

const float COLD_TEMP_C    = 10.0;
const float DARK_LUX       = 100.0;

const float DULL_LUX       = 200.0;

const float HUMID_PERCENT  = 75.0;

const float COOL_TEMP_C    = 16.0;
const float BRIGHT_LUX     = 200.0;

// ============================================================
//  SENSOR VALID RANGE CONSTANTS
//  Used to catch obviously broken readings (boundary/invalid
//  input handling) instead of trusting every number the sensor
//  gives us.
// ============================================================

const float MIN_VALID_TEMP_C = -40.0;
const float MAX_VALID_TEMP_C = 85.0;
const float MIN_VALID_HUM    = 0.0;
const float MAX_VALID_HUM    = 100.0;
const float MIN_VALID_PRES   = 300.0;   // hPa, well below sea level pressure
const float MAX_VALID_PRES   = 1100.0;  // hPa, well above sea level pressure

const unsigned long BUTTON_DEBOUNCE_MS = 300;
const unsigned long LOOP_DELAY_MS      = 2000;
const unsigned long SENSOR_RETRY_MS    = 500;
const int            SENSOR_MAX_TRIES  = 5;

// ============================================================
//  OBJECTS
// ============================================================

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

Adafruit_AHTX0  aht;
Adafruit_BMP280 bmp;
Adafruit_LTR390 ltr;

// ============================================================
//  STATE VARIABLES
// ============================================================

bool hardDataMode = false;   // false = friendly screen, true = raw numbers screen

// Track whether each sensor started up ok. If a sensor failed we
// don't want to freeze the whole board forever - the rest of the
// project should keep working.
bool ahtOK = false;
bool bmpOK = false;
bool ltrOK = false;

// Keep the last good reading for each value. If a new reading looks
// invalid (NAN, or out of realistic range) we fall back to this
// instead of showing garbage on the screen.
float lastTemp = 0;
float lastHum  = 0;
float lastPres = 0;
float lastUvi  = 0;
float lastLux  = 0;

unsigned long lastButtonPress = 0;

// ============================================================
//  SENSOR READING FUNCTIONS
// ============================================================

// Reads both temperature and humidity from the AHT20 in a single
// sensor call and hands the results back through the two pointer
// parameters. The AHT20 gives both values from one getEvent() call,
// so reading them separately would mean polling the same sensor
// twice every loop for no reason. Passing NAN for both if the
// sensor isn't working keeps the "invalid reading" handling
// consistent with the other sensors.
void readAHT(float &tempOut, float &humOut) {
  if (!ahtOK) {
    tempOut = NAN;
    humOut  = NAN;
    return;
  }
  sensors_event_t humidityEvent, tempEvent;
  aht.getEvent(&humidityEvent, &tempEvent);
  tempOut = tempEvent.temperature;
  humOut  = humidityEvent.relative_humidity;
}

// Reads pressure (in hPa) from the BMP280. Returns NAN if the
// sensor isn't working.
float readPressure() {
  if (!bmpOK) return NAN;
  return bmp.readPressure() / 100.0F;
}

// Reads UV index from the LTR390. Returns NAN if the sensor isn't
// working, and returns the last known value if the sensor is
// working but doesn't have new data ready yet.
float readUVI() {
  if (!ltrOK) return NAN;
  ltr.setMode(LTR390_MODE_UVS);
  delay(120);
  if (!ltr.newDataAvailable()) return lastUvi; // keep old value, don't guess
  return (float)ltr.readUVS() / 2300.0F;
}

// Reads light level (lux) from the LTR390. Same idea as readUVI().
float readLux() {
  if (!ltrOK) return NAN;
  ltr.setMode(LTR390_MODE_ALS);
  delay(120);
  if (!ltr.newDataAvailable()) return lastLux;
  return (0.6F * ltr.readALS()) / 3.0F;
}

// Checks whether a reading is a real, sensible number for the type
// of value it is. Handles both "invalid" (NAN, sensor failed) and
// "boundary" (technically a number, but outside what the sensor
// could realistically report) cases.
bool isValidReading(float value, float minAllowed, float maxAllowed) {
  if (isnan(value)) return false;
  if (value < minAllowed || value > maxAllowed) return false;
  return true;
}

// ============================================================
//  SHAPE ICONS FOR THE FRIENDLY WEATHER SCREEN
//  Icons are drawn smaller than before to leave more room for
//  the text next to them so it doesn't run off the screen.
// ============================================================

// Draws a simple sun shape (circle with rays) at x, y.
void drawSun(int x, int y) {
  tft.fillCircle(x, y, 10, ST77XX_YELLOW);
  for (int i = 0; i < 8; i++) {
    float angle = i * 3.14159 / 4;
    int x1 = x + cos(angle) * 13;
    int y1 = y + sin(angle) * 13;
    int x2 = x + cos(angle) * 17;
    int y2 = y + sin(angle) * 17;
    tft.drawLine(x1, y1, x2, y2, ST77XX_YELLOW);
  }
}

// Draws a simple cloud shape made from a few overlapping circles.
void drawCloud(int x, int y) {
  uint16_t color = 0xC618;
  tft.fillCircle(x,      y,     7, color);
  tft.fillCircle(x + 8,  y - 3, 9, color);
  tft.fillCircle(x + 18, y,     7, color);
  tft.fillRect(x - 1, y, 28, 8, color);
}

// Draws a small sun peeking out from behind a cloud, built by
// reusing drawSun() and drawCloud() rather than duplicating code.
void drawSunCloud(int x, int y) {
  drawSun(x + 6, y + 6);
  drawCloud(x + 7, y + 13);
}

// Draws a cloud with rain lines underneath it. Reuses drawCloud().
void drawRain(int x, int y) {
  drawCloud(x, y);
  for (int i = 0; i < 4; i++) {
    tft.drawLine(x + 3 + i * 7, y + 15, x + 1 + i * 7, y + 21, ST77XX_BLUE);
  }
}

// Draws a simple "hot face" icon for very hot days.
void drawHotFace(int x, int y) {
  tft.fillCircle(x, y, 13, ST77XX_RED);
  tft.fillCircle(x - 4, y - 4, 2, ST77XX_BLACK);
  tft.fillCircle(x + 4, y - 4, 2, ST77XX_BLACK);
  tft.fillCircle(x + 10, y - 9, 3, ST77XX_CYAN);
  tft.drawLine(x - 5, y + 5, x - 1, y + 7, ST77XX_BLACK);
  tft.drawLine(x - 1, y + 7, x + 2, y + 5, ST77XX_BLACK);
  tft.drawLine(x + 2, y + 5, x + 5, y + 7, ST77XX_BLACK);
}

// Draws a water droplet shape, used for the "humid" weather state.
void drawDrop(int x, int y) {
  tft.fillCircle(x, y + 7, 9, ST77XX_CYAN);
  tft.fillTriangle(x, y - 6, x - 9, y + 6, x + 9, y + 6, ST77XX_CYAN);
}

// Draws a sunrise icon - half a sun peeking over a horizon line.
void drawSunrise(int x, int y) {
  tft.fillCircle(x, y + 7, 9, ST77XX_YELLOW);
  tft.fillRect(x - 16, y + 7, 32, 10, ST77XX_BLACK);
  tft.drawLine(x - 14, y + 7, x + 14, y + 7, 0xFD20);
  for (int i = 0; i < 5; i++) {
    float angle = (i * 3.14159 / 4) - 3.14159;
    int x1 = x + cos(angle) * 11;
    int y1 = y + 7 + sin(angle) * 11;
    int x2 = x + cos(angle) * 16;
    int y2 = y + 7 + sin(angle) * 16;
    tft.drawLine(x1, y1, x2, y2, 0xFD20);
  }
}

// Draws a "partly cloudy" icon by reusing drawSun() and drawCloud().
void drawPartlyCloudy(int x, int y) {
  drawSun(x + 4, y + 4);
  drawCloud(x + 1, y + 10);
}

// ============================================================
//  WEATHER LOGIC
// ============================================================

// Holds everything needed to describe one "weather mood": which
// icon to draw, what the two lines of text say, and what colour
// to use for the text.
struct WeatherInfo {
  void (*drawIcon)(int, int);
  const char* line1;
  const char* line2;
  uint16_t color;
};

// Works out a friendly description of the current weather from
// the sensor readings. Uses named threshold constants instead of
// magic numbers so the conditions actually make sense to read.
WeatherInfo getWeather(float temp, float hum, float uvi, float lux) {
  if (temp >= HOT_TEMP_C && uvi >= HOT_UVI && lux >= HOT_LUX)
    return { drawHotFace,      "Hot & sunny!",    "Use sunscreen!",  ST77XX_RED    };

  if (temp >= WARM_TEMP_C && uvi >= WARM_UVI && lux >= WARM_LUX)
    return { drawSun,          "Warm & sunny!",   "Great day out!",  ST77XX_YELLOW };

  if (temp >= MILD_TEMP_C && lux >= MILD_LUX)
    return { drawSunCloud,     "Mild & pleasant", "Enjoy the day!",  ST77XX_GREEN  };

  if (temp < COLD_TEMP_C && lux < DARK_LUX)
    return { drawRain,         "Cold & dark,",    "Stay inside!",    ST77XX_BLUE   };

  if (lux < DULL_LUX && temp >= COLD_TEMP_C)
    return { drawCloud,        "Cloudy & dull,",  "Grab a jacket!",  0xC618        };

  if (hum >= HUMID_PERCENT)
    return { drawDrop,         "Humid & sticky,", "Stay hydrated!",  ST77XX_CYAN   };

  if (temp < COOL_TEMP_C && lux >= BRIGHT_LUX)
    return { drawSunrise,      "Cool & bright,",  "Wrap up!",        0xFD20        };

  return   { drawPartlyCloudy, "Looks normal",    "out there!",      ST77XX_WHITE  };
}

// ============================================================
//  TFT SCREENS
// ============================================================

// Shows the raw sensor numbers. Also shows a warning if any
// sensor value being displayed is currently invalid, so the user
// isn't misled by a frozen/old number without knowing it.
void displayHardData(float temp, float hum, float pres, float uvi, float lux) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(4, 2);
  tft.print("RAW DATA          D0:switch"); 

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(4, 16);  tft.print("Temp:     "); tft.print(temp, 1); tft.print("C");
  tft.setCursor(4, 38);  tft.print("Humidity: "); tft.print(hum, 1);  tft.print("%");
  tft.setCursor(4, 60);  tft.print("Pressure: "); tft.print(pres, 0); tft.print("hPa");
  tft.setCursor(4, 82);  tft.print("UV Index: "); tft.print(uvi, 1);
  tft.setCursor(4, 104); tft.print("Light:    "); tft.print(lux, 0);  tft.print("lx");

  if (!ahtOK || !bmpOK || !ltrOK) {
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_RED);
    tft.setCursor(4, 124);
    tft.print("Sensor error - check Serial");
  }
}

// Shows the friendly icon + description screen.
void displayFriendly(float temp, float hum, float pres, float uvi, float lux) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  WeatherInfo w = getWeather(temp, hum, uvi, lux);

  // Draw shape icon on the left (smaller now, so it takes up less space)
  w.drawIcon(28, 50);

  // Two lines of text on the right - moved left slightly since the
  // icon is smaller, giving the text more room before the screen edge
  tft.setTextColor(w.color);
  tft.setTextSize(2);
  tft.setCursor(70, 30);
  tft.print(w.line1);
  tft.setCursor(70, 60);
  tft.print(w.line2);

  // Summary bar along the bottom
  tft.setTextColor(TFT_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(4, 124);
  tft.print(temp, 1); tft.print("C ");
  tft.print(hum, 0);  tft.print("% UV:");
  tft.print(uvi, 1);
  tft.setCursor(195, 124);
  tft.print("D0:sw");
}

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  pinMode(BUTTON_D0, INPUT_PULLUP);

  tft.init(135, 240);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 55);
  tft.print("Starting...");

  Wire.begin();

  // Try starting each sensor a few times instead of giving up
  // straight away or freezing forever on the first failure. If a
  // sensor still isn't working after a few tries we just carry on
  // without it - the rest of the project should still work.
  for (int i = 0; i < SENSOR_MAX_TRIES && !ahtOK; i++) {
    ahtOK = aht.begin();
    if (!ahtOK) delay(SENSOR_RETRY_MS);
  }

  for (int i = 0; i < SENSOR_MAX_TRIES && !bmpOK; i++) {
    bmpOK = bmp.begin(0x76) || bmp.begin(0x77);
    if (!bmpOK) delay(SENSOR_RETRY_MS);
  }

  for (int i = 0; i < SENSOR_MAX_TRIES && !ltrOK; i++) {
    ltrOK = ltr.begin();
    if (!ltrOK) delay(SENSOR_RETRY_MS);
  }

  if (!ahtOK) Serial.println("AHT20 did not start - check wiring");
  if (!bmpOK) Serial.println("BMP280 did not start - check wiring");
  if (!ltrOK) Serial.println("LTR390 did not start - check wiring");

  if (bmpOK) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
  }

  if (ltrOK) {
    ltr.setMode(LTR390_MODE_UVS);
    ltr.setGain(LTR390_GAIN_3);
    ltr.setResolution(LTR390_RESOLUTION_16BIT);
  }

  tft.fillScreen(ST77XX_BLACK);
}

// ============================================================
//  LOOP
// ============================================================

void loop() {
  // Simple debounce so one press doesn't register multiple times.
  if (digitalRead(BUTTON_D0) == LOW && millis() - lastButtonPress > BUTTON_DEBOUNCE_MS) {
    hardDataMode = !hardDataMode;
    lastButtonPress = millis();
  }

  // Take new readings. Temperature and humidity come from one
  // combined call since they're read from the same sensor at the
  // same time.
  float temp, hum;
  readAHT(temp, hum);
  float pres = readPressure();
  float uvi  = readUVI();
  float lux  = readLux();

  // Check each reading is realistic. If not, fall back to the last
  // good value instead of showing a broken number.
  if (isValidReading(temp, MIN_VALID_TEMP_C, MAX_VALID_TEMP_C)) lastTemp = temp; else temp = lastTemp;
  if (isValidReading(hum,  MIN_VALID_HUM,    MAX_VALID_HUM))    lastHum  = hum;  else hum  = lastHum;
  if (isValidReading(pres, MIN_VALID_PRES,   MAX_VALID_PRES))   lastPres = pres; else pres = lastPres;
  if (!isnan(uvi)) lastUvi = uvi; else uvi = lastUvi;
  if (!isnan(lux)) lastLux = lux; else lux = lastLux;

  if (hardDataMode) displayHardData(temp, hum, pres, uvi, lux);
  else              displayFriendly(temp, hum, pres, uvi, lux);

  Serial.printf("Temp: %.1f | Hum: %.1f%% | Pres: %.1f | UVI: %.2f | Lux: %.1f\n",
                temp, hum, pres, uvi, lux);

  delay(LOOP_DELAY_MS);
}