/*
 * ESP32 Local Data Logger
 * Logs DHT22 (temperature/humidity) and HC-SR501 (motion) sensor data
 * Saves data locally for Power BI Desktop visualization
 * 
 * Data is saved to SPIFFS (internal flash) in CSV format
 * Perfect for Power BI Desktop import
 */

// ============================================================================
// LIBRARY INCLUDES
// ============================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <time.h>

// ============================================================================
// HARDWARE CONFIGURATION
// ============================================================================
// Sensor Configuration
#define DHT22_PIN 21
#define MOTION_PIN 13
#define DHT_TYPE DHT22

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================
// WiFi Configuration (for time sync and web access)
const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASS";

// ============================================================================
// TIME CONFIGURATION
// ============================================================================
// Time Configuration (for timestamps)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;  // Helsinki UTC+3
const int daylightOffset_sec = 0;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
// Sensor objects
DHT dht22(DHT22_PIN, DHT_TYPE);

// Global variables
float humidity = 0.0f;
float temperature = 0.0f;
bool motionDetected = false;
int motionStateCurrent = LOW;
int motionStatePrevious = LOW;
unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 10000; // Log every 10 seconds
unsigned long dataPointCount = 0;
time_t lastMotionDetected = 0; // Epoch time of last motion rising edge (0 = never)
bool motionDuringInterval = false; // Track if motion occurred during current 10-second interval

// Web server for data access
WebServer server(80);

// ============================================================================
// MAIN SETUP FUNCTION
// ============================================================================
void setup() {
  Serial.begin(9600);
  Serial.println("=== ESP32 Local Data Logger ===");
  
  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ Failed to mount SPIFFS");
    return;
  }
  Serial.println("✅ SPIFFS mounted successfully");
  
  // Initialize sensors
  dht22.begin();
  pinMode(MOTION_PIN, INPUT);
  Serial.println("✅ Sensors initialized");
  
  // Give DHT22 time to stabilize
  delay(2000);
  
  // Connect to WiFi for time sync
  connectToWiFi();
  
  // Initialize time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("⏰ Time synchronization started");
  
  // Set up web server routes
  setupWebServer();
  
  // Create CSV header if file doesn't exist
  createCSVHeader();
  
  Serial.println("🚀 Data logger ready!");
  Serial.println("📊 Data will be saved every 10 seconds");
  Serial.println("🌐 Access data via: http://" + WiFi.localIP().toString());
  Serial.println("=====================================");
}

// ============================================================================
// MAIN LOOP FUNCTION
// ============================================================================
void loop() {
  // Handle web server requests
  server.handleClient();
  
  // Check motion frequently (every loop)
  checkMotion();
  
  // Log data at specified intervals
  if (millis() - lastLogTime >= LOG_INTERVAL) {
    readSensors();
    
    // Set motion_detected based on whether motion occurred during this interval
    motionDetected = motionDuringInterval;
    
    logDataToCSV();
    displayCurrentData();
    lastLogTime = millis();
    
    // Reset motion tracking for next interval
    motionDuringInterval = false;
  }
  
  // Small delay to prevent watchdog issues
  delay(100);
}

// ============================================================================
// NETWORK FUNCTIONS
// ============================================================================
void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("🔗 Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("✅ WiFi connected!");
  Serial.println("📱 IP address: " + WiFi.localIP().toString());
}

// ============================================================================
// SENSOR FUNCTIONS
// ============================================================================
void readSensors() {
  // Read DHT22 sensor with retry logic
  humidity = dht22.readHumidity();
  temperature = dht22.readTemperature();
  
  // Validate readings
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("⚠️  DHT22 reading failed, retrying...");
    delay(100);
    humidity = dht22.readHumidity();
    temperature = dht22.readTemperature();
    
    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("❌ DHT22 still failing - check connections!");
      // Use defaults to avoid NaN in data
      if (isnan(humidity)) humidity = 0.0;
      if (isnan(temperature)) temperature = 0.0;
    }
  }
}

void checkMotion() {
  // Read motion sensor
  motionStatePrevious = motionStateCurrent;
  motionStateCurrent = digitalRead(MOTION_PIN);
  
  if (motionStatePrevious == LOW && motionStateCurrent == HIGH) {
    Serial.println("🚶 Motion detected!");
    motionDuringInterval = true; // Mark that motion occurred in this interval
    // Record time of motion detection
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      lastMotionDetected = mktime(&timeinfo);
    }
  } else if (motionStatePrevious == HIGH && motionStateCurrent == LOW) {
    Serial.println("🛑 Motion stopped!");
  }
}

// ============================================================================
// DATA STORAGE FUNCTIONS
// ============================================================================
void createCSVHeader() {
  File file = SPIFFS.open("/sensor_data.csv", "r");
  if (!file) {
    // File doesn't exist, create it with header
    file = SPIFFS.open("/sensor_data.csv", "w");
    if (file) {
      file.println("Timestamp,DateTime,Temperature_C,Temperature_F,Humidity_Percent,Motion_Detected,Data_Point");
      file.close();
      Serial.println("📝 Created new CSV file with header");
    } else {
      Serial.println("❌ Failed to create CSV file");
    }
  } else {
    file.close();
    Serial.println("📄 CSV file already exists");
  }
}

void logDataToCSV() {
  // Get current time
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("⚠️  Failed to obtain time");
    return;
  }
  
  // Format timestamp and datetime
  char timestamp[32];
  char datetime[32];
  strftime(timestamp, sizeof(timestamp), "%s", &timeinfo);  // Unix timestamp
  strftime(datetime, sizeof(datetime), "%Y-%m-%d %H:%M:%S", &timeinfo);  // Human readable
  
  // Open file for appending
  File file = SPIFFS.open("/sensor_data.csv", "a");
  if (!file) {
    Serial.println("❌ Failed to open CSV file for writing");
    return;
  }
  
  // Calculate Fahrenheit
  float tempF = (temperature * 9.0/5.0) + 32.0;
  
  // Write data row
  file.printf("%s,%s,%.2f,%.2f,%.2f,%s,%lu\n",
    timestamp,
    datetime,
    temperature,
    tempF,
    humidity,
    motionDetected ? "true" : "false",
    ++dataPointCount
  );
  
  file.close();
  Serial.printf("💾 Data logged to CSV\n");
}

void displayCurrentData() {
  Serial.println("📊 Current Readings:");
  Serial.printf("   🌡️  Temperature: %.1f°C (%.1f°F)\n", temperature, (temperature * 9.0/5.0) + 32.0);
  Serial.printf("   💧 Humidity: %.1f%%\n", humidity);
  Serial.printf("   🚶 Motion: %s\n", motionDetected ? "DETECTED" : "None");
  Serial.printf("   📈 Data Points: %lu\n", dataPointCount);
  Serial.println("---");
}

// ============================================================================
// WEB SERVER FUNCTIONS
// ============================================================================
void setupWebServer() {
  // Home page with current data
  server.on("/", handleRoot);
  
  // Download CSV file
  server.on("/download", handleDownload);
  
  // View data as JSON
  server.on("/data", handleDataJSON);
  
  
  // Clear all data
  server.on("/clear", handleClear);
  
  // Get file info
  server.on("/info", handleInfo);
  
  server.begin();
  Serial.println("🌐 Web server started");
}

// ----------------------------------------------------------------------------
// Web Route Handlers
// ----------------------------------------------------------------------------
void handleRoot() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  char datetime[32];
  strftime(datetime, sizeof(datetime), "%Y-%m-%d %H:%M:%S", &timeinfo);
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>ESP32 IoT Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='" + String(LOG_INTERVAL / 1000) + "'>";
  html += "<style>";
  html += "body{font-family:'Segoe UI',Arial,sans-serif;margin:0;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:#333}";
  html += ".container{max-width:1200px;margin:0 auto;padding:20px}";
  html += ".header{background:rgba(255,255,255,0.95);padding:20px;border-radius:15px;margin-bottom:20px;box-shadow:0 8px 32px rgba(0,0,0,0.1);backdrop-filter:blur(10px)}";
  html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:20px;margin-bottom:20px}";
  html += ".card{background:rgba(255,255,255,0.95);padding:20px;border-radius:15px;box-shadow:0 8px 32px rgba(0,0,0,0.1);backdrop-filter:blur(10px);margin-bottom:20px}";
  html += ".metric{text-align:center;padding:20px}";
  html += ".metric-value{font-size:2.5em;font-weight:bold;margin:10px 0}";
  html += ".metric-label{font-size:1.1em;color:#666;text-transform:uppercase;letter-spacing:1px}";
  html += ".temp{color:#ff6b6b}";
  html += ".humidity{color:#4ecdc4}";
  html += ".motion{color:" + String(motionDetected ? "#ff4757" : "#2ed573") + "}";
  html += ".chart-container{position:relative;height:300px;margin:20px 0}";
  html += ".controls{text-align:center;margin:20px 0}";
  html += "button{background:linear-gradient(45deg,#667eea,#764ba2);color:white;padding:12px 24px;border:none;border-radius:25px;margin:5px;cursor:pointer;font-weight:bold;box-shadow:0 4px 15px rgba(0,0,0,0.2);transition:all 0.3s ease}";
  html += "button:hover{transform:translateY(-2px);box-shadow:0 6px 20px rgba(0,0,0,0.3)}";
  html += ".status{padding:10px;border-radius:10px;margin:10px 0;text-align:center;font-weight:bold}";
  html += ".status.online{background:#d4edda;color:#155724}";
  html += ".status.motion{background:#f8d7da;color:#721c24}";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='container'>";
  html += "<div class='header'>";
  html += "<h1>ESP32 IoT Dashboard</h1>";
  html += "<p><strong>Last Update:</strong> " + String(datetime) + "</p>";
  // Last motion display
  time_t nowEpoch = mktime(&timeinfo);
  String lastMotionText;
  if (lastMotionDetected == 0) {
    lastMotionText = "Never";
  } else {
    struct tm motionTm;
    localtime_r(&lastMotionDetected, &motionTm);
    char motionBuf[32];
    strftime(motionBuf, sizeof(motionBuf), "%Y-%m-%d %H:%M:%S", &motionTm);
    unsigned long diff = (unsigned long)(nowEpoch - lastMotionDetected);
    String ago;
    if (diff < 60) ago = "just now";
    else if (diff < 3600) ago = String(diff / 60) + " min ago";
    else if (diff < 86400) ago = String(diff / 3600) + " hr ago";
    else ago = String(diff / 86400) + " d ago";
    lastMotionText = String(motionBuf) + " (" + ago + ")";
  }
  html += "<div class='status online'>ONLINE - System Running - " + String(dataPointCount) + " data points collected</div>";
  if (motionDetected) {
    html += "<div class='status motion'>ALERT: Motion Currently Detected!</div>";
  }
  html += "</div>";
  
  // Metrics Cards
  html += "<div class='grid'>";
  html += "<div class='card'>";
  html += "<div class='metric temp'>";
  html += "<div class='metric-label'>Temperature</div>";
  html += "<div class='metric-value'>" + String(temperature, 1) + "&deg;C</div>";
  html += "<div style='font-size:1.2em;color:#999'>" + String((temperature * 9.0/5.0) + 32.0, 1) + "&deg;F</div>";
  html += "</div></div>";
  
  html += "<div class='card'>";
  html += "<div class='metric humidity'>";
  html += "<div class='metric-label'>Humidity</div>";
  html += "<div class='metric-value'>" + String(humidity, 1) + "%</div>";
  html += "<div style='font-size:1.2em;color:#999'>Relative Humidity</div>";
  html += "</div></div>";
  
  html += "<div class='card'>";
  html += "<div class='metric motion'>";
  html += "<div class='metric-label'>Motion Status</div>";
  html += "<div class='metric-value'>" + String(motionDetected ? "ACTIVE" : "CLEAR") + "</div>";
  html += "<div style='font-size:1.2em;color:#999'>Last: " + lastMotionText + "</div>";
  html += "</div></div>";
  html += "</div>";
  
  // Controls
  html += "<div class='card'>";
  html += "<div class='controls'>";
  html += "<h3>Data Management</h3>";
  html += "<button onclick=\"location.href='/download'\">Download CSV</button>";
  html += "<button onclick=\"location.href='/data'\">JSON Data</button>";
  html += "<button onclick=\"location.href='/info'\">System Info</button>";
  html += "<button onclick=\"if(confirm('Clear all data?'))location.href='/clear'\">Clear Data</button>";
  html += "</div></div>";
  
  html += "</div>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleDownload() {
  File file = SPIFFS.open("/sensor_data.csv", "r");
  if (!file) {
    server.send(404, "text/plain", "CSV file not found");
    return;
  }
  
  server.setContentLength(file.size());
  server.send(200, "text/csv", "");
  
  // Stream file content
  uint8_t buffer[512];
  while (file.available()) {
    int bytesRead = file.read(buffer, sizeof(buffer));
    server.client().write(buffer, bytesRead);
  }
  
  file.close();
  Serial.println("📥 CSV file downloaded");
}


void handleDataJSON() {
  File file = SPIFFS.open("/sensor_data.csv", "r");
  if (!file) {
    server.send(404, "application/json", "{\"error\":\"No data file found\"}");
    return;
  }
  
  String json = "{\"data\":[";
  String line;
  bool firstLine = true;
  bool isHeader = true;
  
  while (file.available()) {
    line = file.readStringUntil('\n');
    line.trim();
    
    if (isHeader) {
      isHeader = false;
      continue;  // Skip header row
    }
    
    if (line.length() > 0) {
      if (!firstLine) json += ",";
      
      // Parse CSV line and convert to JSON
      int commaCount = 0;
      String fields[7];
      int lastComma = -1;
      
      for (int i = 0; i < line.length(); i++) {
        if (line[i] == ',' || i == line.length() - 1) {
          fields[commaCount] = line.substring(lastComma + 1, i + (i == line.length() - 1 ? 1 : 0));
          lastComma = i;
          commaCount++;
          if (commaCount >= 7) break;
        }
      }
      
      json += "{";
      json += "\"timestamp\":" + fields[0] + ",";
      json += "\"datetime\":\"" + fields[1] + "\",";
      json += "\"temperature_c\":" + fields[2] + ",";
      json += "\"temperature_f\":" + fields[3] + ",";
      json += "\"humidity\":" + fields[4] + ",";
      json += "\"motion_detected\":" + fields[5] + ",";
      json += "\"data_point\":" + fields[6];
      json += "}";
      
      firstLine = false;
    }
  }
  
  json += "]}";
  file.close();
  
  server.send(200, "application/json", json);
}

void handleClear() {
  SPIFFS.remove("/sensor_data.csv");
  createCSVHeader();
  dataPointCount = 0;
  server.send(200, "text/html", "<html><body><h2>Data cleared!</h2><p><a href='/'>Back to dashboard</a></p></body></html>");
  Serial.println("Data file cleared");
}

void handleInfo() {
  File file = SPIFFS.open("/sensor_data.csv", "r");
  String info = "<html><body><h2>File Information</h2>";
  
  if (file) {
    size_t fileSize = file.size();
    file.close();
    
    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    
    info += "<p><strong>File Size:</strong> " + String(fileSize) + " bytes</p>";
    info += "<p><strong>Data Points:</strong> " + String(dataPointCount) + "</p>";
    info += "<p><strong>Storage Used:</strong> " + String(usedBytes) + " / " + String(totalBytes) + " bytes</p>";
    info += "<p><strong>Storage Free:</strong> " + String(totalBytes - usedBytes) + " bytes</p>";
  } else {
    info += "<p>No data file found</p>";
  }
  
  info += "<p><a href='/'>Back to dashboard</a></p></body></html>";
  server.send(200, "text/html", info);
}
