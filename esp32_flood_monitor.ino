/*
 * Flood Early Warning System - ESP32 firmware
 * Sensors: JSN-SR04T (or HC-SR04) ultrasonic, optional tipping-bucket rain gauge, optional DHT22
 * Outputs: OLED (SSD1306), buzzer, 3 LEDs, Firebase Realtime Database (REST)
 *
 * Libraries (Arduino IDE -> Library Manager):
 *   - Adafruit SSD1306
 *   - Adafruit GFX Library
 *   - DHT sensor library + Adafruit Unified Sensor (only if USE_DHT = 1)
 * Board: "ESP32 Dev Module" (install "esp32 by Espressif" in Boards Manager)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ================= USER SETTINGS =================
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
// Firebase Realtime Database URL, no trailing slash
const char* FIREBASE_URL = "https://flood-ews-efb91-default-rtdb.asia-southeast1.firebasedatabase.app";

// Distance from sensor face down to the RIVER/CANAL BED (cm). Measure once after mounting.
const float SENSOR_HEIGHT_CM = 500.0;
// Level thresholds in metres (water height above the bed)
const float WARNING_M  = 3.0;
const float DANGER_M   = 4.0;

#define USE_RAIN 0   // 1 if you connected a tipping-bucket rain gauge
#define USE_DHT  0   // 1 if you connected a DHT22
const float MM_PER_TIP = 0.2794;   // check your rain gauge datasheet

const unsigned long SEND_EVERY_MS    = 10000;   // update "latest"
const unsigned long HISTORY_EVERY_MS = 60000;   // append to history
// =================================================

// Pins
#define PIN_TRIG 5
#define PIN_ECHO 18      // ECHO is 5V -> use a 1k/2k divider to 3.3V!
#define PIN_BUZZER 25
#define PIN_LED_G 26
#define PIN_LED_Y 27
#define PIN_LED_R 14
#define PIN_RAIN 33
#define PIN_DHT 4

#if USE_DHT
#include <DHT.h>
DHT dht(PIN_DHT, DHT22);
#endif

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;

enum Level { NORMAL, WARNING, DANGER };

volatile unsigned long lastTipMs = 0;
volatile uint32_t tipTimes[64];       // seconds since boot of recent tips
volatile uint8_t tipCount = 0;

void IRAM_ATTR onTip() {
  unsigned long now = millis();
  if (now - lastTipMs < 150) return;  // debounce
  lastTipMs = now;
  if (tipCount < 64) tipTimes[tipCount++] = now / 1000;
}

float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW); delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(12);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long d = pulseIn(PIN_ECHO, HIGH, 30000);
  if (d == 0) return -1;
  return d * 0.0343f / 2.0f;          // ~20C speed of sound
}

// median of 7 readings rejects noise/spikes
float medianDistance() {
  float v[7]; int n = 0;
  for (int i = 0; i < 7; i++) {
    float d = readDistanceCm();
    if (d > 20 && d < 600) v[n++] = d;
    delay(60);
  }
  if (n < 3) return -1;
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }
  return v[n / 2];
}

float rainMmPerHour() {
#if USE_RAIN
  unsigned long nowS = millis() / 1000;
  const unsigned long WINDOW = 600;   // last 10 minutes
  noInterrupts();
  uint8_t k = 0;
  for (uint8_t i = 0; i < tipCount; i++)
    if (nowS - tipTimes[i] <= WINDOW) tipTimes[k++] = tipTimes[i];
  tipCount = k;
  interrupts();
  return tipCount * MM_PER_TIP * (3600.0f / WINDOW);
#else
  return 0;
#endif
}

Level classify(float m) {
  if (m >= DANGER_M) return DANGER;
  if (m >= WARNING_M) return WARNING;
  return NORMAL;
}

void beep(int ms) { digitalWrite(PIN_BUZZER, HIGH); delay(ms); digitalWrite(PIN_BUZZER, LOW); }

void alarm(Level l) {
  digitalWrite(PIN_LED_G, l == NORMAL);
  digitalWrite(PIN_LED_Y, l == WARNING);
  digitalWrite(PIN_LED_R, l == DANGER);
  if (l == WARNING) { beep(150); }
  else if (l == DANGER) { beep(300); delay(100); beep(300); }
}

void showOled(float level, float rain, Level l, bool wifiOk) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1); oled.setCursor(0, 0);
  oled.print("Flood EWS  "); oled.print(wifiOk ? "WiFi OK" : "No WiFi");
  oled.setTextSize(2); oled.setCursor(0, 16);
  if (level < 0) oled.print("Sensor err"); else { oled.print(level, 2); oled.print(" m"); }
  oled.setTextSize(1); oled.setCursor(0, 40);
  oled.print("Rain: "); oled.print(rain, 1); oled.print(" mm/h");
  oled.setTextSize(2); oled.setCursor(0, 50 - 2);
  oled.print(l == NORMAL ? "NORMAL" : l == WARNING ? "WARNING" : "DANGER!");
  oled.display();
}

bool firebase(const char* method, const String& path, const String& json) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client; client.setInsecure();   // prototype only; pin the cert for production
  HTTPClient http;
  http.begin(client, String(FIREBASE_URL) + path);
  http.addHeader("Content-Type", "application/json");
  int code = (String(method) == "PUT") ? http.PUT(json) : http.POST(json);
  http.end();
  return code >= 200 && code < 300;
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);
  if (WiFi.status() == WL_CONNECTED) configTime(6 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT); pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_Y, OUTPUT); pinMode(PIN_LED_R, OUTPUT);
#if USE_RAIN
  pinMode(PIN_RAIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_RAIN), onTip, FALLING);
#endif
#if USE_DHT
  dht.begin();
#endif
  Wire.begin(21, 22);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  connectWifi();
  beep(100);
}

unsigned long lastSend = 0, lastHist = 0;

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWifi();

  float dist = medianDistance();
  float level = (dist < 0) ? -1 : max(0.0f, (SENSOR_HEIGHT_CM - dist) / 100.0f);
  float rain = rainMmPerHour();
  Level l = (level < 0) ? NORMAL : classify(level);
  bool wifiOk = WiFi.status() == WL_CONNECTED;

  alarm(l);
  showOled(level, rain, l, wifiOk);
  Serial.printf("dist=%.1fcm level=%.2fm rain=%.1f state=%d\n", dist, level, rain, l);

  float temp = 0, hum = 0;
#if USE_DHT
  temp = dht.readTemperature(); hum = dht.readHumidity();
  if (isnan(temp)) temp = 0; if (isnan(hum)) hum = 0;
#endif

  unsigned long now = millis();
  if (level >= 0 && (now - lastSend >= SEND_EVERY_MS)) {
    lastSend = now;
    String json = "{\"level\":" + String(level, 2) + ",\"rain\":" + String(rain, 1) +
                  ",\"temp\":" + String(temp, 1) + ",\"hum\":" + String(hum, 0) +
                  ",\"bat\":0,\"rssi\":" + String(WiFi.RSSI()) +
                  ",\"state\":" + String((int)l) + ",\"ts\":" + String((long)time(nullptr)) + "}";
    firebase("PUT", "/readings/latest.json", json);
    if (now - lastHist >= HISTORY_EVERY_MS) { lastHist = now; firebase("POST", "/readings/history.json", json); }
  }
  delay(1000);
}
