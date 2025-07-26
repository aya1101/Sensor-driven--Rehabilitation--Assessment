// =======================================================================
// ESP8266 MPU6050 IoT Sensor Node - Phiên bản Tương thích Server Python
// -----------------------------------------------------------------------
// Nâng cấp bởi Minh Lê.
// Đã thay đổi định dạng gói tin sang JSON để tương thích với code server.
// =======================================================================

// --- Thư viện cần thiết cho ESP8266 ---
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Wire.h>

// --- Cấu hình phần cứng và hằng số ---
#define LED_PIN 2 // D2 trên NodeMCU
const int MPU_ADDR = 0x68;
#define UDP_PORT 1234

// --- Hằng số điều khiển ---
#define SOFTAP_CONFIG_TIMEOUT_MINUTES 5
#define HANDSHAKE_RETRY_INTERVAL 5000 // 5 giây
#define SERVER_HEARTBEAT_TIMEOUT 10000 // 10 giây
#define MAX_HEARTBEAT_FAILURES 3
#define DATA_PACKET_RESET_THRESHOLD 200 // Handshake lại sau 200 gói tin

// --- Định nghĩa kích thước và vị trí trong EEPROM ---
#define NODE_ID_LENGTH   32
#define NODE_TYPE_LENGTH 16
#define AP_SSID_LENGTH   32
#define AP_PASS_LENGTH   32
#define SERVER_IP_LENGTH 4
#define MPU_ACCEL_SCALE_LENGTH  1
#define MPU_GYRO_SCALE_LENGTH   1
#define MPU_SAMPLE_RATE_DIVIDER_LENGTH 1
#define DATA_FREQ_LENGTH 1

#define NODE_ID_OFFSET   0
#define NODE_TYPE_OFFSET (NODE_ID_OFFSET + NODE_ID_LENGTH)
#define AP_SSID_OFFSET   (NODE_TYPE_OFFSET + NODE_TYPE_LENGTH)
#define AP_PASS_OFFSET   (AP_SSID_OFFSET + AP_SSID_LENGTH)
#define SERVER_IP_OFFSET (AP_PASS_OFFSET + AP_PASS_LENGTH)
#define MPU_ACCEL_SCALE_OFFSET (SERVER_IP_OFFSET + SERVER_IP_LENGTH)
#define MPU_GYRO_SCALE_OFFSET (MPU_ACCEL_SCALE_OFFSET + MPU_ACCEL_SCALE_LENGTH)
#define MPU_SAMPLE_RATE_DIVIDER_OFFSET (MPU_GYRO_SCALE_OFFSET + MPU_GYRO_SCALE_LENGTH)
#define DATA_FREQ_OFFSET (MPU_SAMPLE_RATE_DIVIDER_OFFSET + MPU_SAMPLE_RATE_DIVIDER_LENGTH)
#define EEPROM_SIZE      (DATA_FREQ_OFFSET + DATA_FREQ_LENGTH)

// --- Đối tượng giao tiếp ---
WiFiUDP udp;
ESP8266WebServer server(80); // Sử dụng ESP8266WebServer

// --- Biến lưu trữ cấu hình (đọc/ghi từ EEPROM) ---
// --- Biến lưu trữ cấu hình (đọc/ghi từ EEPROM) ---
String node_id = ""; // Node name
String node_type = ""; // Sensor Type

String ap_ssid = "ESP32_AP";          // SSID của mạng WiFi cần kết nối (AP của ESP32), đọc từ EEPROM
String ap_pass = "12345678";          // Mật khẩu mạng WiFi, đọc từ EEPROM
IPAddress serverIP(192, 168, 4, 1);
uint8_t mpu_accel_scale = 0; // 0:±2g, 1:±4g, 2:±8g, 3:±16g
uint8_t mpu_gyro_scale = 0;  // 0:±250, 1:±500, 2:±1000, 3:±2000 dps
uint8_t mpu_sample_rate_divider = 7; // 1kHz / (1+7) = 125Hz
uint8_t data_send_frequency_hz = 10;

// --- Biến trạng thái và điều khiển luồng ---
bool isSoftAP = false;
unsigned long softAP_timeout_start = 0;
unsigned int dataPacketCounter = 0;
bool handshakeCompleted = false;
unsigned long lastHandshakeSendTime = 0;
unsigned long lastDataSendTime = 0;
unsigned long lastServerHeartbeatReceiveTime = 0;
int consecutiveHeartbeatFailures = 0;

// --- Biến đồng bộ thời gian ---
unsigned long serverMillisAtHandshake = 0;
unsigned long clientMillisAtHandshake = 0;

// --- Biến cho cảm biến MPU6050 ---
int16_t ax_raw, ay_raw, az_raw, gx_raw, gy_raw, gz_raw;
int32_t gx_offset = 0, gy_offset = 0, gz_offset = 0;
float bias_x = 0, bias_y = 0, bias_z = 0;

// =======================================================================
// --- Các hàm làm việc với EEPROM (Nâng cao) ---
// =======================================================================
void writeStringToEEPROM(int start, int length, String data) {
  for (int i = 0; i < length; i++) EEPROM.write(start + i, (i < data.length()) ? data[i] : 0);
}
String readStringFromEEPROM(int start, int length) {
  String r = "";
  for (int i = 0; i < length; i++) { char c = EEPROM.read(start + i); if (c == 0) break; r += c; }
  return r;
}
void writeIPToEEPROM(int start, IPAddress ip) { for (int i = 0; i < 4; i++) EEPROM.write(start + i, ip[i]); }
IPAddress readIPFromEEPROM(int start) { byte b[4]; for (int i = 0; i < 4; i++) b[i] = EEPROM.read(start + i); return IPAddress(b); }

void saveConfig() {
  Serial.println("Saving configuration to EEPROM...");
  writeStringToEEPROM(NODE_ID_OFFSET, NODE_ID_LENGTH, node_id);
  writeStringToEEPROM(NODE_TYPE_OFFSET, NODE_TYPE_LENGTH, node_type);
  writeStringToEEPROM(AP_SSID_OFFSET, AP_SSID_LENGTH, ap_ssid);
  writeStringToEEPROM(AP_PASS_OFFSET, AP_PASS_LENGTH, ap_pass);
  writeIPToEEPROM(SERVER_IP_OFFSET, serverIP);
  EEPROM.write(MPU_ACCEL_SCALE_OFFSET, mpu_accel_scale);
  EEPROM.write(MPU_GYRO_SCALE_OFFSET, mpu_gyro_scale);
  EEPROM.write(MPU_SAMPLE_RATE_DIVIDER_OFFSET, mpu_sample_rate_divider);
  EEPROM.write(DATA_FREQ_OFFSET, data_send_frequency_hz);
  EEPROM.commit();
  Serial.println("Configuration saved.");
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  
  node_id = readStringFromEEPROM(NODE_ID_OFFSET, NODE_ID_LENGTH);
  node_type = readStringFromEEPROM(NODE_TYPE_OFFSET, NODE_TYPE_LENGTH);
  ap_ssid = readStringFromEEPROM(AP_SSID_OFFSET, AP_SSID_LENGTH);
  ap_pass = readStringFromEEPROM(AP_PASS_OFFSET, AP_PASS_LENGTH);
  serverIP = readIPFromEEPROM(SERVER_IP_OFFSET);
  mpu_accel_scale = EEPROM.read(MPU_ACCEL_SCALE_OFFSET);
  mpu_gyro_scale = EEPROM.read(MPU_GYRO_SCALE_OFFSET);
  mpu_sample_rate_divider = EEPROM.read(MPU_SAMPLE_RATE_DIVIDER_OFFSET);
  data_send_frequency_hz = EEPROM.read(DATA_FREQ_OFFSET);

  // Validate and set defaults if necessary
  bool config_changed = false;
  if (node_id.length() == 0) { node_id = "ESP8266_Sensor_1"; config_changed = true; }
  if (node_type.length() == 0) { node_type = "MPU6050"; config_changed = true; }
  if (ap_ssid.length() == 0) { ap_ssid = "Your_WiFi_SSID"; config_changed = true; }
  if (serverIP[0] == 0 || serverIP[0] == 255) { serverIP = IPAddress(192, 168, 1, 100); config_changed = true; }
  if (mpu_accel_scale > 3) { mpu_accel_scale = 0; config_changed = true; }
  if (mpu_gyro_scale > 3) { mpu_gyro_scale = 0; config_changed = true; }
  if (data_send_frequency_hz == 0 || data_send_frequency_hz > 100) { data_send_frequency_hz = 10; config_changed = true; }
  
  if (config_changed) {
    Serial.println("Applying default values for some settings.");
    saveConfig();
  }
  Serial.println("\n--- Config Loaded ---");
  Serial.println("Node ID: " + node_id);
  Serial.println("Node Type: " + node_type);
  Serial.println("WiFi SSID: " + ap_ssid);
  Serial.println("Server IP: " + serverIP.toString());
  Serial.println("---------------------\n");
}

// =======================================================================
// --- Các hàm MPU6050 ---
// =======================================================================
void applyMpuConfig() {
  Serial.println("Applying MPU configuration...");
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x1B); Wire.write(mpu_gyro_scale << 3); Wire.endTransmission(true);
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x1C); Wire.write(mpu_accel_scale << 3); Wire.endTransmission(true);
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x19); Wire.write(mpu_sample_rate_divider); Wire.endTransmission(true);
  Serial.println("MPU configuration applied.");
}

void readRawData() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU_ADDR, (size_t)14, true);
  ax_raw = (Wire.read() << 8) | Wire.read();
  ay_raw = (Wire.read() << 8) | Wire.read();
  az_raw = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read(); // Bỏ qua 2 byte dữ liệu nhiệt độ
  gx_raw = (Wire.read() << 8) | Wire.read();
  gy_raw = (Wire.read() << 8) | Wire.read();
  gz_raw = (Wire.read() << 8) | Wire.read();
}

void calibrateSensors() {
  Serial.println("Calibrating gyro, keep still for 3s...");
  digitalWrite(LED_PIN, LOW); // LED ON
  gx_offset = 0; gy_offset = 0; gz_offset = 0;
  for (int i = 0; i < 300; i++) { readRawData(); gx_offset += gx_raw; gy_offset += gy_raw; gz_offset += gz_raw; delay(10); }
  gx_offset /= 300; gy_offset /= 300; gz_offset /= 300;
  digitalWrite(LED_PIN, HIGH); // LED OFF
  Serial.printf("Gyro offset: %ld, %ld, %ld\n", gx_offset, gy_offset, gz_offset);
}

float getAccelScaleFactor() {
  switch (mpu_accel_scale) {
    case 0: return 16384.0; case 1: return 8192.0;
    case 2: return 4096.0;  case 3: return 2048.0;
    default: return 16384.0;
  }
}
float getGyroScaleFactor() {
  switch (mpu_gyro_scale) {
    case 0: return 131.0; case 1: return 65.5;
    case 2: return 32.8;  case 3: return 16.4;
    default: return 131.0;
  }
}

// =======================================================================
// --- Các hàm Web Server Cấu hình (Nâng cao) ---
// =======================================================================
void handleRoot() {
  String s = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:Arial,sans-serif;background:#f4f4f4;margin:20px}form{background:white;padding:20px;border-radius:10px;box-shadow:0 2px 5px #ccc}h2{text-align:center}div{margin-bottom:15px}label{display:block;font-weight:bold;margin-bottom:5px}input,select{width:100%;padding:8px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px}button{width:100%;padding:10px;background:#007bff;color:white;border:none;border-radius:4px;cursor:pointer;font-size:16px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}</style></head><body><form method='POST' action='/save'><h2>ESP8266 Sensor Node Config</h2>";
  s += "<div><label>Node ID:</label><input name='node_id' value='" + node_id + "'></div>";
  s += "<div><label>Node Type:</label><input name='node_type' value='" + node_type + "'></div>";
  s += "<div><label>WiFi SSID:</label><input name='ap_ssid' value='" + ap_ssid + "'></div>";
  s += "<div><label>WiFi Password:</label><input type='password' name='ap_pass' value='" + ap_pass + "'></div>";
  s += "<div><label>Server IP:</label><input name='server_ip' value='" + serverIP.toString() + "'></div>";
  s += "<div class='grid'><label>Accel Scale:</label><select name='accel_scale'>";
  s += "<option value='0'" + String(mpu_accel_scale==0?" selected":"") + ">+/- 2g</option>";
  s += "<option value='1'" + String(mpu_accel_scale==1?" selected":"") + ">+/- 4g</option>";
  s += "<option value='2'" + String(mpu_accel_scale==2?" selected":"") + ">+/- 8g</option>";
  s += "<option value='3'" + String(mpu_accel_scale==3?" selected":"") + ">+/- 16g</option></select>";
  s += "<label>Gyro Scale:</label><select name='gyro_scale'>";
  s += "<option value='0'" + String(mpu_gyro_scale==0?" selected":"") + ">+/- 250dps</option>";
  s += "<option value='1'" + String(mpu_gyro_scale==1?" selected":"") + ">+/- 500dps</option>";
  s += "<option value='2'" + String(mpu_gyro_scale==2?" selected":"") + ">+/- 1000dps</option>";
  s += "<option value='3'" + String(mpu_gyro_scale==3?" selected":"") + ">+/- 2000dps</option></select></div>";
  s += "<div class='grid'><label>Sample Rate Divider (0-255):</label><input type='number' name='sample_div' value='" + String(mpu_sample_rate_divider) + "'>";
  s += "<label>Send Freq (Hz):</label><input type='number' name='freq' value='" + String(data_send_frequency_hz) + "'></div>";
  s += "<button type='submit'>Save & Restart</button></form></body></html>";
  server.send(200, "text/html", s);
}

void handleSave() {
  node_id = server.arg("node_id"); node_type = server.arg("node_type");
  ap_ssid = server.arg("ap_ssid"); ap_pass = server.arg("ap_pass");
  serverIP.fromString(server.arg("server_ip"));
  mpu_accel_scale = server.arg("accel_scale").toInt();
  mpu_gyro_scale = server.arg("gyro_scale").toInt();
  mpu_sample_rate_divider = server.arg("sample_div").toInt();
  data_send_frequency_hz = server.arg("freq").toInt();
  saveConfig();
  server.send(200, "text/html", "<html><body><h3>Configuration Saved. Restarting...</h3></body></html>");
  delay(1000); ESP.restart();
}

// =======================================================================
// --- Các hàm WiFi & Mạng (Nâng cao) ---
// =======================================================================
void startSoftAP_mode() {
  isSoftAP = true;
  String soft_ap_ssid = "ESP8266_Sensor_Config";
  WiFi.mode(WIFI_AP); WiFi.softAP(soft_ap_ssid.c_str());
  softAP_timeout_start = millis();
  Serial.println("\n--- Entering SoftAP mode ---");
  Serial.println("SSID: " + soft_ap_ssid);
  Serial.println("IP: " + WiFi.softAPIP().toString());
  server.on("/", HTTP_GET, handleRoot); 
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

bool connectWiFi() {
  isSoftAP = false; handshakeCompleted = false;
  WiFi.mode(WIFI_STA); WiFi.begin(ap_ssid.c_str(), ap_pass.c_str());
  Serial.print("Attempting to connect to WiFi: " + ap_ssid);
  for (int retries = 0; retries < 30 && WiFi.status() != WL_CONNECTED; retries++) {
    delay(500); Serial.print("."); digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
    udp.begin(UDP_PORT);
    lastServerHeartbeatReceiveTime = millis();
    consecutiveHeartbeatFailures = 0;
    return true;
  }
  Serial.println("\nWiFi connection FAILED!");
  return false;
}

void sendHandshake() {
  // Gói tin HELLO giờ đã ngắn gọn và đáng tin cậy hơn.
  char packet[100];
  snprintf(packet, sizeof(packet), "HELLO:%s:%s",
           node_id.c_str(), node_type.c_str());

  udp.beginPacket(serverIP, UDP_PORT);
  udp.print(packet);
  udp.endPacket();
  
  lastHandshakeSendTime = clientMillisAtHandshake = millis();
  Serial.println("Sent SIMPLIFIED HELLO: " + String(packet));
}

void checkUdpIncoming() {
  int packetSize = udp.parsePacket();
  if (!packetSize) return;

  char incomingPacket[255];
  int len = udp.read(incomingPacket, 254);
  incomingPacket[len] = 0;
  String packetData(incomingPacket);

  if (packetData.startsWith("SERVER_HEARTBEAT")) {
    Serial.println("Received SERVER_HEARTBEAT.");
    lastServerHeartbeatReceiveTime = millis();
    consecutiveHeartbeatFailures = 0;
  } else if (packetData.startsWith("WELCOME:")) {
    int firstColon = packetData.indexOf(':');
    int secondColon = packetData.indexOf(':', firstColon + 1);
    if (secondColon > -1) {
      String nodeIdFromServer = packetData.substring(firstColon + 1, secondColon);
      if (nodeIdFromServer == node_id) {
        serverMillisAtHandshake = packetData.substring(secondColon + 1).toInt();
        handshakeCompleted = true;
        dataPacketCounter = 0;
        lastServerHeartbeatReceiveTime = millis();
        Serial.println("Handshake completed! Server time: " + String(serverMillisAtHandshake));
      }
    }
  } else if (packetData.startsWith("CONFIG:")) {
    Serial.println("Received CONFIG command: " + packetData);
    // Format: CONFIG:<NodeID>:<Accel>:<Gyro>:<SampleDiv>:<Freq>
    char* str = (char*)packetData.c_str();
    strtok(str, ":"); // Skip "CONFIG"
    char* targetNodeId = strtok(NULL, ":");
    if (targetNodeId != NULL && String(targetNodeId) == node_id) {
        char* accelStr = strtok(NULL, ":");
        char* gyroStr = strtok(NULL, ":");
        char* divStr = strtok(NULL, ":");
        char* freqStr = strtok(NULL, ":");

        if (accelStr && gyroStr && divStr && freqStr) {
            mpu_accel_scale = atoi(accelStr);
            mpu_gyro_scale = atoi(gyroStr);
            mpu_sample_rate_divider = atoi(divStr);
            data_send_frequency_hz = atoi(freqStr);
            
            Serial.println("Applying new settings from server...");
            saveConfig();
            delay(200);
            ESP.restart();
        }
    }
  }
}

// =======================================================================
// --- Hàm Setup & Loop chính ---
// =======================================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT); 
  digitalWrite(LED_PIN, HIGH); // LED OFF on NodeMCU
  delay(100);
  digitalWrite(LED_PIN, LOW); // LED ON to show power
  
  loadConfig();
  
  Wire.begin(); // Default I2C pins for NodeMCU ESP8266 are D1 (SCL) and D2 (SDA)
  Wire.beginTransmission(MPU_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("MPU6050 not detected! Halting.");
    while (1) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(100); }
  }
  Serial.println("MPU6050 detected!");
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x6B); Wire.write(0); Wire.endTransmission(true); // Wake up
  applyMpuConfig();
  
  calibrateSensors();
  
  if (!connectWiFi()) {
    startSoftAP_mode();
  }
  Serial.println("\nSetup complete. Starting main loop...");
}

void loop() {
  if (isSoftAP) {
    server.handleClient();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(500);
    if (millis() - softAP_timeout_start > SOFTAP_CONFIG_TIMEOUT_MINUTES * 60000) {
      if (WiFi.softAPgetStationNum() == 0) {
        Serial.println("SoftAP timeout. Restarting...");
        ESP.restart();
      } else {
        softAP_timeout_start = millis(); // Reset timer if client is connected
      }
    }
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected. Reconnecting...");
      if (!connectWiFi()) startSoftAP_mode();
      return;
    }

    checkUdpIncoming();

    if (millis() - lastServerHeartbeatReceiveTime > SERVER_HEARTBEAT_TIMEOUT) {
      consecutiveHeartbeatFailures++;
      Serial.printf("Heartbeat timeout! Failures: %d\n", consecutiveHeartbeatFailures);
      lastServerHeartbeatReceiveTime = millis();
      if (consecutiveHeartbeatFailures >= MAX_HEARTBEAT_FAILURES) {
        Serial.println("Max heartbeat failures. Reconnecting WiFi...");
        if (!connectWiFi()) startSoftAP_mode();
        return;
      }
    }

    if (!handshakeCompleted) {
      if (millis() - lastHandshakeSendTime > HANDSHAKE_RETRY_INTERVAL) {
        sendHandshake();
      }
    } else {
      if (millis() - lastDataSendTime >= (1000 / data_send_frequency_hz)) {
        lastDataSendTime = millis();
        digitalWrite(LED_PIN, LOW); // LED ON

        readRawData();
        float ax = (ax_raw / getAccelScaleFactor()) - bias_x;
        float ay = (ay_raw / getAccelScaleFactor()) - bias_y;
        float az = (az_raw / getAccelScaleFactor()) - bias_z;
        float gx = (gx_raw - gx_offset) / getGyroScaleFactor();
        float gy = (gy_raw - gy_offset) / getGyroScaleFactor();
        float gz = (gz_raw - gz_offset) / getGyroScaleFactor();

        unsigned long currentServerMillis = serverMillisAtHandshake + (millis() - clientMillisAtHandshake);

        // *** THAY ĐỔI CHÍNH: TẠO CHUỖI JSON ***
        char packetBuffer[256];
        snprintf(packetBuffer, sizeof(packetBuffer),
                 "{\"id\":\"%s\",\"ts\":%lu,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f}",
                 node_id.c_str(), currentServerMillis,
                 ax, ay, az, gx, gy, gz);

        // Gửi gói tin UDP (vẫn giữ nguyên)
        udp.beginPacket(serverIP, UDP_PORT); 
        udp.print(packetBuffer); 
        udp.endPacket();
        
        // In ra Serial để chương trình Python đọc được
        Serial.println(packetBuffer);
        
        dataPacketCounter++;
        if(dataPacketCounter >= DATA_PACKET_RESET_THRESHOLD) {
            Serial.println("Packet threshold reached. Re-initiating handshake.");
            handshakeCompleted = false;
            dataPacketCounter = 0;
        }
        digitalWrite(LED_PIN, HIGH); // LED OFF
      }
    }
  }
}
