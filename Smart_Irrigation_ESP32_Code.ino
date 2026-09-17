/*
  Smart Irrigation System - ESP32
  Hardware:
    - ESP32
    - DHT11 temperature/humidity sensor
    - Resistive soil-moisture sensor (analog output)
    - Relay module controlling a water pump

  Data flow:
    ESP32 <-> Firebase Realtime Database <-> Website
    ESP32 -> Google Apps Script -> Google Sheet

  Required Arduino libraries:
    1. DHT sensor library by Adafruit
    2. Adafruit Unified Sensor
    3. ArduinoJson 7.x

  IMPORTANT:
    - Replace every placeholder under USER SETTINGS.
    - Calibrate SOIL_DRY_RAW and SOIL_WET_RAW using your own sensor.
    - Many relay modules are active LOW. Change RELAY_ACTIVE_LOW if needed.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ========================== USER SETTINGS ==========================
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Firebase Web API key (Firebase Console -> Project settings -> General)
const char* FIREBASE_API_KEY = "YOUR_FIREBASE_WEB_API_KEY";

// Full Realtime Database URL WITHOUT a trailing slash.
// Example: https://project-name-default-rtdb.asia-southeast1.firebasedatabase.app
const char* FIREBASE_DATABASE_URL = "https://YOUR_DATABASE_NAME-default-rtdb.asia-southeast1.firebasedatabase.app";

// Email/password account created in Firebase Authentication.
// For a prototype, this can be the same single account used by the website.
const char* FIREBASE_EMAIL = "YOUR_FIREBASE_LOGIN_EMAIL";
const char* FIREBASE_PASSWORD = "YOUR_FIREBASE_LOGIN_PASSWORD";

// Keep this exactly the same as DEVICE_ID in firebase-config.js.
const char* DEVICE_ID = "smart-irrigation-01";

// Apps Script deployed Web App URL ending in /exec.
// Leave empty ("") until Google Sheets setup is completed.
const char* GOOGLE_APPS_SCRIPT_URL = "";
// Use the same optional secret in google-apps-script/Code.gs.
const char* GOOGLE_SHEET_SECRET = "CHANGE_THIS_RANDOM_SECRET";

// Pin configuration
const uint8_t DHT_PIN = 4;
const uint8_t SOIL_SENSOR_PIN = 34;
const uint8_t RELAY_PIN = 26;
const uint8_t DHT_TYPE = DHT11;

// Change to false when your relay turns ON with HIGH.
const bool RELAY_ACTIVE_LOW = true;

// Replace these values after calibration.
// Typical ESP32 ADC range: 0 to 4095.
int SOIL_DRY_RAW = 3200;
int SOIL_WET_RAW = 1200;
// ==================================================================

const unsigned long LIVE_UPLOAD_INTERVAL_MS = 5000;
const unsigned long CONFIG_POLL_INTERVAL_MS = 10000;
const unsigned long HISTORY_INTERVAL_MS = 60000;
const unsigned long AUTH_REFRESH_INTERVAL_MS = 45UL * 60UL * 1000UL;
const unsigned long MAX_PUMP_RUN_MS = 2UL * 60UL * 1000UL;
const unsigned long PUMP_COOLDOWN_MS = 60UL * 1000UL;

DHT dht(DHT_PIN, DHT_TYPE);
Preferences preferences;

String firebaseIdToken;
unsigned long lastAuthMs = 0;
unsigned long lastLiveUploadMs = 0;
unsigned long lastConfigPollMs = 0;
unsigned long lastHistoryMs = 0;
unsigned long pumpStartedMs = 0;
unsigned long pumpCooldownUntilMs = 0;

float temperatureC = NAN;
float humidityPercent = NAN;
float soilMoisturePercent = 0.0f;
int soilRaw = 0;
bool dhtOk = false;
bool soilSensorOk = false;
bool pumpOn = false;
String pumpReason = "Starting";

String plantId = "";
String plantName = "Not configured";
String soilId = "";
String soilName = "Not configured";
int minMoisture = 40;
int maxMoisture = 55;
unsigned long long commandVersion = 0;

void setPump(bool turnOn, const String& reason);
void connectWiFi();
bool firebaseSignIn();
bool firebaseRequest(const String& method, const String& path, const String& payload, String* responseBody = nullptr);
void readSensors();
void controlPump();
void publishLiveData();
void storeHistoryAndSheet();
void pollConfiguration();
void loadSavedConfiguration();
void saveConfiguration();
unsigned long long currentTimestampMs();
void sendToGoogleSheet(unsigned long long timestamp);

void setup() {
  Serial.begin(115200);
  delay(400);

  pinMode(RELAY_PIN, OUTPUT);
  setPump(false, "System starting");

  analogReadResolution(12);
  dht.begin();
  preferences.begin("irrigation", false);
  loadSavedConfiguration();

  connectWiFi();
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  if (!firebaseSignIn()) {
    Serial.println("Firebase sign-in failed. The ESP32 will retry.");
  }

  Serial.println("Smart Irrigation ESP32 started.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (firebaseIdToken.isEmpty() || millis() - lastAuthMs >= AUTH_REFRESH_INTERVAL_MS) {
    firebaseSignIn();
  }

  readSensors();
  controlPump();

  if (millis() - lastConfigPollMs >= CONFIG_POLL_INTERVAL_MS) {
    lastConfigPollMs = millis();
    pollConfiguration();
  }

  if (millis() - lastLiveUploadMs >= LIVE_UPLOAD_INTERVAL_MS) {
    lastLiveUploadMs = millis();
    publishLiveData();
  }

  if (millis() - lastHistoryMs >= HISTORY_INTERVAL_MS) {
    lastHistoryMs = millis();
    storeHistoryAndSheet();
  }

  delay(250);
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWi-Fi connection timed out.");
  }
}

bool firebaseSignIn() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // Prototype convenience. Install Google's CA certificate for production use.

  HTTPClient http;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;
  if (!http.begin(client, url)) return false;

  http.addHeader("Content-Type", "application/json");

  JsonDocument requestDoc;
  requestDoc["email"] = FIREBASE_EMAIL;
  requestDoc["password"] = FIREBASE_PASSWORD;
  requestDoc["returnSecureToken"] = true;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  int statusCode = http.POST(requestBody);
  String responseBody = http.getString();
  http.end();

  if (statusCode < 200 || statusCode >= 300) {
    Serial.printf("Firebase auth failed (%d): %s\n", statusCode, responseBody.c_str());
    firebaseIdToken = "";
    return false;
  }

  JsonDocument responseDoc;
  DeserializationError error = deserializeJson(responseDoc, responseBody);
  if (error || !responseDoc["idToken"].is<const char*>()) {
    Serial.printf("Firebase auth JSON error: %s\n", error.c_str());
    firebaseIdToken = "";
    return false;
  }

  firebaseIdToken = responseDoc["idToken"].as<String>();
  lastAuthMs = millis();
  Serial.println("Firebase authentication successful.");
  return true;
}

bool firebaseRequest(const String& method, const String& path, const String& payload, String* responseBody) {
  if (WiFi.status() != WL_CONNECTED || firebaseIdToken.isEmpty()) return false;

  WiFiClientSecure client;
  client.setInsecure(); // Prototype convenience. Use CA validation for production.

  HTTPClient http;
  String url = String(FIREBASE_DATABASE_URL) + path + ".json?auth=" + firebaseIdToken;
  if (!http.begin(client, url)) return false;

  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");

  int statusCode = -1;
  if (method == "GET") {
    statusCode = http.GET();
  } else if (method == "PUT") {
    statusCode = http.PUT(payload);
  } else if (method == "PATCH") {
    statusCode = http.sendRequest("PATCH", payload);
  } else if (method == "DELETE") {
    statusCode = http.sendRequest("DELETE");
  }

  String body = http.getString();
  if (responseBody) *responseBody = body;
  http.end();

  if (statusCode == 401) {
    firebaseIdToken = "";
  }

  if (statusCode < 200 || statusCode >= 300) {
    Serial.printf("Firebase %s failed (%d): %s\n", method.c_str(), statusCode, body.c_str());
    return false;
  }
  return true;
}

void readSensors() {
  soilRaw = analogRead(SOIL_SENSOR_PIN);

  // Values far outside the expected ADC range are treated as sensor errors.
  soilSensorOk = soilRaw >= 0 && soilRaw <= 4095 && SOIL_DRY_RAW != SOIL_WET_RAW;
  if (soilSensorOk) {
    float mapped = 100.0f * (float)(SOIL_DRY_RAW - soilRaw) / (float)(SOIL_DRY_RAW - SOIL_WET_RAW);
    soilMoisturePercent = constrain(mapped, 0.0f, 100.0f);
  }

  float newHumidity = dht.readHumidity();
  float newTemperature = dht.readTemperature();
  dhtOk = !isnan(newHumidity) && !isnan(newTemperature);
  if (dhtOk) {
    humidityPercent = newHumidity;
    temperatureC = newTemperature;
  }
}

void controlPump() {
  if (plantId.isEmpty() || soilId.isEmpty()) {
    setPump(false, "No plant/soil profile");
    return;
  }

  if (!soilSensorOk) {
    setPump(false, "Soil sensor error");
    return;
  }

  if (pumpOn && millis() - pumpStartedMs >= MAX_PUMP_RUN_MS) {
    setPump(false, "Safety timeout");
    pumpCooldownUntilMs = millis() + PUMP_COOLDOWN_MS;
    return;
  }

  if (!pumpOn && millis() < pumpCooldownUntilMs) {
    pumpReason = "Safety cooldown";
    return;
  }

  // Hysteresis control prevents rapid relay switching:
  // turn ON below minimum and remain ON until maximum is reached.
  if (!pumpOn && soilMoisturePercent < minMoisture) {
    setPump(true, "Moisture below minimum");
  } else if (pumpOn && soilMoisturePercent >= maxMoisture) {
    setPump(false, "Target moisture reached");
  } else if (pumpOn) {
    pumpReason = "Watering to target";
  } else {
    pumpReason = "Moisture within range";
  }
}

void setPump(bool turnOn, const String& reason) {
  if (turnOn != pumpOn) {
    pumpOn = turnOn;
    if (pumpOn) pumpStartedMs = millis();
  }

  const uint8_t onLevel = RELAY_ACTIVE_LOW ? LOW : HIGH;
  const uint8_t offLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(RELAY_PIN, pumpOn ? onLevel : offLevel);
  pumpReason = reason;
}

void publishLiveData() {
  unsigned long long timestamp = currentTimestampMs();
  if (timestamp == 0) return;

  JsonDocument doc;
  doc["timestamp"] = timestamp;
  if (dhtOk) {
    doc["temperature"] = temperatureC;
    doc["humidity"] = humidityPercent;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"] = nullptr;
  }
  doc["soilRaw"] = soilRaw;
  doc["soilMoisture"] = soilMoisturePercent;
  doc["pumpOn"] = pumpOn;
  doc["pumpReason"] = pumpReason;
  doc["dhtOk"] = dhtOk;
  doc["soilSensorOk"] = soilSensorOk;
  doc["wifiRssi"] = WiFi.RSSI();
  doc["plantName"] = plantName;
  doc["soilName"] = soilName;
  doc["minMoisture"] = minMoisture;
  doc["maxMoisture"] = maxMoisture;
  doc["deviceOnline"] = true;

  String payload;
  serializeJson(doc, payload);
  firebaseRequest("PUT", String("/devices/") + DEVICE_ID + "/live", payload);
}

void storeHistoryAndSheet() {
  unsigned long long timestamp = currentTimestampMs();
  if (timestamp == 0) return;

  JsonDocument doc;
  doc["timestamp"] = timestamp;
  if (dhtOk) {
    doc["temperature"] = temperatureC;
    doc["humidity"] = humidityPercent;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"] = nullptr;
  }
  doc["soilRaw"] = soilRaw;
  doc["soilMoisture"] = soilMoisturePercent;
  doc["pumpOn"] = pumpOn;
  doc["pumpReason"] = pumpReason;
  doc["plantId"] = plantId;
  doc["plantName"] = plantName;
  doc["soilId"] = soilId;
  doc["soilName"] = soilName;
  doc["minMoisture"] = minMoisture;
  doc["maxMoisture"] = maxMoisture;
  doc["dhtOk"] = dhtOk;
  doc["soilSensorOk"] = soilSensorOk;
  doc["deviceOnline"] = true;

  String payload;
  serializeJson(doc, payload);

  char timestampKey[24];
  snprintf(timestampKey, sizeof(timestampKey), "%llu", timestamp);
  String historyPath = String("/devices/") + DEVICE_ID + "/history/" + timestampKey;
  if (firebaseRequest("PUT", historyPath, payload)) {
    sendToGoogleSheet(timestamp);
  }
}

void pollConfiguration() {
  String response;
  String path = String("/devices/") + DEVICE_ID + "/config";
  if (!firebaseRequest("GET", path, "", &response) || response == "null") return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.printf("Configuration JSON error: %s\n", error.c_str());
    return;
  }

  unsigned long long receivedVersion = doc["commandVersion"] | 0ULL;
  bool changed = receivedVersion != 0 && receivedVersion != commandVersion;
  bool firstLoad = plantId.isEmpty();
  if (!changed && !firstLoad) return;

  String receivedPlantId = doc["plantId"] | "";
  String receivedSoilId = doc["soilId"] | "";
  int receivedMin = doc["minMoisture"] | minMoisture;
  int receivedMax = doc["maxMoisture"] | maxMoisture;

  if (receivedPlantId.isEmpty() || receivedSoilId.isEmpty() || receivedMin < 0 || receivedMax > 100 || receivedMin >= receivedMax) {
    Serial.println("Rejected invalid plant/soil configuration.");
    return;
  }

  plantId = receivedPlantId;
  plantName = doc["plantName"] | receivedPlantId;
  soilId = receivedSoilId;
  soilName = doc["soilName"] | receivedSoilId;
  minMoisture = receivedMin;
  maxMoisture = receivedMax;
  commandVersion = receivedVersion;
  saveConfiguration();

  Serial.printf("New profile: %s + %s, %d%% to %d%%\n",
                plantName.c_str(), soilName.c_str(), minMoisture, maxMoisture);
}

void loadSavedConfiguration() {
  plantId = preferences.getString("plantId", "");
  plantName = preferences.getString("plantName", "Not configured");
  soilId = preferences.getString("soilId", "");
  soilName = preferences.getString("soilName", "Not configured");
  minMoisture = preferences.getInt("minMoist", 40);
  maxMoisture = preferences.getInt("maxMoist", 55);
  commandVersion = preferences.getULong64("cmdVersion", 0);
}

void saveConfiguration() {
  preferences.putString("plantId", plantId);
  preferences.putString("plantName", plantName);
  preferences.putString("soilId", soilId);
  preferences.putString("soilName", soilName);
  preferences.putInt("minMoist", minMoisture);
  preferences.putInt("maxMoist", maxMoisture);
  preferences.putULong64("cmdVersion", commandVersion);
}

unsigned long long currentTimestampMs() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    Serial.println("Waiting for valid internet time...");
    return 0;
  }
  return (unsigned long long)now * 1000ULL;
}

void sendToGoogleSheet(unsigned long long timestamp) {
  if (strlen(GOOGLE_APPS_SCRIPT_URL) < 10 || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // Prototype convenience. Use CA validation for production.

  HTTPClient http;
  if (!http.begin(client, GOOGLE_APPS_SCRIPT_URL)) return;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(12000);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["timestamp"] = timestamp;
  doc["deviceId"] = DEVICE_ID;
  doc["secret"] = GOOGLE_SHEET_SECRET;
  if (dhtOk) {
    doc["temperature"] = temperatureC;
    doc["humidity"] = humidityPercent;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"] = nullptr;
  }
  doc["soilMoisture"] = soilMoisturePercent;
  doc["soilRaw"] = soilRaw;
  doc["pumpStatus"] = pumpOn ? "ON" : "OFF";
  doc["pumpReason"] = pumpReason;
  doc["plant"] = plantName;
  doc["soil"] = soilName;
  doc["minimumMoisture"] = minMoisture;
  doc["maximumMoisture"] = maxMoisture;
  doc["wifiRssi"] = WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);
  int statusCode = http.POST(payload);
  String response = http.getString();
  http.end();

  if (statusCode < 200 || statusCode >= 300) {
    Serial.printf("Google Sheet upload failed (%d): %s\n", statusCode, response.c_str());
  }
}
