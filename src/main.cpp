/*
#include <Arduino.h>
#include <FreeRTOS.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_MPU6050.h>
#include <DHT.h>
#include <HX711.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

#define DHTPIN 6 // Pin DHT22
#define DHTTYPE DHT22
#define STRAIN_GAUGE_DOUT 7 // Pin data untuk HX711
#define STRAIN_GAUGE_SCK 8  // Pin clock untuk HX711
#define MPU6050_ADDRESS 0x68
#define ADXL345_ADDRESS 0x53
#define SDA 21
#define SCL 22

#define RESET_MPU 2
#define RESET_ADXL345 3
#define RESET_OLED 4
#define INDICATOR 5

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SSD1306_I2C_ADDRESS 0x3C // I2C address SSD1306
#define OLED_RESET 9

#define STRAIN_GAUGE_PIN 10 // Pin Strain Gauge Module Y3 BF350-3AA

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

class SensorManager
{
public:
  SensorManager() : dht(DHTPIN, DHTTYPE)
  {
    adxl.begin(ADXL345_ADDRESS);
    mpu.begin(MPU6050_ADDRESS);
    strainGauge.begin(STRAIN_GAUGE_DOUT, STRAIN_GAUGE_SCK);
    dht.begin();
  }

  void readSensors()
  {
    readMPU6050();
    readADXL345();
    readStrainGauge();
    readDHT22();
  }

  float getStrainValue() const
  {
    return strainValue;
  }

  float getTemperature()
  {
    return temperature;
  }

  float getHumidity()
  {
    return humidity;
  }

  float getGyroX() { return gyroX; }
  float getGyroY() { return gyroY; }
  float getGyroZ() { return gyroZ; }

  float getAccelX() { return accelX; }
  float getAccelY() { return accelY; }
  float getAccelZ() { return accelZ; }

private:
  Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
  Adafruit_MPU6050 mpu;
  DHT dht;
  HX711 strainGauge;

  float strainValue;
  float temperature;
  float humidity;

  float gyroX, gyroY, gyroZ;
  float accelX, accelY, accelZ;

  void readMPU6050()
  {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    gyroX = g.gyro.x;
    gyroY = g.gyro.y;
    gyroZ = g.gyro.z;
  }

  void readADXL345()
  {
    sensors_event_t event;
    adxl.getEvent(&event);
    accelX = event.acceleration.x;
    accelY = event.acceleration.y;
    accelZ = event.acceleration.z;
  }

  void readStrainGauge()
  {
    int hodnota = analogRead(STRAIN_GAUGE_PIN);
    strainValue = map(hodnota, 0, 700, 0, 100);
  }

public:
  void readDHT22()
  {
    temperature = dht.readTemperature();
    humidity = dht.readHumidity();

    if (isnan(temperature) || isnan(humidity))
    {
      temperature = 0; // NAN
      humidity = 0;    // NAN
    }
  }
};

class WiFiManager
{
public:
  WiFiManager(const char *ssid, const char *password)
  {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(1000);
    }
  }

  void sendData(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strain, float temperature, float humidity)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      reconnectWiFi();
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      HTTPClient http;
      String url = "http://10.17.38.92/WebsiteMonitoring/SHMS";
      http.begin(url);

      http.addHeader("Content-Type", "application/json");
      String jsonData = "{\"gyroX\": " + String(gyroX) + ", \"gyroY\": " + String(gyroY) + ", \"gyroZ\": " + String(gyroZ) +
                        ", \"accelX\": " + String(accelX) + ", \"accelY\": " + String(accelY) + ", \"accelZ\": " + String(accelZ) +
                        ", \"strain\": " + String(strain) + ", \"temperature\": " + String(temperature) + ", \"humidity\": " + String(humidity) + "}";

      int httpResponseCode = http.POST(jsonData);
      if (httpResponseCode > 0)
      {
        Serial.printf("Data terkirim: %s\n", jsonData.c_str());
      }
      else
      {
        Serial.printf("Gagal mengirim data. Kode Respon: %d\n", httpResponseCode);
      }

      http.end();
    }
  }

  void reconnectWiFi()
  {
    while (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("Mencoba menyambung kembali ke WiFi...");
      WiFi.reconnect();
      delay(1000); // reconnect setiap 1 detik
    }
    Serial.println("Terhubung kembali ke WiFi!");
  }
};

SensorManager sensorManager;
WiFiManager wifiManager("DTEO-VOKASI", "TEO123456");

void setup()
{
  Wire.begin(SDA, SCL);
  display.begin(SSD1306_I2C_ADDRESS, OLED_RESET);
  display.clearDisplay();

  Serial.begin(115200);

  pinMode(RESET_MPU, OUTPUT);
  pinMode(RESET_ADXL345, OUTPUT);
  pinMode(RESET_OLED, OUTPUT);
  pinMode(INDICATOR, OUTPUT);

  digitalWrite(RESET_MPU, LOW);
  delay(100);
  digitalWrite(RESET_MPU, HIGH);

  digitalWrite(RESET_ADXL345, LOW);
  delay(100);
  digitalWrite(RESET_ADXL345, HIGH);

  digitalWrite(RESET_OLED, LOW);
  delay(100);
  digitalWrite(RESET_OLED, HIGH);
}

void loop()
{
  static unsigned long lastDHT22 = 0;
  static unsigned long lastSensorRead = 0;
  static unsigned long lastDisplayUpdate = 0;

  if (millis() - lastSensorRead >= 200) // 5hz
  {
    sensorManager.readSensors();
    lastSensorRead = millis();

    // Cetak data sensor ke Serial
    Serial.print("Gyro X: ");
    Serial.println(sensorManager.getGyroX());
    Serial.print("Gyro Y: ");
    Serial.println(sensorManager.getGyroY());
    Serial.print("Gyro Z: ");
    Serial.println(sensorManager.getGyroZ());
    Serial.print("Accel X: ");
    Serial.println(sensorManager.getAccelX());
    Serial.print("Accel Y: ");
    Serial.println(sensorManager.getAccelY());
    Serial.print("Accel Z: ");
    Serial.println(sensorManager.getAccelZ());
    Serial.print("Strain: ");
    Serial.println(sensorManager.getStrainValue());
    Serial.print("Temp: ");
    Serial.println(sensorManager.getTemperature());
    Serial.print("Humidity: ");
    Serial.println(sensorManager.getHumidity());
  }

  if (millis() - lastDHT22 >= 2000) // 0.5hz
  {
    sensorManager.readDHT22();
    lastDHT22 = millis();
  }

  wifiManager.sendData(sensorManager.getGyroX(), sensorManager.getGyroY(), sensorManager.getGyroZ(),
                       sensorManager.getAccelX(), sensorManager.getAccelY(), sensorManager.getAccelZ(),
                       sensorManager.getStrainValue(), sensorManager.getTemperature(), sensorManager.getHumidity());

  if (millis() - lastDisplayUpdate >= 2000) // 5s
  {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Gyro X: ");
    display.println(sensorManager.getGyroX());
    display.print("Gyro Y: ");
    display.println(sensorManager.getGyroY());
    display.print("Gyro Z: ");
    display.println(sensorManager.getGyroZ());
    display.print("Accel X: ");
    display.println(sensorManager.getAccelX());
    display.print("Accel Y: ");
    display.println(sensorManager.getAccelY());
    display.print("Accel Z: ");
    display.println(sensorManager.getAccelZ());
    display.print("Strain: ");
    display.println(sensorManager.getStrainValue());
    display.print("Temp: ");
    display.println(sensorManager.getTemperature());
    display.print("Humidity: ");
    display.println(sensorManager.getHumidity());
    display.display();

    lastDisplayUpdate = millis(); // Update waktu untuk display
  }

  // Delay untuk pengelolaan CPU
  vTaskDelay(pdMS_TO_TICKS(10)); // 10 ms delay
}

*/

// -----DEBUGGING FEATURE----------- //
#include <Arduino.h>
#include <FreeRTOS.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_MPU6050.h>
#include <DHT.h>
#include <HX711.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

#define DHTPIN 6            // Pin DHT22
#define DHTTYPE DHT22

#define STRAIN_GAUGE_DOUT 7 // Pin data HX711 -> Not Used
#define STRAIN_GAUGE_SCK 8  // Pin clock HX711 -> Not Used
#define STRAIN_GAUGE_PIN 10 // Pin Strain Gauge Module Y3 BF350-3AA -> Used

#define MPU6050_ADDRESS 0x68
#define ADXL345_ADDRESS 0x53
#define SDA 21
#define SCL 22

#define RESET_MPU 2
#define RESET_ADXL345 3
#define RESET_OLED 4
#define OLED_RESET 9

#define INDICATOR 5

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SSD1306_I2C_ADDRESS 0x3C // I2C address SSD1306

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

class SensorManager
{
public:
  SensorManager() : dht(DHTPIN, DHTTYPE)
  {
    adxl.begin(ADXL345_ADDRESS);
    mpu.begin(MPU6050_ADDRESS);
    strainGauge.begin(STRAIN_GAUGE_DOUT, STRAIN_GAUGE_SCK);
    dht.begin();
  }

  void readSensors()
  {
    readMPU6050();
    readADXL345();
    readStrainGauge();
  }

  void readDHT22()
  {
    temperature = dht.readTemperature();
    humidity = dht.readHumidity();

    if (isnan(temperature) || isnan(humidity))
    {
      temperature = 0; // NAN
      humidity = 0;    // NAN
    }
  }

  float getStrainValue() const
  {
    return strainValue;
  }

  float getTemperature()
  {
    return temperature;
  }

  float getHumidity()
  {
    return humidity;
  }

  float getGyroX() { return gyroX; }
  float getGyroY() { return gyroY; }
  float getGyroZ() { return gyroZ; }

  float getAccelX() { return accelX; }
  float getAccelY() { return accelY; }
  float getAccelZ() { return accelZ; }

private:
  Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
  Adafruit_MPU6050 mpu;
  DHT dht;
  HX711 strainGauge;

  float strainValue;
  float temperature;
  float humidity;

  float gyroX, gyroY, gyroZ;
  float accelX, accelY, accelZ;

  void readMPU6050()
  {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    gyroX = g.gyro.x;
    gyroY = g.gyro.y;
    gyroZ = g.gyro.z;
  }

  void readADXL345()
  {
    sensors_event_t event;
    adxl.getEvent(&event);
    accelX = event.acceleration.x;
    accelY = event.acceleration.y;
    accelZ = event.acceleration.z;
  }

  void readStrainGauge()
  {
    int hodnota = analogRead(STRAIN_GAUGE_PIN);
    strainValue = map(hodnota, 0, 700, 0, 100);
  }
};

class WiFiManager
{
public:
  WiFiManager(const char *ssid, const char *password)
  {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
      vTaskDelay(1000);
    }
  }

  void sendData(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strain, float temperature, float humidity)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      reconnectWiFi();
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      HTTPClient http;
      String url = "http://10.17.38.92/WebsiteMonitoring/SHMS";
      http.begin(url);

      http.addHeader("Content-Type", "application/json");
      String jsonData = "{\"gyroX\": " + String(gyroX) + ", \"gyroY\": " + String(gyroY) + ", \"gyroZ\": " + String(gyroZ) +
                        ", \"accelX\": " + String(accelX) + ", \"accelY\": " + String(accelY) + ", \"accelZ\": " + String(accelZ) +
                        ", \"strain\": " + String(strain) + ", \"temperature\": " + String(temperature) + ", \"humidity\": " + String(humidity) + "}";

      Serial.println("Mengirim data: " + jsonData);
      int httpResponseCode = http.POST(jsonData);
      if (httpResponseCode > 0)
      {
        Serial.printf("Data terkirim: %s\n", jsonData.c_str());
      }
      else
      {
        Serial.printf("Gagal mengirim data. Kode Respon: %d\n", httpResponseCode);
      }

      http.end();
    }
  }

  void reconnectWiFi()
  {
    while (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("Mencoba menyambung kembali ke WiFi...");
      WiFi.reconnect();
      vTaskDelay(1000); // reconnect 1 detik
    }
    Serial.println("Terhubung kembali ke WiFi!");
  }
};

SensorManager sensorManager;
WiFiManager wifiManager("DTEO-VOKASI", "TEO123456");

void TaskReadSensors(void *pvParameters)
{
  for (;;)
  {
    sensorManager.readSensors();
    vTaskDelay(pdMS_TO_TICKS(200)); // 5 Hz
  }
}

void TaskReadDHT22(void *pvParameters)
{
  for (;;)
  {
    sensorManager.readDHT22();
    vTaskDelay(pdMS_TO_TICKS(2000)); // 0.5 Hz
  }
}

void TaskSendData(void *pvParameters)
{
  for (;;)
  {
    wifiManager.sendData(sensorManager.getGyroX(), sensorManager.getGyroY(), sensorManager.getGyroZ(),
                         sensorManager.getAccelX(), sensorManager.getAccelY(), sensorManager.getAccelZ(),
                         sensorManager.getStrainValue(), sensorManager.getTemperature(), sensorManager.getHumidity());
    vTaskDelay(pdMS_TO_TICKS(1000)); // Send 1s
  }
}

void TaskUpdateDisplay(void *pvParameters)
{
  for (;;)
  {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Gyro X: ");
    display.println(sensorManager.getGyroX());
    display.print("Gyro Y: ");
    display.println(sensorManager.getGyroY());
    display.print("Gyro Z: ");
    display.println(sensorManager.getGyroZ());
    display.print("Accel X: ");
    display.println(sensorManager.getAccelX());
    display.print("Accel Y: ");
    display.println(sensorManager.getAccelY());
    display.print("Accel Z: ");
    display.println(sensorManager.getAccelZ());
    display.print("Strain: ");
    display.println(sensorManager.getStrainValue());
    display.print("Temp: ");
    display.println(sensorManager.getTemperature());
    display.print("Humidity: ");
    display.println(sensorManager.getHumidity());
    display.display();

    vTaskDelay(pdMS_TO_TICKS(1000)); // Refresh Display 1s
  }
}

void setup()
{
  Wire.begin(SDA, SCL);
  display.begin(SSD1306_I2C_ADDRESS, OLED_RESET);
  display.clearDisplay();

  Serial.begin(115200);

  pinMode(RESET_MPU, OUTPUT);
  pinMode(RESET_ADXL345, OUTPUT);
  pinMode(RESET_OLED, OUTPUT);
  pinMode(INDICATOR, OUTPUT);

  digitalWrite(RESET_MPU, LOW);
  vTaskDelay(100);
  digitalWrite(RESET_MPU, HIGH);

  digitalWrite(RESET_ADXL345, LOW);
  vTaskDelay(100);
  digitalWrite(RESET_ADXL345, HIGH);

  digitalWrite(RESET_OLED, LOW);
  vTaskDelay(100);
  digitalWrite(RESET_OLED, HIGH);

  // FreeRTOS
  xTaskCreate(TaskReadSensors, "TaskReadSensors", 2048, NULL, 1, NULL);
  xTaskCreate(TaskReadDHT22, "TaskReadDHT22", 2048, NULL, 1, NULL);
  xTaskCreate(TaskSendData, "TaskSendData", 2048, NULL, 1, NULL);
  xTaskCreate(TaskUpdateDisplay, "TaskUpdateDisplay", 2048, NULL, 1, NULL);
}

void loop()
{
}
