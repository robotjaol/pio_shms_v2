#include <Arduino.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_MPU6050.h>
#include <DHT.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_SSD1306.h>

#define DHTPIN 1
#define DHTTYPE DHT22
#define MPU6050_ADDRESS 0x68
#define ADXL345_ADDRESS 0x53
#define SDA 8
#define SCL 9
#define OLED_RESET 7
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define SSD1306_I2C_ADDRESS 0x3C
#define STRAIN_GAUGE_PIN 20

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
Adafruit_MPU6050 mpu;
DHT dht(DHTPIN, DHTTYPE);
// Adafruit_ADXL345_Unified accelADXL = Adafruit_ADXL345_Unified(12345);

float temperature = 0, humidity = 0;
float strainValue;
unsigned long lastSensorReadTime = 0;
unsigned long lastDHTReadTime = 0;
unsigned long lastDisplayUpdateTime = 0;

// Interval
#define sensorReadInterval 200
#define dhtReadInterval 2000
#define displayUpdateInterval  2000

//Convertion
// Variabel untuk menyimpan hasil kalkulasi
float richterMagnitude = 0.0;
float tiltAngle = 0.0;
float angleInDegrees = 0.0;
const float distanceToEpicenter = 500.0; // Jarak ke episenter (pusat)

//WIFI
const char* ssid = "DTEO-VOKASI";
const char* password = "TEO123456";

// Display 
int displayIndex = 0;

// Fuzzy Variable
String vibrationStatus = "Unknown";

//Kalman variabel
float gyroX, gyroY, gyroZ, accelX, accelY, accelZ;
float ADaccelX, ADaccelY, ADaccelZ;
float dt = 0.01; // read 10ms Kalman 

typedef struct {
    float Q_angle;    // Varians proses untuk sudut
    float Q_bias;     // Varians proses untuk bias
    float R_measure;  // Varians pengukuran
    float angle;      // Sudut yang dikalkulasi
    float bias;       // Bias kalkulasi
    float rate;       // Laju rotasi
    float P[2][2];    // Matriks error estimasi
} KalmanFilter;

// Inisialisasi Kalman Filter
KalmanFilter kalmanX = {0.001f, 0.003f, 0.03f, 0, 0, 0, {{0, 0}, {0, 0}}};
KalmanFilter kalmanY = {0.001f, 0.003f, 0.03f, 0, 0, 0, {{0, 0}, {0, 0}}};

// Get Angle KF
float getKalmanAngle(KalmanFilter *kalman, float newAngle, float newRate, float dt) {
    // Prediksi
    kalman->rate = newRate - kalman->bias;
    kalman->angle += dt * kalman->rate;

    kalman->P[0][0] += dt * (dt * kalman->P[1][1] - kalman->P[0][1] - kalman->P[1][0] + kalman->Q_angle);
    kalman->P[0][1] -= dt * kalman->P[1][1];
    kalman->P[1][0] -= dt * kalman->P[1][1];
    kalman->P[1][1] += kalman->Q_bias * dt;

    // Update
    float S = kalman->P[0][0] + kalman->R_measure;
    float K[2] = {kalman->P[0][0] / S, kalman->P[1][0] / S};

    float y = newAngle - kalman->angle;
    kalman->angle += K[0] * y;
    kalman->bias += K[1] * y;

    float P00_temp = kalman->P[0][0];
    float P01_temp = kalman->P[0][1];

    kalman->P[0][0] -= K[0] * P00_temp;
    kalman->P[0][1] -= K[0] * P01_temp;
    kalman->P[1][0] -= K[1] * P00_temp;
    kalman->P[1][1] -= K[1] * P01_temp;

    angleInDegrees = kalman->angle * 180.0 / PI;
    return angleInDegrees;
}

// Fungsi untuk menghubungkan ke WiFi
void connectToWiFi()
{
  WiFi.begin(ssid, password);
  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    if (millis() - startTime > 8000) // Timeout 8 detik
    {
      Serial.println("WiFi gagal terhubung. Restarting...");
      ESP.restart();
    }
  }
  Serial.println("WiFi Terhubung!");
}

// Himpunan fuzzy untuk akselerasi
float membershipLow(float value) {
  return (value <= 1.0) ? 1.0 : (value >= 3.0 ? 0.0 : (3.0 - value) / 2.0);
}

float membershipMedium(float value) {
  return (value <= 2.0 || value >= 6.0) ? 0.0 : (value <= 4.0 ? (value - 2.0) / 2.0 : (6.0 - value) / 2.0);
}

float membershipHigh(float value) {
  return (value <= 4.0) ? 0.0 : (value >= 6.0 ? 1.0 : (value - 4.0) / 2.0);
}

// Fungsi parameter float inferensi fuzzy
float fuzzyInference(float accelValue) {

  float low = membershipLow(accelValue);
  float medium = membershipMedium(accelValue);
  float high = membershipHigh(accelValue);

  // fuzzy rule
  float noVibration = low;
  float slightVibration = medium;
  float strongVibration = high;

  // Defuzzifikasi menggunakan metode rata-rata berbobot
  return (noVibration * 0.0 + slightVibration * 50.0 + strongVibration * 100.0) /
         (noVibration + slightVibration + strongVibration);
}

// Decision-making getaran
String detectVibration(float accelValue) {
  float severity = fuzzyInference(accelValue);
  if (severity < 25.0) {
    return "No";
  } else if (severity < 75.0) {
    return "Slight";
  } else {
    return "Strong";
  }
}


// Fungsi membaca sensor DHT22
void readDHT()
{
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp) || isnan(hum))
  {
    Serial.println("DHT Error: Menggunakan nilai terakhir.");
  }
  else
  {
    temperature = temp;
    humidity = hum;
  }
}

// Fungsi membaca data sensor MPU6050, ADXL345, dan strain gauge
void readSensors()
{
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp); 
  
  //  gyro  MPU6050
  gyroX = g.gyro.x;
  gyroY = g.gyro.y;
  gyroZ = g.gyro.z;

  // MPU accelerometer
  accelX = a.acceleration.x;
  accelY = a.acceleration.y;
  accelZ = a.acceleration.z;

  // Baca akselerometer dari ADXL345
  sensors_event_t event;
  adxl.getEvent(&event);

  // ADXL accelerometer
  ADaccelX = event.acceleration.x;
  ADaccelY = event.acceleration.y;
  ADaccelZ = event.acceleration.z;

 // Hitung sudut dari akselerometer
  float accelAngleX = atan2(accelY, accelZ) * 180 / PI;
  float accelAngleY = atan2(-accelX, sqrt(accelY * accelY + accelZ * accelZ)) * 180 / PI;

 // Terapkan Kalman Filter
  float kalmanAngleX = getKalmanAngle(&kalmanX, accelAngleX, gyroX, dt);
  float kalmanAngleY = getKalmanAngle(&kalmanY, accelAngleY, gyroY, dt);

 // Debug
  // Serial.print("Kalman Angle X: "); Serial.println(kalmanAngleX);
  // Serial.print("Kalman Angle Y: "); Serial.println(kalmanAngleY);

  int rawValue = analogRead(STRAIN_GAUGE_PIN);
  strainValue = rawValue * (100.0 / 4095.0);

  vibrationStatus = detectVibration(abs(ADaccelZ)); // Deteksi getaran berdasarkan accelZ
  // Serial.println("Vibration Status: " + vibrationStatus);  // Debugging
  // Serial.println("Angle Kalman : ", angleInDegrees);
  richterMagnitude = calculateRichterMagnitude(ADaccelX, ADaccelY, ADaccelZ, distanceToEpicenter);

}


// Update tampilan OLED
void updateDisplay()
{
  display.clearDisplay(); // Clear Displat update
  display.setTextSize(1); // Ukuran teks standar
  display.setTextColor(SSD1306_WHITE); // Warna teks putih
  display.setCursor(0, 0); // Mulai dari pojok kiri atas

  switch (displayIndex)
  {
  case 0: // Data Gyroscope
    display.printf("Gyro X: %.2f deg/s\nGyro Y: %.2f deg/s\nGyro Z: %.2f deg/s", gyroX, gyroY, gyroZ);
    break;

  case 1: // Data Accelerometer
    display.printf("Accel X: %.2f m/s^2\nAccel Y: %.2f m/s^2\nAccel Z: %.2f m/s^2", accelX, accelY, accelZ);
    break;

  case 2: // Data Strain Gauge, Temperatur, dan Kelembaban
    display.printf("Strain: %.2f N\nTemp: %.2f C\nHumidity: %.2f %%", strainValue, temperature, humidity);
    break;

  case 3: // Status Getaran
    display.printf("Richter Mag: %.2f\nTilt: %.2f deg\nVibration: %s", 
                   richterMagnitude, angleInDegrees, vibrationStatus.c_str());
    break;

  }
  display.display(); 
  displayIndex = (displayIndex + 1) % 4; // Ganti layar berikutnya
}

// Kirim data ke server
void kirimDataKeServer()
{
  HTTPClient http;
  char postData[256];
  snprintf(postData, sizeof(postData),
           "humidity=%.2f&temperature=%.2f&gyroX=%.2f&gyroY=%.2f&gyroZ=%.2f&accelX=%.2f&accelY=%.2f&accelZ=%.2f&strainValue=%.2f",
           humidity, temperature, ADaccelX, ADaccelY, ADaccelZ, gyroX, gyroY, gyroZ, strainValue); 

  //http.begin("http://192.168.54.36/shmsv2_2/sensor.php"); -->
  // http.begin("http://10.17.38.92/SHMS2/shmsv2_2/sensor2.php"); //modul 2
  http.begin("http://10.17.38.92/SHMS2/shmsv2_2/sensor.php"); // modul 1
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int httpCode = http.POST(postData);
  if (httpCode > 0)
  {
    Serial.printf("HTTP Response code: %d\n", httpCode);
    String payload = http.getString();
    Serial.println(payload);
  }
  else
  {
    Serial.printf("HTTP request gagal: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// Function Magnitudo Richter (estimation < 500 Meter Center)
float calculateRichterMagnitude(float accelX, float accelY, float accelZ, float distance) {
  float totalAcceleration = sqrt(pow(accelX, 2) + pow(accelY, 2) + pow(accelZ, 2));
  float groundMotion = totalAcceleration * 980.0; // Konversi dari m/s² ke gal
  return log10(groundMotion) + 3.0 * log10(distance / 100.0) - 2.92;
}

// Setup program
void setup()
{
  Serial.begin(115200);
  Wire.begin(SDA, SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SSD1306_I2C_ADDRESS))
  {
    Serial.println("SSD1306 Gagal");
    while (true)
      ;
  }

  if (!mpu.begin())
    Serial.println("MPU6050 Tidak Terhubung");

  if (!adxl.begin())
    Serial.println("ADXL345 Tidak Terhubung");

  dht.begin();
  connectToWiFi();

  // Tampilkan pesan awal
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Connected !!");
  display.print("SSID: ");
  display.println(ssid);
  display.println("Inisialisasi Selesai");
  display.display();
  delay(2000);
}

// Loop utama
void loop()
{
  unsigned long currentMillis = millis();

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Reconnect WiFi ...");
    connectToWiFi();
  }

  if (currentMillis - lastDHTReadTime >= dhtReadInterval)
  {
    lastDHTReadTime = currentMillis;
    readDHT();
  }

  if (currentMillis - lastSensorReadTime >= sensorReadInterval)
  {
    lastSensorReadTime = currentMillis;
    readSensors();
    kirimDataKeServer();
  }

  if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
  {
    lastDisplayUpdateTime = currentMillis;
    updateDisplay();
  }
}

// #include <Arduino.h>
// #include <Adafruit_ADXL345_U.h>
// #include <Adafruit_MPU6050.h>
// #include <DHT.h>
// #include <Wire.h>
// #include <WiFi.h>
// #include <HTTPClient.h>
// #include <Adafruit_SSD1306.h>

// #define DHTPIN 1
// #define DHTTYPE DHT22
// #define MPU6050_ADDRESS 0x68
// #define ADXL345_ADDRESS 0x53
// #define SDA 8
// #define SCL 9
// #define OLED_RESET 7
// #define SCREEN_WIDTH 128
// #define SCREEN_HEIGHT 32
// #define SSD1306_I2C_ADDRESS 0x3C
// #define STRAIN_GAUGE_PIN 20

// Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
// Adafruit_MPU6050 mpu;
// DHT dht(DHTPIN, DHTTYPE);

// float temperature = 0, humidity = 0;
// float gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue;
// unsigned long lastSensorReadTime = 0;
// unsigned long lastDHTReadTime = 0;
// unsigned long lastDisplayUpdateTime = 0;
// const unsigned long sensorReadInterval = 200;
// const unsigned long dhtReadInterval = 2000;
// const unsigned long displayUpdateInterval = 2000;
// int displayIndex = 0;

// //WIFI
// const char* ssid = "DTEO-VOKASI";
// const char* password = "TEO123456";
// // Fungsi untuk menghubungkan ke WiFi
// void connectToWiFi()
// {
//   WiFi.begin(ssid, password);
//   unsigned long startTime = millis();

//   while (WiFi.status() != WL_CONNECTED)
//   {
//     delay(500);
//     if (millis() - startTime > 2000) // Timeout 10 detik
//     {
//       Serial.println("WiFi gagal terhubung. Restarting...");
//       ESP.restart();
//     }
//   }
//   Serial.println("WiFi Terhubung!");
// }

// // Fungsi membaca sensor DHT22
// void readDHT()
// {
//   float temp = dht.readTemperature();
//   float hum = dht.readHumidity();

//   if (isnan(temp) || isnan(hum))
//   {
//     Serial.println("DHT Error: Menggunakan nilai terakhir.");
//   }
//   else
//   {
//     temperature = temp;
//     humidity = hum;
//   }
// }

// // Fungsi membaca data sensor MPU6050, ADXL345, dan strain gauge
// void readSensors()
// {
//   sensors_event_t a, g, temp;
//   mpu.getEvent(&a, &g, &temp);

//   gyroX = g.gyro.x;
//   gyroY = g.gyro.y;
//   gyroZ = g.gyro.z;
//   accelX = a.acceleration.x;
//   accelY = a.acceleration.y;
//   accelZ = a.acceleration.z;

//   int rawValue = analogRead(STRAIN_GAUGE_PIN);
//   strainValue = rawValue * (100.0 / 1023.0);
// }

// // Update tampilan OLED
// void updateDisplay()
// {
//   display.clearDisplay(); // Bersihkan layar setiap update
//   display.setTextSize(1); // Ukuran teks standar
//   display.setTextColor(SSD1306_WHITE); // Warna teks putih
//   display.setCursor(0, 0); // Mulai dari pojok kiri atas

//   switch (displayIndex)
//   {
//   case 0: // Data Gyroscope
//     display.printf("Gyro X: %.2f deg/s\nGyro Y: %.2f deg/s\nGyro Z: %.2f deg/s", gyroX, gyroY, gyroZ);
//     break;

//   case 1: // Data Accelerometer
//     display.printf("Accel X: %.2f m/s^2\nAccel Y: %.2f m/s^2\nAccel Z: %.2f m/s^2", accelX, accelY, accelZ);
//     break;

//   case 2: // Data Strain Gauge, Temperatur, dan Kelembaban
//     display.printf("Strain: %.2f N\nTemp: %.2f C\nHumidity: %.2f %%", strainValue, temperature, humidity);
//     break;
//   }

//   display.display(); // Tampilkan hasil
//   displayIndex = (displayIndex + 1) % 3; // Ganti layar berikutnya
// }

// // Kirim data ke server
// void kirimDataKeServer()
// {
//   HTTPClient http;
//   char postData[256];
//   snprintf(postData, sizeof(postData),
//            "humidity=%.2f&temperature=%.2f&accelX=%.2f&accelY=%.2f&accelZ=%.2f&gyroX=%.2f&gyroY=%.2f&gyroZ=%.2f&strainValue=%.2f",
//            humidity, temperature, accelX, accelY, accelZ, gyroX, gyroY, gyroZ, strainValue);

//   http.begin("http://192.168.54.36/shmsv2_2/sensor.php");
//   http.addHeader("Content-Type", "application/x-www-form-urlencoded");

//   int httpCode = http.POST(postData);
//   if (httpCode > 0)
//   {
//     Serial.printf("HTTP Response code: %d\n", httpCode);
//     String payload = http.getString();
//     Serial.println(payload);
//   }
//   else
//   {
//     Serial.printf("HTTP request gagal: %s\n", http.errorToString(httpCode).c_str());
//   }
//   http.end();
// }

// // Setup program
// void setup()
// {
//   Serial.begin(115200);
//   Wire.begin(SDA, SCL);

//   if (!display.begin(SSD1306_SWITCHCAPVCC, SSD1306_I2C_ADDRESS))
//   {
//     Serial.println("SSD1306 Gagal");
//     while (true)
//       ;
//   }

//   if (!mpu.begin())
//     Serial.println("MPU6050 Tidak Terhubung");

//   if (!adxl.begin())
//     Serial.println("ADXL345 Tidak Terhubung");

//   dht.begin();
//   connectToWiFi();

//   // Tampilkan pesan awal
//   display.clearDisplay();
//   display.setTextSize(1);
//   display.setCursor(0, 0);
//   display.println("Connected !!");
//   display.print("SSID: ");
//   display.println(ssid);
//   display.println("Inisialisasi Selesai");
//   display.display();
//   delay(2000);
// }

// // Loop utama
// void loop()
// {
//   unsigned long currentMillis = millis();

//   if (WiFi.status() != WL_CONNECTED)
//   {
//     Serial.println("Reconnect WiFi ...");
//     connectToWiFi();
//   }

//   if (currentMillis - lastDHTReadTime >= dhtReadInterval)
//   {
//     lastDHTReadTime = currentMillis;
//     readDHT();
//   }

//   if (currentMillis - lastSensorReadTime >= sensorReadInterval)
//   {
//     lastSensorReadTime = currentMillis;
//     readSensors();
//     kirimDataKeServer();
//   }

//   if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
//   {
//     lastDisplayUpdateTime = currentMillis;
//     updateDisplay();
//   }
// }

// #include <Arduino.h>
// #include <Adafruit_ADXL345_U.h>
// #include <Adafruit_MPU6050.h>
// #include <DHT.h>
// #include <Wire.h>
// #include <WiFi.h>
// #include <HTTPClient.h>
// #include <Adafruit_SSD1306.h>

// // Define Floor and indicator
// #define FLOOR_1 37
// #define FLOOR_2 36
// #define FLOOR_3 35
// #define FLOOR_IND_1 38
// #define FLOOR_IND_2 39
// #define FLOOR_IND_3 40

// // Additional constants for DB access
// int lastLantai = 0;                                                 // Lantai terakhir ke db
// bool stateBtn1 = LOW, stateBtn2 = LOW, stateBtn3 = LOW;             // Status tombol
// bool lastStateBtn1 = LOW, lastStateBtn2 = LOW, lastStateBtn3 = LOW; // Status tombol sebelumnya

// // Define PIN DHT22
// #define DHTPIN 1
// #define DHTTYPE DHT22

// // Define MPU and ADXL
// #define MPU6050_ADDRESS 0x68
// #define ADXL345_ADDRESS 0x53
// #define SDA 8
// #define SCL 9

// // Define LCD output
// #define OLED_RESET 7 // RESET LCD output
// #define SCREEN_WIDTH 128
// #define SCREEN_HEIGHT 32
// #define SSD1306_I2C_ADDRESS 0x3C

// // Define Strain Gauge and Reset
// #define STRAIN_GAUGE_PIN 20
// #define RESET_BUTTON_PIN 2 // RESET All Instruments

// Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
// Adafruit_MPU6050 mpu;
// DHT dht(DHTPIN, DHTTYPE);

// unsigned long lastSensorReadTime = 0;
// unsigned long lastDHTReadTime = 0;
// unsigned long lastDisplayUpdateTime = 0;
// const unsigned long sensorReadInterval = 200;     // Define 5hz interval 200ms
// const unsigned long dhtReadInterval = 2000;       // Define 0.5hz interval 2s
// const unsigned long displayUpdateInterval = 2000; // LCD Display Update Interval

// int displayIndex = 0;

// void kirimDataKeServer(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity);

// void connectToWiFi()
// {
//     display.clearDisplay();
//     display.setTextSize(1);
//     display.setTextColor(SSD1306_WHITE);
//     display.setCursor(0, 0);

//     Serial.println("Connecting to DTEO-VOKASI ... ");
//     display.println("Connecting to");
//     display.println("\nDTEO-VOKASI ...");
//     display.display();
//     delay(1500);

//     WiFi.begin("DTEO-VOKASI", "TEO123456");

//     unsigned long startTime = millis();
//     while (WiFi.status() != WL_CONNECTED)
//     {
//         if (millis() - startTime > 2000) // Re-connect 2s
//         {
//             //* DEBUG */
//             // Serial.println("Failed connect WiFi");
//             display.clearDisplay();
//             display.setCursor(0, 0);
//             display.println("WiFi Fail To Connect !!!");
//             display.display();
//             return;
//         }
//         delay(500);
//     }

//     //* DEBUG */
//     // Serial.println("WiFi Connected !!!");
//     // Serial.print("IP Address: ");
//     // Serial.println(WiFi.localIP());

//     display.clearDisplay();
//     display.setCursor(0, 0);
//     display.println("WiFi Connected !!!");
//     display.print("DTEO-VOKASI");
//     display.display();
//     delay(1500);
// }

// void setup()
// {
//     Serial.begin(115200);
//     Wire.begin(SDA, SCL);

//     // Reset Pin Configuration
//     pinMode(RESET_BUTTON_PIN, INPUT);

//     // Floor Button Configuration
//     pinMode(FLOOR_1, INPUT);
//     pinMode(FLOOR_2, INPUT);
//     pinMode(FLOOR_3, INPUT);
//     // Floor Button Indicator
//     pinMode(FLOOR_IND_1, OUTPUT);
//     pinMode(FLOOR_IND_2, OUTPUT);
//     pinMode(FLOOR_IND_3, OUTPUT);
//     digitalWrite(FLOOR_IND_1, LOW); // Normal mode Mati
//     digitalWrite(FLOOR_IND_2, LOW);
//     digitalWrite(FLOOR_IND_3, LOW);

//     if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
//     {
//         Serial.println(F("SSD1306 Gagal"));
//         for (;;)
//             ;
//     }

//     // if (!mpu.begin() && !adxl.begin()) {
//     //   Serial.println("MPU6050 & ADXL345 failed to init.");
//     // } else {
//     //   Serial.println("MPU6050 & ADXL345 connect.");
//     // }

//     //** ERROR HANDLER */
//     adxl.begin(ADXL345_ADDRESS);
//     mpu.begin(MPU6050_ADDRESS);
//     dht.begin();
//     if (!mpu.begin() && !adxl.begin())
//     {
//         Serial.println("MPU6050 Not Connected");
//         Serial.println("ADXL345 Not Connected");
//     }
//     else
//     {
//         Serial.println("MPU6050 Connected");
//         Serial.println("ADXL Connected");
//     }

//     connectToWiFi();
// }

// void readSensors(float &gyroX, float &gyroY, float &gyroZ, float &accelX, float &accelY, float &accelZ, float &strainValue)
// {
//     sensors_event_t a, g, temp;
//     mpu.getEvent(&a, &g, &temp);
//     gyroX = g.gyro.x;
//     gyroY = g.gyro.y;
//     gyroZ = g.gyro.z;
//     accelX = a.acceleration.x;
//     accelY = a.acceleration.y;
//     accelZ = a.acceleration.z;

//     int rawValue = analogRead(STRAIN_GAUGE_PIN);
//     strainValue = rawValue * (100.0 / 1023.0);
// }

// void readDHT(float &temperature, float &humidity)
// {
//     temperature = dht.readTemperature();
//     humidity = dht.readHumidity();

//     //** ERROR HANDLER */
//     if (isnan(temperature) || isnan(humidity))
//     {
//         Serial.println("Gagal membaca data dari sensor DHT.");
//         // } else {
//         //     Serial.print("Suhu: ");
//         //     Serial.print(temperature);
//         //     Serial.print("°C, Kelembaban: ");
//         //     Serial.print(humidity);
//         //     Serial.println("%");
//         // }
//     }
// }

// void kirimDataKeServer(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity) // int current Floor saya hapus
// {
//     // Print sensor values to Serial
//     // Serial.println("Data yang dikirim ke server:");
//     // Serial.print("Floor: ");
//     // Serial.println(currentFloor);
//     // Serial.print("Gyro X: ");
//     // Serial.println(gyroX);
//     // Serial.print("Gyro Y: ");
//     // Serial.println(gyroY);
//     // Serial.print("Gyro Z: ");
//     // Serial.println(gyroZ);
//     // Serial.print("Accel X: ");
//     // Serial.println(accelX);
//     // Serial.print("Accel Y: ");
//     // Serial.println(accelY);
//     // Serial.print("Accel Z: ");
//     // Serial.println(accelZ);
//     // Serial.print("Strain Value: ");
//     // Serial.println(strainValue);
//     // Serial.print("Temperature: ");
//     // Serial.println(temperature);
//     // Serial.print("Humidity: ");
//     // Serial.println(humidity);
//     // Serial.println();

//     HTTPClient http;
//     WiFiClient client;
//     String postData;

//     // Construct POST data string
//     postData = "humidity=" + String(humidity) +
//                "&temperature=" + String(temperature) +
//                "&accelX=" + String(accelX) +
//                "&accelY=" + String(accelY) +
//                "&accelZ=" + String(accelZ) +
//                "&gyroX=" + String(gyroX) +
//                "&gyroY=" + String(gyroY) +
//                "&gyroZ=" + String(gyroZ) +
//                "&strainValue=" + String(strainValue);
//     //  "&lantai=" + String(lastLantai);

//     http.begin(client, "http://10.17.38.172/shmsv2_2/sensor.php");
//     http.addHeader("Content-Type", "application/x-www-form-urlencoded");

//     int httpCode = http.POST(postData); // request
//     String payload = http.getString();  // payload

//     Serial.println("HTTP Response code: " + String(httpCode));
//     Serial.println("Server response: " + payload);

//     http.end();
// }

// void updateDisplay(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity)
// {
//     display.clearDisplay();
//     display.setTextSize(1);
//     display.setTextColor(SSD1306_WHITE);
//     display.setCursor(0, 0);

//     switch (displayIndex)
//     {
//     case 0:
//         display.printf("Gyro X: %.2f deg/s", gyroX);
//         display.printf("\nGyro Y: %.2f deg/s", gyroY);
//         display.printf("\nGyro Z: %.2f deg/s", gyroZ);
//         break;
//     case 1:
//         display.printf("Accel X: %.2f m/s^2", accelX);
//         display.printf("\nAccel Y: %.2f m/s^2", accelY);
//         display.printf("\nAccel Z: %.2f m/s^2", accelZ);
//         break;
//     case 2:
//         display.printf("Strain  : %.2f N", strainValue);
//         display.printf("\nTemp    : %.2f C", temperature);
//         display.printf("\nHumidity: %.2f %%", humidity);
//         break;
//         // case 3:
//         //   display.printf("Floor: %d", currentFloor);
//         //   break;
//     }

//     display.display();
//     displayIndex = (displayIndex + 1) % 4;
// }

// void resetSensors(float &gyroX, float &gyroY, float &gyroZ, float &accelX, float &accelY, float &accelZ, float &strainValue, float &temperature, float &humidity)
// {
//     gyroX = gyroY = gyroZ = accelX = accelY = accelZ = strainValue = temperature = humidity = 0;
//     display.clearDisplay();
//     display.display();

//     /*Debugging Device Calibration*/
//     Serial.println(gyroX);
//     Serial.println(gyroY);
//     Serial.println(gyroZ);
//     Serial.println(accelX);
//     Serial.println(accelY);
//     Serial.println(accelZ);
//     Serial.println(strainValue);
//     Serial.println(temperature);
//     Serial.println(humidity);
// }

// void loop()
// {
//     static float gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue;
//     static float temperature = 0, humidity = 0;
//     unsigned long currentMillis = millis();

//     if (digitalRead(RESET_BUTTON_PIN) != HIGH)
//     {
//         resetSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
//     }

//     if (WiFi.status() != WL_CONNECTED)
//     {
//         Serial.println("Reconnect WiFi ...");
//         connectToWiFi();
//     }

//     stateBtn1 = digitalRead(FLOOR_1);
//     stateBtn2 = digitalRead(FLOOR_2);
//     stateBtn3 = digitalRead(FLOOR_3);

//     if (stateBtn1 == HIGH && lastStateBtn1 == LOW)
//     {
//         lastLantai = 1;
//         digitalWrite(FLOOR_IND_1, HIGH);
//         digitalWrite(FLOOR_IND_2, LOW);
//         digitalWrite(FLOOR_IND_3, LOW);
//     }
//     else if (stateBtn2 == HIGH && lastStateBtn2 == LOW)
//     {
//         lastLantai = 2;
//         digitalWrite(FLOOR_IND_1, LOW);
//         digitalWrite(FLOOR_IND_2, HIGH);
//         digitalWrite(FLOOR_IND_3, LOW);
//     }
//     else if (stateBtn3 == HIGH && lastStateBtn3 == LOW)
//     {
//         lastLantai = 3;
//         digitalWrite(FLOOR_IND_1, LOW);
//         digitalWrite(FLOOR_IND_2, LOW);
//         digitalWrite(FLOOR_IND_3, HIGH);
//     }

//     lastStateBtn1 = stateBtn1;
//     lastStateBtn2 = stateBtn2;
//     lastStateBtn3 = stateBtn3;

//     if (lastLantai < 1 || lastLantai > 3)
//     {
//         Serial.println("Error reading");
//     }

//     if (currentMillis - lastDHTReadTime >= 2000)
//     {
//         lastDHTReadTime = currentMillis;
//         readDHT(temperature, humidity);

//         //======================Serial Monitor DEBUGGING Only
//         // Serial.print("Temperature: ");
//         // Serial.print(temperature);
//         // Serial.print("\t Humidity: ");
//         // Serial.println(humidity);
//     }

//     // (5 Hz)
//     if (currentMillis - lastSensorReadTime >= 200)
//     {
//         lastSensorReadTime = currentMillis;
//         readSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue);

//         kirimDataKeServer(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity); // lastLantai saya hapus

//         //==============Serial Monitor DEBUGGING ONLY
//         // Serial.print("\n");
//         // Serial.print("Lantai: ");
//         // Serial.println(lastLantai);
//         // Serial.print("Gyro X: ");
//         // Serial.print(gyroX);
//         // Serial.print("\t Gyro Y: ");
//         // Serial.print(gyroY);
//         // Serial.print("\t Gyro Z: ");
//         // Serial.println(gyroZ);
//         // Serial.print("Accel X: ");
//         // Serial.print(accelX);
//         // Serial.print("\t Accel Y: ");
//         // Serial.print(accelY);
//         // Serial.print("\t Accel Z: ");
//         // Serial.println(accelZ);
//         // Serial.print("Strain Value: ");
//         // Serial.println(strainValue);
//     }

//     // * LCD Display *//
//     if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
//     {
//         lastDisplayUpdateTime = currentMillis;
//         updateDisplay(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity); // int currentFloor saya hapus
//     }
// }

// // void loop()
// // {
// //   if (WiFi.status() != WL_CONNECTED)
// //   {
// //     Serial.println("Reconnect WiFi ...");
// //     connectToWiFi();
// //   }

// //   // Cek state tombol lantai
// //   stateBtn1 = digitalRead(FLOOR_1);
// //   stateBtn2 = digitalRead(FLOOR_2);
// //   stateBtn3 = digitalRead(FLOOR_3);

// //   // Kondisi jika tombol lantai ditekan
// //   if (stateBtn1 == HIGH && lastStateBtn1 == LOW)
// //   {
// //     lastLantai = 1;
// //     digitalWrite(FLOOR_IND_1, HIGH);
// //     digitalWrite(FLOOR_IND_2, LOW);
// //     digitalWrite(FLOOR_IND_3, LOW);
// //   }
// //   else if (stateBtn2 == HIGH && lastStateBtn2 == LOW)
// //   {
// //     lastLantai = 2;
// //     digitalWrite(FLOOR_IND_1, LOW);
// //     digitalWrite(FLOOR_IND_2, HIGH);
// //     digitalWrite(FLOOR_IND_3, LOW);
// //   }
// //   else if (stateBtn3 == HIGH && lastStateBtn3 == LOW)
// //   {
// //     lastLantai = 3;
// //     digitalWrite(FLOOR_IND_1, LOW);
// //     digitalWrite(FLOOR_IND_2, LOW);
// //     digitalWrite(FLOOR_IND_3, HIGH);
// //   }

// //   lastStateBtn1 = stateBtn1;
// //   lastStateBtn2 = stateBtn2;
// //   lastStateBtn3 = stateBtn3;

// //   if (lastLantai < 1 || lastLantai > 3)
// //   {
// //     Serial.println("Error reading");
// //   }

// //   static float gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity;
// //   unsigned long currentMillis = millis();
// //   if (digitalRead(RESET_BUTTON_PIN) == LOW)
// //   {
// //     resetSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
// //   }
// //   else
// //   {
// //     // Update sensor ADXL, MPU, dan strain gauge (5 Hz)
// //     if (currentMillis - lastSensorReadTime >= sensorReadInterval)
// //     {
// //       lastSensorReadTime = currentMillis;
// //       readSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue);

// //       // Serial Monitor
// //       Serial.print("\n");
// //       Serial.print("Lantai: ");
// //       Serial.println(lastLantai);
// //       Serial.print("Gyro X: ");
// //       Serial.print(gyroX);
// //       Serial.print("\t Gyro Y: ");
// //       Serial.print(gyroY);
// //       Serial.print("\t Gyro Z: ");
// //       Serial.println(gyroZ);
// //       Serial.print("Accel X: ");
// //       Serial.print(accelX);
// //       Serial.print("\t Accel Y: ");
// //       Serial.print(accelY);
// //       Serial.print("\t Accel Z: ");
// //       Serial.println(accelZ);
// //       Serial.print("Strain Value: ");
// //       Serial.println(strainValue);
// //     }

// //     // Update DHT Sensor (0.5 Hz)
// //     if (currentMillis - lastDHTReadTime >= dhtReadInterval)
// //     {
// //       lastDHTReadTime = currentMillis;
// //       readDHT(temperature, humidity);

// //       // Serial Monitor
// //       Serial.print("Temperature: ");
// //       Serial.print(temperature);
// //       Serial.print("\t Humidity: ");
// //       Serial.println(humidity);
// //       Serial.print("\n");

// //       // Kirim data ke server
// //       kirimDataKeServer(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity, lastLantai);
// //     }

// //     // Update OLED Display (2 Hz)
// //     if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
// //     {
// //       lastDisplayUpdateTime = currentMillis;
// //       updateDisplay(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity, lastLantai);
// //     }
// //   }
// // }

// //=====================FIX DB SENDER=============================
// // ===============================================================

// // //------DEBUG USB TYPE C ------------
// // // #include <Arduino.h>
// // // #include <Adafruit_I2CDevice.h>
// // // #include <SPI.h>

// // #include <Arduino.h>
// // #include <Adafruit_ADXL345_U.h>
// // #include <Adafruit_MPU6050.h>
// // #include <DHT.h>
// // #include <Wire.h>
// // #include <WiFi.h>
// // #include <HTTPClient.h>
// // #include <Adafruit_SSD1306.h>

// // // Define PIN DHT22
// // #define DHTPIN 1
// // #define DHTTYPE DHT22

// // // Define MPU and ADXL
// // #define MPU6050_ADDRESS 0x68
// // #define ADXL345_ADDRESS 0x53
// // #define SDA 8
// // #define SCL 9

// // // Define LCD output
// // #define OLED_RESET 7 // RESET LCD output
// // #define SCREEN_WIDTH 128
// // #define SCREEN_HEIGHT 32
// // #define SSD1306_I2C_ADDRESS 0x3C

// // // Define Strain Gauge and Reset
// // #define STRAIN_GAUGE_PIN 20
// // #define RESET_BUTTON_PIN 2 // RESET All Instruments

// // Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// // Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
// // Adafruit_MPU6050 mpu;
// // DHT dht(DHTPIN, DHTTYPE);

// // unsigned long lastSensorReadTime = 0;
// // unsigned long lastDHTReadTime = 0;
// // unsigned long lastDisplayUpdateTime = 0;
// // const unsigned long sensorReadInterval = 200;     // Define 5hz interval
// // const unsigned long dhtReadInterval = 2000;       // Define 0.5hz interval
// // const unsigned long displayUpdateInterval = 2000; // LCD Display Update Interval

// // int displayIndex = 0;

// // void connectToWiFi()
// // {
// //   display.clearDisplay();
// //   display.setTextSize(1);
// //   display.setTextColor(SSD1306_WHITE);
// //   display.setCursor(0, 0);

// //   Serial.println("Connecting to DTEO-VOKASI ... ");
// //   display.println("Connecting to");
// //   display.println("\nDTEO-VOKASI ...");
// //   display.display();
// //   delay(1500);

// //   WiFi.begin("DTEO-VOKASI", "TEO123456");

// //   unsigned long startTime = millis();
// //   while (WiFi.status() != WL_CONNECTED)
// //   {
// //     if (millis() - startTime > 10000) // Re-connect 10s
// //     {
// //       Serial.println("Failed connect WiFi");
// //       display.clearDisplay();
// //       display.setCursor(0, 0);
// //       display.println("WiFi Fail To Connect !!!");
// //       display.display();
// //       return;
// //     }
// //     delay(500);
// //   }

// //   Serial.println("WiFi Connected !!!");
// //   Serial.print("IP Address: ");
// //   Serial.println(WiFi.localIP());

// //   display.clearDisplay();
// //   display.setCursor(0, 0);
// //   display.println("WiFi Connected !!!");
// //   display.print("DTEO-VOKASI");
// //   display.display();
// //   delay(1500);
// // }

// // void setup()
// // {
// //   Serial.begin(115200);
// //   Wire.begin(SDA, SCL);

// //   pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

// //   if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
// //   {
// //     Serial.println(F("SSD1306 Gagal"));
// //     for (;;)
// //       ;
// //   }
// //   adxl.begin(ADXL345_ADDRESS);
// //   mpu.begin(MPU6050_ADDRESS);
// //   dht.begin();
// //   if (!mpu.begin() && !adxl.begin())
// //   {
// //     Serial.println("MPU6050 Not Connected");
// //     Serial.println("ADXL345 Not Connected");
// //   }
// //   else
// //   {
// //     Serial.println("MPU6050 Connected");
// //     Serial.println("ADXL Connected");
// //   }

// //   connectToWiFi();
// // }

// // void readSensors(float &gyroX, float &gyroY, float &gyroZ, float &accelX, float &accelY, float &accelZ, float &strainValue)
// // {
// //   sensors_event_t a, g, temp;
// //   mpu.getEvent(&a, &g, &temp);
// //   gyroX = g.gyro.x;
// //   gyroY = g.gyro.y;
// //   gyroZ = g.gyro.z;
// //   accelX = a.acceleration.x;
// //   accelY = a.acceleration.y;
// //   accelZ = a.acceleration.z;

// //   int rawValue = analogRead(STRAIN_GAUGE_PIN);
// //   strainValue = rawValue * (100.0 / 1023.0);
// // }

// // void readDHT(float &temperature, float &humidity)
// // {
// //   temperature = dht.readTemperature();
// //   humidity = dht.readHumidity();
// // }

// // void sendData(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity)
// // {
// //   if (WiFi.status() == WL_CONNECTED)
// //   {
// //     HTTPClient http;
// //     http.begin("http://10.17.38.92/WebsiteMonitoring/SHMS");
// //     http.addHeader("Content-Type", "application/json");

// //     char jsonData[256];
// //     snprintf(jsonData, sizeof(jsonData), "{\"gyroX\": %.2f, \"gyroY\": %.2f, \"gyroZ\": %.2f, \"accelX\": %.2f, \"accelY\": %.2f, \"accelZ\": %.2f, \"strain\": %.2f, \"temperature\": %.2f, \"humidity\": %.2f}",
// //              gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);

// //     int httpResponseCode = http.POST(jsonData);
// //     http.end();
// //   }
// // }

// // void updateDisplay(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity)
// // {
// //   display.clearDisplay();
// //   display.setTextSize(1);
// //   display.setTextColor(SSD1306_WHITE);
// //   display.setCursor(0, 0);

// //   switch (displayIndex)
// //   {
// //   case 0:
// //     display.printf("Gyro X: %.2f deg/s", gyroX);
// //     display.printf("\nGyro Y: %.2f deg/s", gyroY);
// //     display.printf("\nGyro Z: %.2f deg/s", gyroZ);
// //     break;
// //   case 1:
// //     display.printf("Accel X: %.2f m/s^2", accelX);
// //     display.printf("\nAccel Y: %.2f m/s^2", accelY);
// //     display.printf("\nAccel Z: %.2f m/s^2", accelZ);
// //     break;
// //   case 2:
// //     display.printf("Strain  : %.2f N", strainValue);
// //     display.printf("\nTemp    : %.2f C", temperature);
// //     display.printf("\nHumidity: %.2f %", humidity);
// //     break;
// //   }

// //   display.display();
// //   displayIndex = (displayIndex + 1) % 3;
// // }

// // void resetSensors(float &gyroX, float &gyroY, float &gyroZ, float &accelX, float &accelY, float &accelZ, float &strainValue, float &temperature, float &humidity)
// // {
// //   gyroX = gyroY = gyroZ = accelX = accelY = accelZ = strainValue = temperature = humidity = 0;
// //   display.clearDisplay();
// //   display.display();
// // }

// // void loop()
// // {
// //   if (WiFi.status() != WL_CONNECTED)
// //   {
// //     Serial.println("Reconnect WiFi ...");
// //     connectToWiFi();
// //   }

// //   static float gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity;
// //   unsigned long currentMillis = millis();
// //   if (digitalRead(RESET_BUTTON_PIN) == LOW)
// //   {
// //     resetSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
// //   }
// //   else
// //   {
// //     // Update sensor ADXL, MPU, dan strain gauge (5 Hz)
// //     if (currentMillis - lastSensorReadTime >= 200)
// //     {
// //       lastSensorReadTime = currentMillis;
// //       readSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue);

// //       // Serial Monitor
// //       Serial.print("Gyro X: ");
// //       Serial.print(gyroX);
// //       Serial.print(" deg/s, ");
// //       Serial.print("Gyro Y: ");
// //       Serial.print(gyroY);
// //       Serial.print(" deg/s, ");
// //       Serial.print("Gyro Z: ");
// //       Serial.print(gyroZ);
// //       Serial.println(" deg/s");

// //       Serial.print("Accel X: ");
// //       Serial.print(accelX);
// //       Serial.print(" m/s^2, ");
// //       Serial.print("Accel Y: ");
// //       Serial.print(accelY);
// //       Serial.print(" m/s^2, ");
// //       Serial.print("Accel Z: ");
// //       Serial.println(accelZ);
// //       Serial.println(" m/s^2");

// //       Serial.print("Strain Gauge: ");
// //       Serial.println(strainValue);
// //       Serial.println(" N");
// //       Serial.println();
// //     }

// //     // Update sensor DHT (0.5 Hz)
// //     if (currentMillis - lastDHTReadTime >= 2000)
// //     {
// //       lastDHTReadTime = currentMillis;
// //       readDHT(temperature, humidity);
// //       Serial.print("Temperature: ");
// //       Serial.print(temperature);
// //       Serial.println(" °C");
// //       Serial.print("Humidity: ");
// //       Serial.println(humidity);
// //       Serial.println(" %");
// //       Serial.println();
// //     }
// //     if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
// //     {
// //       lastDisplayUpdateTime = currentMillis;
// //       updateDisplay(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
// //     }

// //     sendData(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
// //   }
// // }

// // Funtion Recommended

// // if (!isnan(temperature) && !isnan(humidity)) {
// //     Serial.print("Temperature: ");
// //     Serial.print(temperature);
// //     Serial.println(" °C");
// //     Serial.print("Humidity: ");
// //     Serial.println(humidity);
// //     Serial.println(" %");
// // } else {
// //     Serial.println("Failed to read from DHT sensor!");
// // }

// // void setup(){
// //   Serial.begin(115200);
// //   // put your setup code here, to run once:
// // }

// // void loop(){
// //   // put your main code here, to run repeatedly:
// //   Serial.println("test");
// // }
// //------ DEBUG TYPE C END ------------

// //------ PROGRAM BIASA NO FREERTOS ------------
// // #include <Arduino.h>
// // #include <Adafruit_ADXL345_U.h>
// // #include <Adafruit_MPU6050.h>
// // #include <DHT.h>
// // #include <HX711.h>
// // #include <Wire.h>
// // #include <WiFi.h>
// // #include <HTTPClient.h>
// // #include <Adafruit_SSD1306.h>
// // #include <Adafruit_GFX.h>
// // #include <Adafruit_I2CDevice.h>
// // #include <SPI.h>

// // #define DHTPIN 6 // Pin DHT22
// // #define DHTTYPE DHT22
// // // #define STRAIN_GAUGE_DOUT 7 // Pin data untuk HX711
// // // #define STRAIN_GAUGE_SCK 8  // Pin clock untuk HX711
// // #define MPU6050_ADDRESS 0x68
// // #define ADXL345_ADDRESS 0x53
// // #define SDA 8
// // #define SCL 9

// // #define RESET_MPU 2
// // #define RESET_ADXL345 3
// // #define RESET_OLED 4
// // #define INDICATOR 5

// // #define SCREEN_WIDTH 128
// // #define SCREEN_HEIGHT 64
// // #define SSD1306_I2C_ADDRESS 0x3C // I2C address SSD1306
// // #define OLED_RESET 7

// // #define STRAIN_GAUGE_PIN 10 // Pin Strain Gauge Module Y3 BF350-3AA

// // Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// // class SensorManager
// // {
// // public:
// //   SensorManager() : dht(DHTPIN, DHTTYPE)
// //   {
// //     adxl.begin(ADXL345_ADDRESS);
// //     mpu.begin(MPU6050_ADDRESS);
// //     // strainGauge.begin(STRAIN_GAUGE_DOUT, STRAIN_GAUGE_SCK);
// //     dht.begin();
// //   }

// //   void readSensors()
// //   {
// //     readMPU6050();
// //     readADXL345();
// //     readStrainGauge();
// //     readDHT22();
// //   }

// //   float getStrainValue() const
// //   {
// //     return strainValue;
// //   }

// //   float getTemperature()
// //   {
// //     return temperature;
// //   }

// //   float getHumidity()
// //   {
// //     return humidity;
// //   }

// //   float getGyroX() { return gyroX; }
// //   float getGyroY() { return gyroY; }
// //   float getGyroZ() { return gyroZ; }

// //   float getAccelX() { return accelX; }
// //   float getAccelY() { return accelY; }
// //   float getAccelZ() { return accelZ; }

// // private:
// //   Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
// //   Adafruit_MPU6050 mpu;
// //   DHT dht;
// //   HX711 strainGauge;

// //   float strainValue;
// //   float temperature;
// //   float humidity;

// //   float gyroX, gyroY, gyroZ;
// //   float accelX, accelY, accelZ;

// //   void readMPU6050()
// //   {
// //     sensors_event_t a, g, temp;
// //     mpu.getEvent(&a, &g, &temp);
// //     gyroX = g.gyro.x;
// //     gyroY = g.gyro.y;
// //     gyroZ = g.gyro.z;
// //   }

// //   void readADXL345()
// //   {
// //     sensors_event_t event;
// //     adxl.getEvent(&event);
// //     accelX = event.acceleration.x;
// //     accelY = event.acceleration.y;
// //     accelZ = event.acceleration.z;
// //   }

// //   void readStrainGauge()
// //   {
// //     int hodnota = analogRead(STRAIN_GAUGE_PIN);
// //     strainValue = map(hodnota, 0, 700, 0, 100);
// //   }

// // public:
// //   void readDHT22()
// //   {
// //     temperature = dht.readTemperature();
// //     humidity = dht.readHumidity();

// //     if (isnan(temperature) || isnan(humidity))
// //     {
// //       temperature = 0; // NAN
// //       humidity = 0;    // NAN
// //     }
// //   }
// // };

// // class WiFiManager
// // {
// // public:
// //   WiFiManager(const char *ssid, const char *password)
// //   {
// //     WiFi.begin(ssid, password);
// //     while (WiFi.status() != WL_CONNECTED)
// //     {
// //       delay(1000);
// //     }
// //   }

// //   void sendData(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strain, float temperature, float humidity)
// //   {
// //     if (WiFi.status() != WL_CONNECTED)
// //     {
// //       reconnectWiFi();
// //     }

// //     if (WiFi.status() == WL_CONNECTED)
// //     {
// //       HTTPClient http;
// //       String url = "http://10.17.38.92/WebsiteMonitoring/SHMS";
// //       http.begin(url);

// //       http.addHeader("Content-Type", "application/json");
// //       String jsonData = "{\"gyroX\": " + String(gyroX) + ", \"gyroY\": " + String(gyroY) + ", \"gyroZ\": " + String(gyroZ) +
// //                         ", \"accelX\": " + String(accelX) + ", \"accelY\": " + String(accelY) + ", \"accelZ\": " + String(accelZ) +
// //                         ", \"strain\": " + String(strain) + ", \"temperature\": " + String(temperature) + ", \"humidity\": " + String(humidity) + "}";

// //       int httpResponseCode = http.POST(jsonData);
// //       if (httpResponseCode > 0)
// //       {
// //         Serial.printf("Data terkirim: %s\n", jsonData.c_str());
// //       }
// //       else
// //       {
// //         Serial.printf("Gagal mengirim data. Kode Respon: %d\n", httpResponseCode);
// //       }

// //       http.end();
// //     }
// //   }

// //   void reconnectWiFi()
// //   {
// //     while (WiFi.status() != WL_CONNECTED)
// //     {
// //       Serial.println("Mencoba menyambung kembali ke WiFi...");
// //       WiFi.reconnect();
// //       delay(1000); // reconnect setiap 1 detik
// //     }
// //     Serial.println("Terhubung kembali ke WiFi!");
// //   }
// // };

// // SensorManager sensorManager;
// // WiFiManager wifiManager("DTEO-VOKASI", "TEO123456");

// // void setup()
// // {
// //   Wire.begin(SDA, SCL);
// //   display.begin(SSD1306_I2C_ADDRESS, OLED_RESET);
// //   display.clearDisplay();

// //   Serial.begin(115200);

// //   pinMode(RESET_MPU, OUTPUT);
// //   pinMode(RESET_ADXL345, OUTPUT);
// //   pinMode(RESET_OLED, OUTPUT);
// //   pinMode(INDICATOR, OUTPUT);

// //   digitalWrite(RESET_MPU, LOW);
// //   delay(100);
// //   digitalWrite(RESET_MPU, HIGH);

// //   digitalWrite(RESET_ADXL345, LOW);
// //   delay(100);
// //   digitalWrite(RESET_ADXL345, HIGH);

// //   digitalWrite(RESET_OLED, LOW);
// //   delay(100);
// //   digitalWrite(RESET_OLED, HIGH);
// // }

// // void loop()
// // {
// //   static unsigned long lastDHT22 = 0;
// //   static unsigned long lastSensorRead = 0;
// //   static unsigned long lastDisplayUpdate = 0;

// //   if (millis() - lastSensorRead >= 200) // 5hz
// //   {
// //     sensorManager.readSensors();
// //     lastSensorRead = millis();

// //     // Cetak data sensor ke Serial
// //     Serial.print("Gyro X: ");
// //     Serial.println(sensorManager.getGyroX());
// //     Serial.print("Gyro Y: ");
// //     Serial.println(sensorManager.getGyroY());
// //     Serial.print("Gyro Z: ");
// //     Serial.println(sensorManager.getGyroZ());
// //     Serial.print("Accel X: ");
// //     Serial.println(sensorManager.getAccelX());
// //     Serial.print("Accel Y: ");
// //     Serial.println(sensorManager.getAccelY());
// //     Serial.print("Accel Z: ");
// //     Serial.println(sensorManager.getAccelZ());
// //     Serial.print("Strain: ");
// //     Serial.println(sensorManager.getStrainValue());
// //     Serial.print("Temp: ");
// //     Serial.println(sensorManager.getTemperature());
// //     Serial.print("Humidity: ");
// //     Serial.println(sensorManager.getHumidity());
// //   }

// //   if (millis() - lastDHT22 >= 2000) // 0.5hz
// //   {
// //     sensorManager.readDHT22();
// //     lastDHT22 = millis();
// //   }

// //   wifiManager.sendData(sensorManager.getGyroX(), sensorManager.getGyroY(), sensorManager.getGyroZ(),
// //                        sensorManager.getAccelX(), sensorManager.getAccelY(), sensorManager.getAccelZ(),
// //                        sensorManager.getStrainValue(), sensorManager.getTemperature(), sensorManager.getHumidity());

// //   if (millis() - lastDisplayUpdate >= 2000) // 5s
// //   {
// //     display.clearDisplay();
// //     display.setTextSize(1);
// //     display.setTextColor(SSD1306_WHITE);
// //     display.setCursor(0, 0);
// //     display.print("Gyro X: ");
// //     display.println(sensorManager.getGyroX());
// //     display.print("Gyro Y: ");
// //     display.println(sensorManager.getGyroY());
// //     display.print("Gyro Z: ");
// //     display.println(sensorManager.getGyroZ());
// //     display.print("Accel X: ");
// //     display.println(sensorManager.getAccelX());
// //     display.print("Accel Y: ");
// //     display.println(sensorManager.getAccelY());
// //     display.print("Accel Z: ");
// //     display.println(sensorManager.getAccelZ());
// //     display.print("Strain: ");
// //     display.println(sensorManager.getStrainValue());
// //     display.print("Temp: ");
// //     display.println(sensorManager.getTemperature());
// //     display.print("Humidity: ");
// //     display.println(sensorManager.getHumidity());
// //     display.display();

// //     lastDisplayUpdate = millis();
// //   }
// // }
// //------ PROGRAM BIASA END ------------

// // // -----FreeRTOS----------- //
// // #include <Arduino.h>
// // #include <FreeRTOS.h>
// // #include <Adafruit_ADXL345_U.h>
// // #include <Adafruit_MPU6050.h>
// // #include <DHT.h>
// // #include <HX711.h>
// // #include <Wire.h>
// // #include <WiFi.h>
// // #include <HTTPClient.h>
// // #include <Adafruit_SSD1306.h>
// // #include <Adafruit_GFX.h>
// // #include <Adafruit_I2CDevice.h>
// // #include <SPI.h>

// // #define DHTPIN 6            // Pin DHT22
// // #define DHTTYPE DHT22

// // // #define STRAIN_GAUGE_DOUT 7 // Pin data HX711 -> Not Used
// // // #define STRAIN_GAUGE_SCK 8  // Pin clock HX711 -> Not Used
// // #define STRAIN_GAUGE_PIN 10 // Pin Strain Gauge Module Y3 BF350-3AA -> Used

// // #define MPU6050_ADDRESS 0x68
// // #define ADXL345_ADDRESS 0x53
// // #define SDA 8
// // #define SCL 9

// // #define RESET_MPU 2
// // #define RESET_ADXL345 3
// // #define RESET_OLED 4
// // #define OLED_RESET 7

// // #define INDICATOR 5

// // #define SCREEN_WIDTH 128
// // #define SCREEN_HEIGHT 64
// // #define SSD1306_I2C_ADDRESS 0x3C // I2C address SSD1306

// // Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// // class SensorManager
// // {
// // public:
// //   SensorManager() : dht(DHTPIN, DHTTYPE)
// //   {
// //     adxl.begin(ADXL345_ADDRESS);
// //     mpu.begin(MPU6050_ADDRESS);
// //     // strainGauge.begin(STRAIN_GAUGE_DOUT, STRAIN_GAUGE_SCK);
// //     dht.begin();
// //   }

// //   void readSensors()
// //   {
// //     readMPU6050();
// //     readADXL345();
// //     readStrainGauge();
// //   }

// //   void readDHT22()
// //   {
// //     temperature = dht.readTemperature();
// //     humidity = dht.readHumidity();

// //     if (isnan(temperature) || isnan(humidity))
// //     {
// //       temperature = 0; // NAN
// //       humidity = 0;    // NAN
// //     }
// //   }

// //   float getStrainValue() const
// //   {
// //     return strainValue;
// //   }

// //   float getTemperature()
// //   {
// //     return temperature;
// //   }

// //   float getHumidity()
// //   {
// //     return humidity;
// //   }

// //   float getGyroX() { return gyroX; }
// //   float getGyroY() { return gyroY; }
// //   float getGyroZ() { return gyroZ; }

// //   float getAccelX() { return accelX; }
// //   float getAccelY() { return accelY; }
// //   float getAccelZ() { return accelZ; }

// // private:
// //   Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
// //   Adafruit_MPU6050 mpu;
// //   DHT dht;
// //   HX711 strainGauge;

// //   float strainValue;
// //   float temperature;
// //   float humidity;

// //   float gyroX, gyroY, gyroZ;
// //   float accelX, accelY, accelZ;

// //   void readMPU6050()
// //   {
// //     sensors_event_t a, g, temp;
// //     mpu.getEvent(&a, &g, &temp);
// //     gyroX = g.gyro.x;
// //     gyroY = g.gyro.y;
// //     gyroZ = g.gyro.z;
// //   }

// //   void readADXL345()
// //   {
// //     sensors_event_t event;
// //     adxl.getEvent(&event);
// //     accelX = event.acceleration.x;
// //     accelY = event.acceleration.y;
// //     accelZ = event.acceleration.z;
// //   }

// //   void readStrainGauge()
// //   {
// //     int hodnota = analogRead(STRAIN_GAUGE_PIN);
// //     strainValue = map(hodnota, 0, 700, 0, 100);
// //   }
// // };

// // class WiFiManager
// // {
// // public:
// //   WiFiManager(const char *ssid, const char *password)
// //   {
// //     WiFi.begin(ssid, password);
// //     while (WiFi.status() != WL_CONNECTED)
// //     {
// //       vTaskDelay(1000);
// //     }
// //   }

// //   void sendData(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strain, float temperature, float humidity)
// //   {
// //     if (WiFi.status() != WL_CONNECTED)
// //     {
// //       reconnectWiFi();
// //     }

// //     if (WiFi.status() == WL_CONNECTED)
// //     {
// //       HTTPClient http;
// //       String url = "http://10.17.38.92/WebsiteMonitoring/SHMS";
// //       http.begin(url);

// //       http.addHeader("Content-Type", "application/json");
// //       String jsonData = "{\"gyroX\": " + String(gyroX) + ", \"gyroY\": " + String(gyroY) + ", \"gyroZ\": " + String(gyroZ) +
// //                         ", \"accelX\": " + String(accelX) + ", \"accelY\": " + String(accelY) + ", \"accelZ\": " + String(accelZ) +
// //                         ", \"strain\": " + String(strain) + ", \"temperature\": " + String(temperature) + ", \"humidity\": " + String(humidity) + "}";

// //       Serial.println("Mengirim data: " + jsonData);
// //       int httpResponseCode = http.POST(jsonData);
// //       if (httpResponseCode > 0)
// //       {
// //         Serial.printf("Data terkirim: %s\n", jsonData.c_str());
// //       }
// //       else
// //       {
// //         Serial.printf("Gagal mengirim data. Kode Respon: %d\n", httpResponseCode);
// //       }

// //       http.end();
// //     }
// //   }

// //   void reconnectWiFi()
// //   {
// //     while (WiFi.status() != WL_CONNECTED)
// //     {
// //       Serial.println("Mencoba menyambung kembali ke WiFi...");
// //       WiFi.reconnect();
// //       vTaskDelay(1000); // reconnect 1 detik
// //     }
// //     Serial.println("Terhubung kembali ke WiFi!");
// //   }
// // };

// // SensorManager sensorManager;
// // WiFiManager wifiManager("DTEO-VOKASI", "TEO123456");

// // void TaskReadSensors(void *pvParameters)
// // {
// //   for (;;)
// //   {
// //     sensorManager.readSensors();
// //     vTaskDelay(pdMS_TO_TICKS(200)); // 5 Hz
// //   }
// // }

// // void TaskReadDHT22(void *pvParameters)
// // {
// //   for (;;)
// //   {
// //     sensorManager.readDHT22();
// //     vTaskDelay(pdMS_TO_TICKS(2000)); // 0.5 Hz
// //   }
// // }

// // void TaskSendData(void *pvParameters)
// // {
// //   for (;;)
// //   {
// //     wifiManager.sendData(sensorManager.getGyroX(), sensorManager.getGyroY(), sensorManager.getGyroZ(),
// //                          sensorManager.getAccelX(), sensorManager.getAccelY(), sensorManager.getAccelZ(),
// //                          sensorManager.getStrainValue(), sensorManager.getTemperature(), sensorManager.getHumidity());
// //     vTaskDelay(pdMS_TO_TICKS(1000)); // Send 1s
// //   }
// // }

// // void TaskUpdateDisplay(void *pvParameters)
// // {
// //   for (;;)
// //   {
// //     display.clearDisplay();
// //     display.setTextSize(1);
// //     display.setTextColor(SSD1306_WHITE);
// //     display.setCursor(0, 0);
// //     display.print("Gyro X: ");
// //     display.println(sensorManager.getGyroX());
// //     display.print("Gyro Y: ");
// //     display.println(sensorManager.getGyroY());
// //     display.print("Gyro Z: ");
// //     display.println(sensorManager.getGyroZ());
// //     display.print("Accel X: ");
// //     display.println(sensorManager.getAccelX());
// //     display.print("Accel Y: ");
// //     display.println(sensorManager.getAccelY());
// //     display.print("Accel Z: ");
// //     display.println(sensorManager.getAccelZ());
// //     display.print("Strain: ");
// //     display.println(sensorManager.getStrainValue());
// //     display.print("Temp: ");
// //     display.println(sensorManager.getTemperature());
// //     display.print("Humidity: ");
// //     display.println(sensorManager.getHumidity());
// //     display.display();

// //     vTaskDelay(pdMS_TO_TICKS(1000)); // Refresh Display 1s
// //   }
// // }

// // void setup()
// // {
// //   Wire.begin(SDA, SCL);
// //   display.begin(SSD1306_I2C_ADDRESS, OLED_RESET);
// //   display.clearDisplay();

// //   Serial.begin(115200);

// //   pinMode(RESET_MPU, OUTPUT);
// //   pinMode(RESET_ADXL345, OUTPUT);
// //   pinMode(RESET_OLED, OUTPUT);
// //   pinMode(INDICATOR, OUTPUT);

// //   digitalWrite(RESET_MPU, LOW);
// //   vTaskDelay(100);
// //   digitalWrite(RESET_MPU, HIGH);

// //   digitalWrite(RESET_ADXL345, LOW);
// //   vTaskDelay(100);
// //   digitalWrite(RESET_ADXL345, HIGH);

// //   digitalWrite(RESET_OLED, LOW);
// //   vTaskDelay(100);
// //   digitalWrite(RESET_OLED, HIGH);

// //   // FreeRTOS
// //   xTaskCreate(TaskReadSensors, "TaskReadSensors", 2048, NULL, 1, NULL);
// //   xTaskCreate(TaskReadDHT22, "TaskReadDHT22", 2048, NULL, 1, NULL);
// //   xTaskCreate(TaskSendData, "TaskSendData", 2048, NULL, 1, NULL);
// //   xTaskCreate(TaskUpdateDisplay, "TaskUpdateDisplay", 2048, NULL, 1, NULL);
// // }

// // void loop()
// // {
// // }


// ************* KODE BISA 2***********************

// #include <Arduino.h>
// #include <Adafruit_ADXL345_U.h>
// #include <Adafruit_MPU6050.h>
// #include <DHT.h>
// #include <Wire.h>
// #include <WiFi.h>
// #include <HTTPClient.h>
// #include <Adafruit_SSD1306.h>

// // Define Floor and indicator
// #define FLOOR_1 37
// #define FLOOR_2 36
// #define FLOOR_3 35
// #define FLOOR_IND_1 38
// #define FLOOR_IND_2 39
// #define FLOOR_IND_3 40

// // Additional constants for DB access
// int lastLantai = 0;                                                 // Lantai terakhir ke db
// bool stateBtn1 = LOW, stateBtn2 = LOW, stateBtn3 = LOW;             // Status tombol
// bool lastStateBtn1 = LOW, lastStateBtn2 = LOW, lastStateBtn3 = LOW; // Status tombol sebelumnya

// float temperature = 0, humidity = 0;
// float temperatureState = temperature; // Handler last value
// float humidityState = humidity;       // Handler last value
// float humidityLast;
// float temperatureLast;

// // Define PIN DHT22
// #define DHTPIN 1
// #define DHTTYPE DHT22

// // Define MPU and ADXL
// #define MPU6050_ADDRESS 0x68
// #define ADXL345_ADDRESS 0x53
// #define SDA 8
// #define SCL 9

// // Define LCD output
// #define OLED_RESET 7 // RESET LCD output
// #define SCREEN_WIDTH 128
// #define SCREEN_HEIGHT 32
// #define SSD1306_I2C_ADDRESS 0x3C

// // Define Strain Gauge and Reset
// #define STRAIN_GAUGE_PIN 20
// #define RESET_BUTTON_PIN 2 // RESET All Instruments

// Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
// Adafruit_MPU6050 mpu;
// DHT dht(DHTPIN, DHTTYPE);

// unsigned long lastSensorReadTime = 0;
// unsigned long lastDHTReadTime = 0;
// unsigned long lastDisplayUpdateTime = 0;
// const unsigned long sensorReadInterval = 200;     // Define 5hz interval 200ms
// const unsigned long dhtReadInterval = 2000;       // Define 0.5hz interval 2s
// const unsigned long displayUpdateInterval = 2000; // LCD Display Update Interval

// int displayIndex = 0;
// int updateCount = 0;

// void kirimDataKeServer(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity);

// void connectToWiFi()
// {
//   display.clearDisplay();
//   display.setTextSize(1);
//   display.setTextColor(SSD1306_WHITE);
//   display.setCursor(0, 0);

//   Serial.println("Connecting to DTEO-VOKASI ... ");
//   display.println("Connecting to");
//   display.println("\nDTEO-VOKASI ...");
//   display.display();
//   delay(1500);

//   WiFi.begin("DTEO-VOKASI", "TEO123456");

//   unsigned long startTime = millis();
//   while (WiFi.status() != WL_CONNECTED)
//   {
//     if (millis() - startTime > 2000) // Re-connect 2s
//     {
//       //* DEBUG */
//       // Serial.println("Failed connect WiFi");
//       display.clearDisplay();
//       display.setCursor(0, 0);
//       display.println("WiFi Fail To Connect !!!");
//       display.display();
//       return;
//     }
//     delay(500);
//   }

//   // Debug WIFI
//   //  Serial.println("WiFi Connected !!!");
//   //  Serial.print("IP Address: ");
//   //  Serial.println(WiFi.localIP());

//   display.clearDisplay();
//   display.setCursor(0, 0);
//   display.println("WiFi Connected !!!");
//   display.print("DTEO-VOKASI");
//   display.display();
//   delay(1500);
// }

// void setup()
// {
//   Serial.begin(115200);
//   Wire.begin(SDA, SCL);

//   // Reset Pin Configuration
//   pinMode(RESET_BUTTON_PIN, INPUT);

//   // Floor Button Configuration
//   pinMode(FLOOR_1, INPUT);
//   pinMode(FLOOR_2, INPUT);
//   pinMode(FLOOR_3, INPUT);
//   // Floor Button Indicator
//   pinMode(FLOOR_IND_1, OUTPUT);
//   pinMode(FLOOR_IND_2, OUTPUT);
//   pinMode(FLOOR_IND_3, OUTPUT);
//   digitalWrite(FLOOR_IND_1, LOW); // Normal mode Mati
//   digitalWrite(FLOOR_IND_2, LOW);
//   digitalWrite(FLOOR_IND_3, LOW);

//   if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
//   {
//     Serial.println(F("SSD1306 Gagal"));
//     for (;;)
//       ;
//   }

//   // if (!mpu.begin() && !adxl.begin()) {
//   //   Serial.println("MPU6050 & ADXL345 failed to init.");
//   // } else {
//   //   Serial.println("MPU6050 & ADXL345 connect.");
//   // }

//   //** ERROR HANDLER */
//   adxl.begin(ADXL345_ADDRESS);
//   mpu.begin(MPU6050_ADDRESS);
//   dht.begin();
//   if (!mpu.begin() && !adxl.begin())
//   {
//     Serial.println("MPU6050 Not Connected");
//     Serial.println("ADXL345 Not Connected");
//   }
//   else
//   {
//     Serial.println("MPU6050 Connected");
//     Serial.println("ADXL Connected");
//   }

//   connectToWiFi();
// }

// void readSensors(float &gyroX, float &gyroY, float &gyroZ, float &accelX, float &accelY, float &accelZ, float &strainValue)
// {
//   sensors_event_t a, g, temp;
//   mpu.getEvent(&a, &g, &temp);
//   gyroX = g.gyro.x;
//   gyroY = g.gyro.y;
//   gyroZ = g.gyro.z;
//   accelX = a.acceleration.x;
//   accelY = a.acceleration.y;
//   accelZ = a.acceleration.z;

//   int rawValue = analogRead(STRAIN_GAUGE_PIN);
//   strainValue = rawValue * (100.0 / 1023.0);
// }

// void readDHT(float &temperature, float &humidity)
// {
//   temperature = dht.readTemperature();
//   humidity = dht.readHumidity();

//   //** ERROR HANDLER */
//   if (isnan(temperature) || isnan(humidity))
//   {
//     Serial.println("DHT Sensor Error: NaN value");
//   }
//   else
//   {
//     temperatureState = temperature;
//     humidityState = humidity;
//   }
// }

// void kirimDataKeServer(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity) // int current Floor saya hapus
// {
//   // Print sensor values to Serial
//   // Serial.println("Data yang dikirim ke server:");
//   // Serial.print("Floor: ");
//   // Serial.println(currentFloor);
//   // Serial.print("Gyro X: ");
//   // Serial.println(gyroX);
//   // Serial.print("Gyro Y: ");
//   // Serial.println(gyroY);
//   // Serial.print("Gyro Z: ");
//   // Serial.println(gyroZ);
//   // Serial.print("Accel X: ");
//   // Serial.println(accelX);
//   // Serial.print("Accel Y: ");
//   // Serial.println(accelY);
//   // Serial.print("Accel Z: ");
//   // Serial.println(accelZ);
//   // Serial.print("Strain Value: ");
//   // Serial.println(strainValue);
//   // Serial.print("Temperature: ");
//   // Serial.println(temperature);
//   // Serial.print("Humidity: ");
//   // Serial.println(humidity);
//   // Serial.println();

//   HTTPClient http;
//   WiFiClient client;
//   String postData;

//   // Construct POST data string
//   postData = "humidity=" + String(humidity) +
//              "&temperature=" + String(temperature) +
//              "&accelX=" + String(accelX) +
//              "&accelY=" + String(accelY) +
//              "&accelZ=" + String(accelZ) +
//              "&gyroX=" + String(gyroX) +
//              "&gyroY=" + String(gyroY) +
//              "&gyroZ=" + String(gyroZ) +
//              "&strainValue=" + String(strainValue);
//   //  "&lantai=" + String(lastLantai);

//   http.begin(client, "http://10.17.37.202/shmsv2_2/sensor.php"); // MODUL LAMA
//   // http.begin(client, "http://10.17.37.202/shmsv2_2/sensor.php2"); // MODUL BARU
//   http.addHeader("Content-Type", "application/x-www-form-urlencoded");

//   int httpCode = http.POST(postData); // request
//   String payload = http.getString();  // payload

//   Serial.println("HTTP Response code: " + String(httpCode));
//   Serial.println("Server response: " + payload);

//   http.end();
// }

// void updateDisplay(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity)
// {
//   if (updateCount < 3)
//   {
//     display.clearDisplay();
//     display.setTextSize(1);
//     display.setTextColor(SSD1306_WHITE);
//     display.setCursor(0, 0);

//     switch (displayIndex)
//     {
//     case 0:
//       display.printf("Gyro X: %.2f deg/s", gyroX);
//       display.printf("\nGyro Y: %.2f deg/s", gyroY);
//       display.printf("\nGyro Z: %.2f deg/s", gyroZ);
//       digitalWrite(FLOOR_IND_1, HIGH);
//       digitalWrite(FLOOR_IND_2, LOW);
//       digitalWrite(FLOOR_IND_3, LOW);
//       break;
//     case 1:
//       display.printf("Accel X: %.2f m/s^2", accelX);
//       display.printf("\nAccel Y: %.2f m/s^2", accelY);
//       display.printf("\nAccel Z: %.2f m/s^2", accelZ);
//       digitalWrite(FLOOR_IND_1, LOW);
//       digitalWrite(FLOOR_IND_2, HIGH);
//       digitalWrite(FLOOR_IND_3, LOW);
//       break;
//     case 2:
//       display.printf("Strain  : %.2f N", strainValue);
//       display.printf("\nTemp    : %.2f C", temperature);
//       display.printf("\nHumidity: %.2f %%", humidity);
//       digitalWrite(FLOOR_IND_1, LOW);
//       digitalWrite(FLOOR_IND_2, LOW);
//       digitalWrite(FLOOR_IND_3, HIGH);
//       break;
//       // case 3:
//       //   display.printf("Floor: %d", currentFloor);
//       //   break;
//     }

//     display.display();
//     displayIndex = (displayIndex + 1) % 3;
//   }
// }

// void resetSensors(float &gyroX, float &gyroY, float &gyroZ, float &accelX, float &accelY, float &accelZ, float &strainValue, float &temperature, float &humidity)
// {
//   gyroX = gyroY = gyroZ = accelX = accelY = accelZ = strainValue = temperature = humidity = 0;

//   display.clearDisplay();
//   // display.setTextSize(1);
//   // display.setTextColor(SSD1306_WHITE);
//   display.setCursor(0, 0);
//   display.printf("Reset Success");

//   Serial.println("Reset Success");

//   digitalWrite(FLOOR_IND_1, HIGH);
//   digitalWrite(FLOOR_IND_2, HIGH);
//   digitalWrite(FLOOR_IND_3, HIGH);
//   delay(500);

//   /*Debugging Device Calibration*/
//   // Serial.println(gyroX);
//   // Serial.println(gyroY);
//   // Serial.println(gyroZ);
//   // Serial.println(accelX);
//   // Serial.println(accelY);
//   // Serial.println(accelZ);
//   // Serial.println(strainValue);
//   // Serial.println(temperature);
//   // Serial.println(humidity);
// }

// void loop()
// {
//   static float gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue;
//   unsigned long currentMillis = millis();

//   if (digitalRead(RESET_BUTTON_PIN) == HIGH)
//   {
//     resetSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
//   }

//   if (WiFi.status() != WL_CONNECTED)
//   {
//     Serial.println("Reconnect WiFi ...");
//     connectToWiFi();
//   }

//   stateBtn1 = digitalRead(FLOOR_1);
//   stateBtn2 = digitalRead(FLOOR_2);
//   stateBtn3 = digitalRead(FLOOR_3);

//   /*
//     if (stateBtn1 == HIGH && lastStateBtn1 == LOW)
//     {
//       lastLantai = 1;
//       digitalWrite(FLOOR_IND_1, HIGH);
//       digitalWrite(FLOOR_IND_2, LOW);
//       digitalWrite(FLOOR_IND_3, LOW);
//     }
//     else if (stateBtn2 == HIGH && lastStateBtn2 == LOW)
//     {
//       lastLantai = 2;
//       digitalWrite(FLOOR_IND_1, LOW);
//       digitalWrite(FLOOR_IND_2, HIGH);
//       digitalWrite(FLOOR_IND_3, LOW);
//     }
//     else if (stateBtn3 == HIGH && lastStateBtn3 == LOW)
//     {
//       lastLantai = 3;
//       digitalWrite(FLOOR_IND_1, LOW);
//       digitalWrite(FLOOR_IND_2, LOW);
//       digitalWrite(FLOOR_IND_3, HIGH);
//     }

//   */

//   lastStateBtn1 = stateBtn1;
//   lastStateBtn2 = stateBtn2;
//   lastStateBtn3 = stateBtn3;

//   // if (lastLantai < 1 || lastLantai > 3)
//   // {
//   //   Serial.println("Error reading");
//   // }

//   if (currentMillis - lastDHTReadTime >= 2000) // saya ganti, sebelumnya lastDHTReadTime
//   {
//     lastDHTReadTime = currentMillis;
//     readDHT(temperature, humidity);

//     //======================Serial Monitor DEBUGGING Only
//     // Serial.print("Temperature: ");
//     // Serial.print(temperature);
//     // Serial.print("\t Humidity: ");
//     // Serial.println(humidity);
//   }

//   // DHT last correction
//   humidityLast = humidityState;
//   temperatureLast = temperatureState;

//   // (5 Hz)
//   if (currentMillis - lastSensorReadTime >= 200)
//   {
//     lastSensorReadTime = currentMillis;
//     readSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue);
//     kirimDataKeServer(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperatureLast, humidityLast); // lastLantai saya hapus
//   }

//   // * LCD Display *//
//   if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
//   {
//     lastDisplayUpdateTime = currentMillis;
//     updateDisplay(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity); // int currentFloor saya hapus
//   }
// }

// // void loop()
// // {
// //   if (WiFi.status() != WL_CONNECTED)
// //   {
// //     Serial.println("Reconnect WiFi ...");
// //     connectToWiFi();
// //   }

// //   // Cek state tombol lantai
// //   stateBtn1 = digitalRead(FLOOR_1);
// //   stateBtn2 = digitalRead(FLOOR_2);
// //   stateBtn3 = digitalRead(FLOOR_3);

// //   // Kondisi jika tombol lantai ditekan
// //   if (stateBtn1 == HIGH && lastStateBtn1 == LOW)
// //   {
// //     lastLantai = 1;
// //     digitalWrite(FLOOR_IND_1, HIGH);
// //     digitalWrite(FLOOR_IND_2, LOW);
// //     digitalWrite(FLOOR_IND_3, LOW);
// //   }
// //   else if (stateBtn2 == HIGH && lastStateBtn2 == LOW)
// //   {
// //     lastLantai = 2;
// //     digitalWrite(FLOOR_IND_1, LOW);
// //     digitalWrite(FLOOR_IND_2, HIGH);
// //     digitalWrite(FLOOR_IND_3, LOW);
// //   }
// //   else if (stateBtn3 == HIGH && lastStateBtn3 == LOW)
// //   {
// //     lastLantai = 3;
// //     digitalWrite(FLOOR_IND_1, LOW);
// //     digitalWrite(FLOOR_IND_2, LOW);
// //     digitalWrite(FLOOR_IND_3, HIGH);
// //   }

// //   lastStateBtn1 = stateBtn1;
// //   lastStateBtn2 = stateBtn2;
// //   lastStateBtn3 = stateBtn3;

// //   if (lastLantai < 1 || lastLantai > 3)
// //   {
// //     Serial.println("Error reading");
// //   }

// //   static float gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity;
// //   unsigned long currentMillis = millis();
// //   if (digitalRead(RESET_BUTTON_PIN) == LOW)
// //   {
// //     resetSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
// //   }
// //   else
// //   {
// //     // Update sensor ADXL, MPU, dan strain gauge (5 Hz)
// //     if (currentMillis - lastSensorReadTime >= sensorReadInterval)
// //     {
// //       lastSensorReadTime = currentMillis;
// //       readSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue);

// //       // Serial Monitor
// //       Serial.print("\n");
// //       Serial.print("Lantai: ");
// //       Serial.println(lastLantai);
// //       Serial.print("Gyro X: ");
// //       Serial.print(gyroX);
// //       Serial.print("\t Gyro Y: ");
// //       Serial.print(gyroY);
// //       Serial.print("\t Gyro Z: ");
// //       Serial.println(gyroZ);
// //       Serial.print("Accel X: ");
// //       Serial.print(accelX);
// //       Serial.print("\t Accel Y: ");
// //       Serial.print(accelY);
// //       Serial.print("\t Accel Z: ");
// //       Serial.println(accelZ);
// //       Serial.print("Strain Value: ");
// //       Serial.println(strainValue);
// //     }

// //     // Update DHT Sensor (0.5 Hz)
// //     if (currentMillis - lastDHTReadTime >= dhtReadInterval)
// //     {
// //       lastDHTReadTime = currentMillis;
// //       readDHT(temperature, humidity);

// //       // Serial Monitor
// //       Serial.print("Temperature: ");
// //       Serial.print(temperature);
// //       Serial.print("\t Humidity: ");
// //       Serial.println(humidity);
// //       Serial.print("\n");

// //       // Kirim data ke server
// //       kirimDataKeServer(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity, lastLantai);
// //     }

// //     // Update OLED Display (2 Hz)
// //     if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
// //     {
// //       lastDisplayUpdateTime = currentMillis;
// //       updateDisplay(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity, lastLantai);
// //     }
// //   }
// // }

// //=====================FIX DB SENDER=============================
// // ===============================================================


// ************* KODE BISA 1 ***********************
// #include <Arduino.h>
// #include <Adafruit_ADXL345_U.h>
// #include <Adafruit_MPU6050.h>
// #include <DHT.h>
// #include <Wire.h>
// #include <WiFi.h>
// #include <HTTPClient.h>
// #include <Adafruit_SSD1306.h>

// // Define Floor and indicator
// #define FLOOR_1 37
// #define FLOOR_2 36
// #define FLOOR_3 35
// #define FLOOR_IND_1 38
// #define FLOOR_IND_2 39
// #define FLOOR_IND_3 40

// // Additional constants for DB access
// int lastLantai = 0;                                                 // Lantai terakhir ke db
// bool stateBtn1 = LOW, stateBtn2 = LOW, stateBtn3 = LOW;             // Status tombol
// bool lastStateBtn1 = LOW, lastStateBtn2 = LOW, lastStateBtn3 = LOW; // Status tombol sebelumnya

// float temperature = 0, humidity = 0;
// float temperatureState = temperature; // Handler last value
// float humidityState = humidity;       // Handler last value

// // Define PIN DHT22
// #define DHTPIN 1
// #define DHTTYPE DHT22

// // Define MPU and ADXL
// #define MPU6050_ADDRESS 0x68
// #define ADXL345_ADDRESS 0x53
// #define SDA 8
// #define SCL 9

// // Define LCD output
// #define OLED_RESET 7 // RESET LCD output
// #define SCREEN_WIDTH 128
// #define SCREEN_HEIGHT 32
// #define SSD1306_I2C_ADDRESS 0x3C

// // Define Strain Gauge and Reset
// #define STRAIN_GAUGE_PIN 20
// #define RESET_BUTTON_PIN 2 // RESET All Instruments

// Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified();
// Adafruit_MPU6050 mpu;
// DHT dht(DHTPIN, DHTTYPE);

// unsigned long lastSensorReadTime = 0;
// unsigned long lastDHTReadTime = 0;
// unsigned long lastDisplayUpdateTime = 0;
// const unsigned long sensorReadInterval = 200;     // Define 5hz interval 200ms
// const unsigned long dhtReadInterval = 2000;       // Define 0.5hz interval 2s
// const unsigned long displayUpdateInterval = 2000; // LCD Display Update Interval

// int displayIndex = 0;
// int updateCount = 0;

// void kirimDataKeServer(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity);

// void connectToWiFi()
// {
//   display.clearDisplay();
//   display.setTextSize(1);
//   display.setTextColor(SSD1306_WHITE);
//   display.setCursor(0, 0);

//   Serial.println("Connecting to DTEO-VOKASI ... ");
//   display.println("Connecting to");
//   display.println("\nDTEO-VOKASI ...");
//   display.display();
//   delay(1500);

//   WiFi.begin("DTEO-VOKASI", "TEO123456");

//   unsigned long startTime = millis();
//   while (WiFi.status() != WL_CONNECTED)
//   {
//     if (millis() - startTime > 2000) // Re-connect 2s
//     {
//       //* DEBUG */
//       // Serial.println("Failed connect WiFi");
//       display.clearDisplay();
//       display.setCursor(0, 0);
//       display.println("WiFi Fail To Connect !!!");
//       display.display();
//       return;
//     }
//     delay(500);
//   }

//   //* DEBUG */
//   // Serial.println("WiFi Connected !!!");
//   // Serial.print("IP Address: ");
//   // Serial.println(WiFi.localIP());

//   display.clearDisplay();
//   display.setCursor(0, 0);
//   display.println("WiFi Connected !!!");
//   display.print("DTEO-VOKASI");
//   display.display();
//   delay(1500);
// }

// void setup()
// {
//   Serial.begin(115200);
//   Wire.begin(SDA, SCL);

//   // Reset Pin Configuration
//   pinMode(RESET_BUTTON_PIN, INPUT);

//   // Floor Button Configuration
//   pinMode(FLOOR_1, INPUT);
//   pinMode(FLOOR_2, INPUT);
//   pinMode(FLOOR_3, INPUT);
//   // Floor Button Indicator
//   pinMode(FLOOR_IND_1, OUTPUT);
//   pinMode(FLOOR_IND_2, OUTPUT);
//   pinMode(FLOOR_IND_3, OUTPUT);
//   digitalWrite(FLOOR_IND_1, LOW); // Normal mode Mati
//   digitalWrite(FLOOR_IND_2, LOW);
//   digitalWrite(FLOOR_IND_3, LOW);

//   if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
//   {
//     Serial.println(F("SSD1306 Gagal"));
//     for (;;)
//       ;
//   }

//   // if (!mpu.begin() && !adxl.begin()) {
//   //   Serial.println("MPU6050 & ADXL345 failed to init.");
//   // } else {
//   //   Serial.println("MPU6050 & ADXL345 connect.");
//   // }

//   //** ERROR HANDLER */
//   adxl.begin(ADXL345_ADDRESS);
//   mpu.begin(MPU6050_ADDRESS);
//   dht.begin();
//   if (!mpu.begin() && !adxl.begin())
//   {
//     Serial.println("MPU6050 Not Connected");
//     Serial.println("ADXL345 Not Connected");
//   }
//   else
//   {
//     Serial.println("MPU6050 Connected");
//     Serial.println("ADXL Connected");
//   }

//   connectToWiFi();
// }

// void readSensors(float &gyroX, float &gyroY, float &gyroZ, float &accelX, float &accelY, float &accelZ, float &strainValue)
// {
//   sensors_event_t a, g, temp;
//   mpu.getEvent(&a, &g, &temp);
//   gyroX = g.gyro.x;
//   gyroY = g.gyro.y;
//   gyroZ = g.gyro.z;
//   accelX = a.acceleration.x;
//   accelY = a.acceleration.y;
//   accelZ = a.acceleration.z;

//   int rawValue = analogRead(STRAIN_GAUGE_PIN);
//   strainValue = rawValue * (100.0 / 1023.0);
// }

// void readDHT(float &temperature, float &humidity)
// {
//   temperature = dht.readTemperature();
//   humidity = dht.readHumidity();

//   //** ERROR HANDLER */
//   if (isnan(temperature) || isnan(humidity))
//   {
//     Serial.println("DHT Sensor Error: NaN value");
//   }
//   else
//   {
//     temperatureState = temperature;
//     humidityState = humidity;
//   }
// }

// void kirimDataKeServer(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity) // int current Floor saya hapus
// {
//   // Print sensor values to Serial
//   // Serial.println("Data yang dikirim ke server:");
//   // Serial.print("Floor: ");
//   // Serial.println(currentFloor);
//   // Serial.print("Gyro X: ");
//   // Serial.println(gyroX);
//   // Serial.print("Gyro Y: ");
//   // Serial.println(gyroY);
//   // Serial.print("Gyro Z: ");
//   // Serial.println(gyroZ);
//   // Serial.print("Accel X: ");
//   // Serial.println(accelX);
//   // Serial.print("Accel Y: ");
//   // Serial.println(accelY);
//   // Serial.print("Accel Z: ");
//   // Serial.println(accelZ);
//   // Serial.print("Strain Value: ");
//   // Serial.println(strainValue);
//   // Serial.print("Temperature: ");
//   // Serial.println(temperature);
//   // Serial.print("Humidity: ");
//   // Serial.println(humidity);
//   // Serial.println();

//   HTTPClient http;
//   WiFiClient client;
//   String postData;

//   // Construct POST data string
//   postData = "humidity=" + String(humidity) +
//              "&temperature=" + String(temperature) +
//              "&accelX=" + String(accelX) +
//              "&accelY=" + String(accelY) +
//              "&accelZ=" + String(accelZ) +
//              "&gyroX=" + String(gyroX) +
//              "&gyroY=" + String(gyroY) +
//              "&gyroZ=" + String(gyroZ) +
//              "&strainValue=" + String(strainValue);
//   //  "&lantai=" + String(lastLantai);

//   http.begin(client, "http://10.17.38.118/shmsv2_2/sensor.php"); // MODUL LAMA
//   // http.begin(client, "http://10.17.37.202/shmsv2_2/sensor.php2"); // MODUL BARU
//   http.addHeader("Content-Type", "application/x-www-form-urlencoded");

//   int httpCode = http.POST(postData); // request
//   String payload = http.getString();  // payload

//   Serial.println("HTTP Response code: " + String(httpCode));
//   Serial.println("Server response: " + payload);

//   http.end();
// }

// void updateDisplay(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float strainValue, float temperature, float humidity)
// {
//   if (updateCount < 3)
//   {
//     display.clearDisplay();
//     display.setTextSize(1);
//     display.setTextColor(SSD1306_WHITE);
//     display.setCursor(0, 0);

//     switch (displayIndex)
//     {
//     case 0:
//       display.printf("Gyro X: %.2f deg/s", gyroX);
//       display.printf("\nGyro Y: %.2f deg/s", gyroY);
//       display.printf("\nGyro Z: %.2f deg/s", gyroZ);
//       digitalWrite(FLOOR_IND_1, HIGH);
//       digitalWrite(FLOOR_IND_2, LOW);
//       digitalWrite(FLOOR_IND_3, LOW);
//       break;
//     case 1:
//       display.printf("Accel X: %.2f m/s^2", accelX);
//       display.printf("\nAccel Y: %.2f m/s^2", accelY);
//       display.printf("\nAccel Z: %.2f m/s^2", accelZ);
//       digitalWrite(FLOOR_IND_1, LOW);
//       digitalWrite(FLOOR_IND_2, HIGH);
//       digitalWrite(FLOOR_IND_3, LOW);
//       break;
//     case 2:
//       display.printf("Strain  : %.2f N", strainValue);
//       display.printf("\nTemp    : %.2f C", temperature);
//       display.printf("\nHumidity: %.2f %%", humidity);
//       digitalWrite(FLOOR_IND_1, LOW);
//       digitalWrite(FLOOR_IND_2, LOW);
//       digitalWrite(FLOOR_IND_3, HIGH);
//       break;
//       // case 3:
//       //   display.printf("Floor: %d", currentFloor);
//       //   break;
//     }

//     display.display();
//     displayIndex = (displayIndex + 1) % 3;
//   }
// }

// void resetSensors(float &gyroX, float &gyroY, float &gyroZ, float &accelX, float &accelY, float &accelZ, float &strainValue, float &temperature, float &humidity)
// {
//   gyroX = gyroY = gyroZ = accelX = accelY = accelZ = strainValue = temperature = humidity = 0;

//   display.clearDisplay();
//   // display.setTextSize(1);
//   // display.setTextColor(SSD1306_WHITE);
//   display.setCursor(0, 0);
//   display.printf("Reset Success");

//   Serial.println("Reset Success");

//   digitalWrite(FLOOR_IND_1, 1);
//   delayMicroseconds(1500);
//   digitalWrite(FLOOR_IND_1, 0);
//   delayMicroseconds(500);
//   digitalWrite(FLOOR_IND_1, 1);
//   delayMicroseconds(1500);
//   digitalWrite(FLOOR_IND_1, 0);
//   delayMicroseconds(500);

//   //Debugging Device Calibration/
//   // Serial.println(gyroX);
//   // Serial.println(gyroY);
//   // Serial.println(gyroZ);
//   // Serial.println(accelX);
//   // Serial.println(accelY);
//   // Serial.println(accelZ);
//   // Serial.println(strainValue);
//   // Serial.println(temperature);
//   // Serial.println(humidity);
// }

// void loop()
// {
//   static float gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue;
//   unsigned long currentMillis = millis();

//   // RESET mu gurung kenek jancok
//   // if (digitalRead(RESET_BUTTON_PIN) == HIGH)
//   // {
//   //   resetSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
//   // }

//   if (WiFi.status() != WL_CONNECTED)
//   {
//     Serial.println("Reconnect WiFi ...");
//     connectToWiFi();
//   }

//   stateBtn1 = digitalRead(FLOOR_1);
//   stateBtn2 = digitalRead(FLOOR_2);
//   stateBtn3 = digitalRead(FLOOR_3);

//   /* // Current Floor SKIP jadi lewatnya DB 
//     if (stateBtn1 == HIGH && lastStateBtn1 == LOW)
//     {
//       lastLantai = 1;
//       digitalWrite(FLOOR_IND_1, HIGH);
//       digitalWrite(FLOOR_IND_2, LOW);
//       digitalWrite(FLOOR_IND_3, LOW);
//     }
//     else if (stateBtn2 == HIGH && lastStateBtn2 == LOW)
//     {
//       lastLantai = 2;
//       digitalWrite(FLOOR_IND_1, LOW);
//       digitalWrite(FLOOR_IND_2, HIGH);
//       digitalWrite(FLOOR_IND_3, LOW);
//     }
//     else if (stateBtn3 == HIGH && lastStateBtn3 == LOW)
//     {
//       lastLantai = 3;
//       digitalWrite(FLOOR_IND_1, LOW);
//       digitalWrite(FLOOR_IND_2, LOW);
//       digitalWrite(FLOOR_IND_3, HIGH);
//     }

//   */

//   lastStateBtn1 = stateBtn1;
//   lastStateBtn2 = stateBtn2;
//   lastStateBtn3 = stateBtn3;

//   // if (lastLantai < 1 || lastLantai > 3)
//   // {
//   //   Serial.println("Error reading");
//   // }

//   if (currentMillis - lastSensorReadTime >= 200) // saya ganti, sebelumnya lastDHTReadTime
//   {
//     lastDHTReadTime = currentMillis;
//     readDHT(temperature, humidity);

//     //======================Serial Monitor DEBUGGING Only
//     // Serial.print("Temperature: ");
//     // Serial.print(temperature);
//     // Serial.print("\t Humidity: ");
//     // Serial.println(humidity);
//   }

//   // (5 Hz)
//   if (currentMillis - lastSensorReadTime >= 200)
//   {
//     lastSensorReadTime = currentMillis;
//     readSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue);
//     kirimDataKeServer(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperatureState, humidityState); // lastLantai saya hapus
//   }

//   // * LCD Display *//
//   if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
//   {
//     lastDisplayUpdateTime = currentMillis;
//     updateDisplay(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity); // int currentFloor saya hapus
//   }
// }

// // void loop()
// // {
// //   if (WiFi.status() != WL_CONNECTED)
// //   {
// //     Serial.println("Reconnect WiFi ...");
// //     connectToWiFi();
// //   }

// //   // Cek state tombol lantai
// //   stateBtn1 = digitalRead(FLOOR_1);
// //   stateBtn2 = digitalRead(FLOOR_2);
// //   stateBtn3 = digitalRead(FLOOR_3);

// //   // Kondisi jika tombol lantai ditekan
// //   if (stateBtn1 == HIGH && lastStateBtn1 == LOW)
// //   {
// //     lastLantai = 1;
// //     digitalWrite(FLOOR_IND_1, HIGH);
// //     digitalWrite(FLOOR_IND_2, LOW);
// //     digitalWrite(FLOOR_IND_3, LOW);
// //   }
// //   else if (stateBtn2 == HIGH && lastStateBtn2 == LOW)
// //   {
// //     lastLantai = 2;
// //     digitalWrite(FLOOR_IND_1, LOW);
// //     digitalWrite(FLOOR_IND_2, HIGH);
// //     digitalWrite(FLOOR_IND_3, LOW);
// //   }
// //   else if (stateBtn3 == HIGH && lastStateBtn3 == LOW)
// //   {
// //     lastLantai = 3;
// //     digitalWrite(FLOOR_IND_1, LOW);
// //     digitalWrite(FLOOR_IND_2, LOW);
// //     digitalWrite(FLOOR_IND_3, HIGH);
// //   }

// //   lastStateBtn1 = stateBtn1;
// //   lastStateBtn2 = stateBtn2;
// //   lastStateBtn3 = stateBtn3;

// //   if (lastLantai < 1 || lastLantai > 3)
// //   {
// //     Serial.println("Error reading");
// //   }

// //   static float gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity;
// //   unsigned long currentMillis = millis();
// //   if (digitalRead(RESET_BUTTON_PIN) == LOW)
// //   {
// //     resetSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity);
// //   }
// //   else
// //   {
// //     // Update sensor ADXL, MPU, dan strain gauge (5 Hz)
// //     if (currentMillis - lastSensorReadTime >= sensorReadInterval)
// //     {
// //       lastSensorReadTime = currentMillis;
// //       readSensors(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue);

// //       // Serial Monitor
// //       Serial.print("\n");
// //       Serial.print("Lantai: ");
// //       Serial.println(lastLantai);
// //       Serial.print("Gyro X: ");
// //       Serial.print(gyroX);
// //       Serial.print("\t Gyro Y: ");
// //       Serial.print(gyroY);
// //       Serial.print("\t Gyro Z: ");
// //       Serial.println(gyroZ);
// //       Serial.print("Accel X: ");
// //       Serial.print(accelX);
// //       Serial.print("\t Accel Y: ");
// //       Serial.print(accelY);
// //       Serial.print("\t Accel Z: ");
// //       Serial.println(accelZ);
// //       Serial.print("Strain Value: ");
// //       Serial.println(strainValue);
// //     }

// //     // Update DHT Sensor (0.5 Hz)
// //     if (currentMillis - lastDHTReadTime >= dhtReadInterval)
// //     {
// //       lastDHTReadTime = currentMillis;
// //       readDHT(temperature, humidity);

// //       // Serial Monitor
// //       Serial.print("Temperature: ");
// //       Serial.print(temperature);
// //       Serial.print("\t Humidity: ");
// //       Serial.println(humidity);
// //       Serial.print("\n");

// //       // Kirim data ke server
// //       kirimDataKeServer(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity, lastLantai);
// //     }

// //     // Update OLED Display (2 Hz)
// //     if (currentMillis - lastDisplayUpdateTime >= displayUpdateInterval)
// //     {
// //       lastDisplayUpdateTime = currentMillis;
// //       updateDisplay(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, strainValue, temperature, humidity, lastLantai);
// //     }
// //   }
// // }

// //=====================FIX DB SENDER=============================
// // ===============================================================