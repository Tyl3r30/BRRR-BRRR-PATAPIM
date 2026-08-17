/*
  Weather Station
  ---------------------------------
  Reads temperature, humidity, pressure, UV and light from the
  sensors and shows it on the TFT screen. Has two screens you can
  swap between with the button - a nice friendly one with icons,
  and a raw numbers one for checking the actual values.

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

// Sensors are on the board's default I2C pins, so Wire.begin()
// doesn't need any arguments.
// TFT_CS, TFT_DC and TFT_RST aren't defined anywhere in this file
// because this board already defines them for us in the board
// files, so I can just use them straight away.

// ============================================================
//  WEATHER THRESHOLD CONSTANTS
//  I made these into named constants instead of just typing numbers
//  into getWeather(), makes it way easier to read and tweak later.
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
//  These are here so I can catch a sensor giving a dodgy reading
//  instead of just trusting whatever number comes back.
// ============================================================

const float MIN_VALID_TEMP_C = -40.0;
const float MAX_VALID_TEMP_C = 85.0;
const float MIN_VALID_HUM    = 0.0;
const float MAX_VALID_HUM    = 100.0;
const float MIN_VALID_PRES   = 300.0;   // hPa, way below sea level pressure
const float MAX_VALID_PRES   = 1100.0;  // hPa, way above sea level pressure
const float MIN_VALID_UVI    = 0.0;
const float MAX_VALID_UVI    = 20.0;    // highest UV index that actually happens
const float MIN_VALID_LUX    = 0.0;
const float MAX_VALID_LUX    = 130000.0; // about as bright as direct sunlight gets

const unsigned long BUTTON_DEBOUNCE_MS = 300;
const unsigned long LOOP_DELAY_MS      = 2000;
const unsigned long SENSOR_RETRY_MS    = 500;
const int            SENSOR_MAX_TRIES  = 5;

// ============================================================
//  SENSOR CALIBRATION / ADDRESS CONSTANTS
//  Pulled these numbers out into constants so they're not just
//  random numbers sitting in the read functions with no explanation.
// ============================================================

// BMP280 could be wired to either address depending on the board,
// so setup() just tries both.
const uint8_t BMP280_ADDR_PRIMARY   = 0x76;
const uint8_t BMP280_ADDR_SECONDARY = 0x77;

// These conversion numbers come from Adafruit's own example code
// for the LTR390 sensor.
const float UVI_SENSITIVITY_DIVISOR = 2300.0F;
const float LUX_WINDOW_FACTOR = 0.6F;
const float LUX_GAIN_DIVISOR  = 3.0F;

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

// Keeps track of whether each sensor actually started up properly.
// If one fails I don't want the whole board to just stop working,
// the rest of the project should still run fine.
bool ahtOK = false;
bool bmpOK = false;
bool ltrOK = false;

// Stores the last good reading for each value. If a new reading
// comes back invalid I just reuse this instead of showing garbage
// on the screen.
float lastTemp = 0;
float lastHum  = 0;
float lastPres = 0;
float lastUvi  = 0;
float lastLux  = 0;

unsigned long lastButtonPress = 0;

// ============================================================
//  SENSOR READING FUNCTIONS
// ============================================================

// Gets temp (C) and humidity (%) from the AHT20 sensor. Returns
// them through the tempOut/humOut parameters since I need two
// values back at once. If the sensor never started properly I just
// set both to NAN so the validity check further down catches it.
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

// Gets pressure from the BMP280 and converts it from Pa to hPa
// since hPa is easier to read. Returns NAN if the sensor's not
// working so it gets filtered out like any other bad reading.
float readPressure() {
  if (!bmpOK) return NAN;
  return bmp.readPressure() / 100.0F;
}

// Gets the UV Index from the LTR390. The sensor needs a bit of time
// to settle after switching modes, so if there's no new data ready
// yet I just return NAN. The fallback to the last good value only
// happens once, down in loop(), same as the other sensors.
float readUVI() {
  if (!ltrOK) return NAN;
  ltr.setMode(LTR390_MODE_UVS);
  delay(120);
  if (!ltr.newDataAvailable()) return NAN;
  return (float)ltr.readUVS() / UVI_SENSITIVITY_DIVISOR;
}

// Gets light level (lux) from the LTR390, works the same as readUVI().
float readLux() {
  if (!ltrOK) return NAN;
  ltr.setMode(LTR390_MODE_ALS);
  delay(120);
  if (!ltr.newDataAvailable()) return NAN;
  return (LUX_WINDOW_FACTOR * ltr.readALS()) / LUX_GAIN_DIVISOR;
}

// Checks a reading is an actual number and makes sense for what
// it's measuring. This is the one spot that decides if a reading
// is valid or not, so I'm not repeating this check everywhere.
bool isValidReading(float value, float minAllowed, float maxAllowed) {
  if (isnan(value)) return false;
  if (value < minAllowed || value > maxAllowed) return false;
  return true;
}

// ============================================================
//  SHAPE ICONS FOR THE FRIENDLY WEATHER SCREEN
//  Each of these just draws one small icon around the x,y position
//  given to it. A few of them reuse other icon functions instead of
//  drawing the same shapes twice (e.g. drawSunCloud() below).
// ============================================================

// Draws a sun - a filled circle with rays coming off it.
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

// Draws a cloud out of a few overlapping circles and a rectangle
// to fill in the bottom.
void drawCloud(int x, int y) {
  uint16_t color = 0xC618;
  tft.fillCircle(x,      y,     7, color);
  tft.fillCircle(x + 8,  y - 3, 9, color);
  tft.fillCircle(x + 18, y,     7, color);
  tft.fillRect(x - 1, y, 28, 8, color);
}

// Draws a small sun poking out from behind a cloud, just calls
// drawSun() and drawCloud() at slightly different spots instead of
// writing the shapes out again.
void drawSunCloud(int x, int y) {
  drawSun(x + 6, y + 6);
  drawCloud(x + 7, y + 13);
}

// Draws a cloud with some rain lines underneath, reuses drawCloud().
void drawRain(int x, int y) {
  drawCloud(x, y);
  for (int i = 0; i < 4; i++) {
    tft.drawLine(x + 3 + i * 7, y + 15, x + 1 + i * 7, y + 21, ST77XX_BLUE);
  }
}

// Draws a red "hot" face - eyes, a sweat drop and a wavy mouth.
void drawHotFace(int x, int y) {
  tft.fillCircle(x, y, 13, ST77XX_RED);
  tft.fillCircle(x - 4, y - 4, 2, ST77XX_BLACK);
  tft.fillCircle(x + 4, y - 4, 2, ST77XX_BLACK);
  tft.fillCircle(x + 10, y - 9, 3, ST77XX_CYAN);
  tft.drawLine(x - 5, y + 5, x - 1, y + 7, ST77XX_BLACK);
  tft.drawLine(x - 1, y + 7, x + 2, y + 5, ST77XX_BLACK);
  tft.drawLine(x + 2, y + 5, x + 5, y + 7, ST77XX_BLACK);
}

// Draws a single water drop, used for the humid weather state.
void drawDrop(int x, int y) {
  tft.fillCircle(x, y + 7, 9, ST77XX_CYAN);
  tft.fillTriangle(x, y - 6, x - 9, y + 6, x + 9, y + 6, ST77XX_CYAN);
}

// Draws a little sunrise - a sun sitting on a horizon line with
// rays only going up.
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

// Draws a sun mostly hidden by a cloud, used as the default icon
// when nothing else matches.
void drawPartlyCloudy(int x, int y) {
  drawSun(x + 4, y + 4);
  drawCloud(x + 1, y + 10);
}

// ============================================================
//  WEATHER LOGIC
// ============================================================

// Holds everything the friendly screen needs for one weather state:
// which icon function to call, the two lines of text, and what
// colour to draw them in. Using a struct means I only have to pass
// one thing around instead of four separate values.
struct WeatherInfo {
  void (*drawIcon)(int, int);
  const char* line1;
  const char* line2;
  uint16_t color;
};

// Works out which "weather state" matches the current readings by
// checking them against the threshold constants, starting with the
// most extreme conditions first. Whichever condition matches first
// wins. If nothing matches, it just returns a neutral default state.
WeatherInfo getWeather(float temp, float hum, float uvi, float lux) {
  if (temp >= HOT_TEMP_C && uvi >= HOT_UVI && lux >= HOT_LUX)
    return { drawHotFace,      "Hot & sunny!",    "Use sunscreen!",  ST77XX_RED    };

  if (temp >= WARM_TEMP_C && uvi >= WARM_UVI && lux >= WARM_LUX)
    return { drawSun,          "Warm & sunny!",   "Great day out!",  ST77XX_YELLOW };

  if (temp >= MILD_TEMP_C && lux >= MILD_LUX)
    return { drawSunCloud,     "Mild & pleasant", "Enjoy the day!",  ST77XX_GREEN  };

  // Catches a hot day even when it's overcast (low light). Without
  // this, a 30 degree day with low light would slip past the sunny
  // checks above and end up wrongly showing "Cloudy & dull" further
  // down, which doesn't make sense for how hot it actually is.
  if (temp >= HOT_TEMP_C)
    return { drawHotFace,      "Hot & overcast,", "Stay cool!",      ST77XX_RED    };

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

// Draws the raw numbers screen - just every sensor value printed
// as text, plus a warning message if any sensor didn't start up.
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

// Draws the friendly screen - a weather icon, two lines of text
// from getWeather(), and a small strip of raw numbers at the bottom
// in case you still want to see them.
void displayFriendly(float temp, float hum, float pres, float uvi, float lux) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  WeatherInfo w = getWeather(temp, hum, uvi, lux);

  w.drawIcon(28, 50);

  tft.setTextColor(w.color);
  tft.setTextSize(2);
  tft.setCursor(70, 30);
  tft.print(w.line1);
  tft.setCursor(70, 60);
  tft.print(w.line2);

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
//  Runs once when the board turns on. Starts the serial monitor
//  and screen, tries to get each sensor going (retrying a few
//  times if it doesn't respond right away), then sets each one up.
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

  // Sensors are wired to the board's default I2C pins.
  Wire.begin();

  // Give each sensor a few tries to start, since sometimes they
  // don't respond straight away right after power-on.
  for (int i = 0; i < SENSOR_MAX_TRIES && !ahtOK; i++) {
    ahtOK = aht.begin();
    if (!ahtOK) delay(SENSOR_RETRY_MS);
  }

  for (int i = 0; i < SENSOR_MAX_TRIES && !bmpOK; i++) {
    bmpOK = bmp.begin(BMP280_ADDR_PRIMARY) || bmp.begin(BMP280_ADDR_SECONDARY);
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
//  Runs over and over. Checks the button, reads every sensor,
//  checks the readings are actually valid, redraws whichever
//  screen is selected, then logs everything to Serial.
// ============================================================

void loop() {

  // Checks if the button was pressed, with a debounce so it
  // doesn't flip back and forth from one press.
  if (digitalRead(BUTTON_D0) == LOW && millis() - lastButtonPress > BUTTON_DEBOUNCE_MS) {
    hardDataMode = !hardDataMode;
    lastButtonPress = millis();
  }

  // Grab a new reading from every sensor.
  float temp, hum;
  readAHT(temp, hum);
  float pres = readPressure();
  float uvi  = readUVI();
  float lux  = readLux();

  // Keeps a reading if it's valid, otherwise falls back to the
  // last known good value instead of showing something wrong.
  // Same pattern for all five values.
  if (isValidReading(temp, MIN_VALID_TEMP_C, MAX_VALID_TEMP_C)) lastTemp = temp; else temp = lastTemp;
  if (isValidReading(hum,  MIN_VALID_HUM,    MAX_VALID_HUM))    lastHum  = hum;  else hum  = lastHum;
  if (isValidReading(pres, MIN_VALID_PRES,   MAX_VALID_PRES))   lastPres = pres; else pres = lastPres;
  if (isValidReading(uvi,  MIN_VALID_UVI,    MAX_VALID_UVI))    lastUvi  = uvi;  else uvi  = lastUvi;
  if (isValidReading(lux,  MIN_VALID_LUX,    MAX_VALID_LUX))    lastLux  = lux;  else lux  = lastLux;

  // Redraw whichever screen is currently active.
  if (hardDataMode) displayHardData(temp, hum, pres, uvi, lux);
  else              displayFriendly(temp, hum, pres, uvi, lux);

  // Print the same values out to Serial too, handy for debugging.
  Serial.printf("Temp: %.1f | Hum: %.1f%% | Pres: %.1f | UVI: %.2f | Lux: %.1f\n",
                temp, hum, pres, uvi, lux);

  delay(LOOP_DELAY_MS);
}