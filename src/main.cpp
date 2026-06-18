#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_LTR390.h>

// --- Sensor objects ---
Adafruit_AHTX0  aht;
Adafruit_BMP280 bmp;
Adafruit_LTR390 ltr;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Wire.begin();

  Serial.println("ESP32-S3 Feather – Sensor Init");
  Serial.println("================================");

  if (!aht.begin()) {
    Serial.println("[ERROR] AHT20 not found! Check wiring.");
    while (1) delay(100);
  }
  Serial.println("[OK] AHT20 ready");

  if (!bmp.begin(0x76)) {
    if (!bmp.begin(0x77)) {
      Serial.println("[ERROR] BMP280 not found! Check wiring / address.");
      while (1) delay(100);
    }
  }
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                  Adafruit_BMP280::SAMPLING_X2,
                  Adafruit_BMP280::SAMPLING_X16,
                  Adafruit_BMP280::FILTER_X16,
                  Adafruit_BMP280::STANDBY_MS_500);
  Serial.println("[OK] BMP280 ready");

  if (!ltr.begin()) {
    Serial.println("[ERROR] LTR390 not found! Check wiring.");
    while (1) delay(100);
  }
  ltr.setMode(LTR390_MODE_UVS);
  ltr.setGain(LTR390_GAIN_3);
  ltr.setResolution(LTR390_RESOLUTION_16BIT);
  Serial.println("[OK] LTR390 ready");

  Serial.println();
  Serial.println("Temp(°C) | Humidity(%) | Pressure(hPa) | UV Index | Lux");
  Serial.println("----------------------------------------------------------");
}

void loop() {
  sensors_event_t humidity_event, temp_event;
  aht.getEvent(&humidity_event, &temp_event);
  float temperature = temp_event.temperature;
  float humidity    = humidity_event.relative_humidity;

  float pressure = bmp.readPressure() / 100.0F;

  float uvi = 0;
  float lux = 0;

  ltr.setMode(LTR390_MODE_UVS);
  delay(120);
  if (ltr.newDataAvailable()) {
    uint32_t raw_uv = ltr.readUVS();
    uvi = (float)raw_uv / 2300.0F;
  }

  ltr.setMode(LTR390_MODE_ALS);
  delay(120);
  if (ltr.newDataAvailable()) {
    uint32_t raw_als = ltr.readALS();
    lux = (0.6F * (float)raw_als) / (3.0F * 1.0F);
  }

  Serial.print("Temp:     "); Serial.print(temperature, 1); Serial.println(" °C");
  Serial.print("Humidity: "); Serial.print(humidity, 1);    Serial.println(" %");
  Serial.print("Pressure: "); Serial.print(pressure, 1);   Serial.println(" hPa");
  Serial.print("UV Index: "); Serial.println(uvi, 2);
  Serial.print("Light:    "); Serial.print(lux, 1);        Serial.println(" lux");
  Serial.println("----------------------------------------------------------");

  delay(2000);
}