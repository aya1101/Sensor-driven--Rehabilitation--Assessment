#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Wire.h>

// IMPORTANT: Assuming you are using Jeff Rowberg's I2Cdevlib-MPU6050 or a compatible fork
// You might need to include MPU6050_6Axis_MotionApps20.h if you plan to use DMP.
// For basic raw data, I2Cdev.h and MPU6050.h are often sufficient.
#include <I2Cdev.h>
#include <MPU6050.h> // This is the MPU6050.h from I2Cdevlib (or Electronic Cats)

// --- Cấu hình chân và hằng số ---
#define LED_PIN 2         // Chân LED tích hợp trên ESP8266 (thường là GPIO2)

// --- EEPROM SIZES AND OFFSETS ---
// Định nghĩa độ dài và vị trí của từng trường trong EEPROM
// Việc này giúp dễ đọc, dễ bảo trì và tránh lỗi offset khi thay đổi cấu trúc
#define NODE_ID_LENGTH          16 // Increased length to accommodate "Name:Type" (e.g., 31 chars + ':' + 15 chars = 47, plus null)
#define NODE_TYPE_LENGTH        16 // Increased length to accommodate "Name:Type" (e.g., 31 chars + ':' + 15 chars = 47, plus null)
#define AP_SSID_LENGTH          32
#define AP_PASS_LENGTH          32
#define SERVER_IP_LENGTH        4 // For IPAddress object
#define MPU_ACCEL_SCALE_LENGTH  1
#define MPU_GYRO_SCALE_LENGTH   1
#define MPU_SAMPLE_RATE_DIVIDER_LENGTH 1
#define DATA_SEND_FREQUENCY_LENGTH 1

// Tính toán các offset dựa trên các độ dài đã định nghĩa
#define NODE_ID_OFFSET 0
#define NODE_TYPE_OFFSET (NODE_ID_OFFSET + NODE_ID_LENGTH)
#define AP_SSID_OFFSET (NODE_TYPE_OFFSET + NODE_TYPE_LENGTH)
#define AP_PASS_OFFSET (AP_SSID_OFFSET + AP_SSID_LENGTH)
#define SERVER_IP_OFFSET (AP_PASS_OFFSET + AP_PASS_LENGTH)
#define MPU_ACCEL_SCALE_OFFSET (SERVER_IP_OFFSET + SERVER_IP_LENGTH)
#define MPU_GYRO_SCALE_OFFSET (MPU_ACCEL_SCALE_OFFSET + MPU_ACCEL_SCALE_LENGTH)
#define MPU_SAMPLE_RATE_DIVIDER_OFFSET (MPU_GYRO_SCALE_OFFSET + MPU_GYRO_SCALE_LENGTH)
#define DATA_SEND_FREQUENCY_OFFSET (MPU_SAMPLE_RATE_DIVIDER_OFFSET + MPU_SAMPLE_RATE_DIVIDER_LENGTH)

// Tổng kích thước EEPROM cần thiết
#define EEPROM_SIZE (DATA_SEND_FREQUENCY_OFFSET + DATA_SEND_FREQUENCY_LENGTH)

#define UDP_PORT 1234       // Cổng UDP dùng để giao tiếp với Server (ESP32)

// Thời gian SoftAP sẽ chờ cấu hình trước khi thử kết nối lại (phút)
#define SOFTAP_CONFIG_TIMEOUT_MINUTES 1

// --- Đối tượng phần cứng/giao tiếp ---
MPU6050 mpu;                  // Đối tượng MPU6050
WiFiUDP udp;                  // Đối tượng UDP
ESP8266WebServer server(80);  // Đối tượng Web Server cho cấu hình


// --- Biến lưu trữ cấu hình (đọc/ghi từ EEPROM) ---
String node_id = ""; // Node name
String node_type = ""; // Sensor Type

String ap_ssid = "";          // SSID của mạng WiFi cần kết nối (AP của ESP32), đọc từ EEPROM
String ap_pass = "";          // Mật khẩu mạng WiFi, đọc từ EEPROM
IPAddress serverIP;           // Địa chỉ IP của Server SoftAP, đọc từ EEPROM hoặc cấu hình qua web

// MPU6050 Configuration variables
uint8_t mpu_accel_scale = MPU6050_ACCEL_FS_8;  // Default +/- 8g
uint8_t mpu_gyro_scale = MPU6050_GYRO_FS_1000; // Default +/- 1000 deg/s
uint8_t mpu_sample_rate_divider = 7;         // Default 125 Hz (1kHz / (1+7))
uint8_t data_send_frequency_hz = 10; // Default 10Hz. Changed name to clarify it's in Hz
unsigned long dataSendInterval = 100; // millisecond Calculated in setup()

// --- Biến trạng thái và kiểm soát luồng ---
bool isSoftAP = false;        // Cờ báo hiệu đang ở chế độ SoftAP (cấu hình)
unsigned long softAP_timeout_start = 0; // Thời điểm bắt đầu chế độ SoftAP
unsigned int DataPacketCounter = 0; // NEW: Biến đếm số gói dữ liệu đã gửi
const unsigned int DATA_PACKET_RESET_THRESHOLD = 200; // NEW: Ngưỡng để reset handshake

// --- Biến trạng thái LED ---
unsigned long lastLedToggleTime = 0;
bool ledState = HIGH; // Mặc định LED OFF (cho NodeMCU và ESP-01)

// --- Biến điều khiển Handshake và gửi dữ liệu ---
bool handshakeCompleted = false;      // Cờ báo hiệu handshake đã hoàn tất
unsigned long lastHandshakeSendTime = 0;
const unsigned long HANDSHAKE_RETRY_INTERVAL = 5000; // Thử gửi handshake lại mỗi 5 giây nếu chưa nhận được WELCOME

unsigned long lastDataSendTime = 0; // Declared lastDataSendTime

// --- Biến đồng bộ thời gian ---
unsigned long serverMillisAtHandshake = 0; // Timestamp của Server tại thời điểm handshake cuối cùng
unsigned long clientMillisAtHandshake = 0; // Thời điểm client thực hiện handshake (millis() của client)

// --- Biến điều khiển Heartbeat từ Server ---
unsigned long lastServerHeartbeatReceiveTime = 0; // Thời điểm nhận được SERVER_HEARTBEAT cuối cùng
const unsigned long SERVER_HEARTBEAT_TIMEOUT = 3000; // Thời gian chờ SERVER_HEARTBEAT tối đa (3s)
int consecutiveHeartbeatFailures = 0;
const int MAX_HEARTBEAT_FAILURES = 5; // Số lần thất bại liên tiếp trước khi coi server đã chết

// =======================================================================
// --- EEPROM Functions ---
// =======================================================================

// Ghi một chuỗi vào EEPROM tại vị trí start với độ dài cố định.
// Các byte còn lại (nếu chuỗi ngắn hơn length) sẽ được ghi 0 (null terminator).
void writeStringToEEPROM(int start, int length, String data) {
  for (int i = 0; i < length; i++) {
    if (i < data.length()) {
      EEPROM.write(start + i, data[i]);
    } else {
      EEPROM.write(start + i, 0); // Ghi ký tự null để kết thúc chuỗi
    }
  }
  EEPROM.commit(); // Ghi thay đổi vào flash
}

// Đọc một chuỗi từ EEPROM tại vị trí start với độ dài cố định.
// Dừng đọc khi gặp ký tự null.
String readStringFromEEPROM(int start, int int_length) {
  String result = "";
  for (int i = 0; i < int_length; i++) {
    char c = char(EEPROM.read(start + i));
    if (c == 0) { // Dừng khi gặp ký tự null (kết thúc chuỗi)
      break;
    }
    result += c;
  }
  return result;
}

// Ghi địa chỉ IP vào EEPROM (4 byte)
void writeIPToEEPROM(int start, IPAddress ip) {
  for (int i = 0; i < 4; i++) {
    EEPROM.write(start + i, ip[i]);
  }
  EEPROM.commit(); // Ghi thay đổi vào flash
}

// Đọc địa chỉ IP từ EEPROM (4 byte)
IPAddress readIPFromEEPROM(int start) {
  byte ipBytes[4];
  for (int i = 0; i < 4; i++) {
    ipBytes[i] = EEPROM.read(start + i);
  }
  return IPAddress(ipBytes[0], ipBytes[1], ipBytes[2], ipBytes[3]);
}

// Ghi 1 byte (uint8_t) vào EEPROM
void writeByteToEEPROM(int address, uint8_t value) {
  EEPROM.write(address, value);
  EEPROM.commit();
}

// Đọc 1 byte (uint8_t) từ EEPROM
uint8_t readByteFromEEPROM(int address) {
  return EEPROM.read(address);
}

// Tải cấu hình từ EEPROM và áp dụng giá trị mặc định nếu dữ liệu không hợp lệ/trống
void loadConfig() {
  EEPROM.begin(EEPROM_SIZE); // Bắt đầu EEPROM (cần thiết mỗi khi truy cập)

  // Load node_id directly
  node_id = readStringFromEEPROM(NODE_ID_OFFSET, NODE_ID_LENGTH);
  node_type = readStringFromEEPROM(NODE_TYPE_OFFSET, NODE_TYPE_LENGTH);
  ap_ssid = readStringFromEEPROM(AP_SSID_OFFSET, AP_SSID_LENGTH);
  ap_pass = readStringFromEEPROM(AP_PASS_OFFSET, AP_PASS_LENGTH);
  serverIP = readIPFromEEPROM(SERVER_IP_OFFSET);
  mpu_accel_scale = readByteFromEEPROM(MPU_ACCEL_SCALE_OFFSET);
  mpu_gyro_scale = readByteFromEEPROM(MPU_GYRO_SCALE_OFFSET);
  mpu_sample_rate_divider = readByteFromEEPROM(MPU_SAMPLE_RATE_DIVIDER_OFFSET);
  data_send_frequency_hz = readByteFromEEPROM(DATA_SEND_FREQUENCY_OFFSET);

  // --- DEBUG: In ra giá trị đọc được từ EEPROM ---
  Serial.println("\n--- Config loaded from EEPROM ---");
  Serial.println("Node ID raw read: '" + node_id + "'");
  Serial.println("Node Type raw read: '" + node_type + "'");
  Serial.println("AP SSID raw read: '" + ap_ssid + "'");
  Serial.println("AP Pass raw read: '" + ap_pass + "'"); // In ra pass raw (chỉ để debug nội bộ)
  Serial.println("Server IP raw read: " + serverIP.toString());
  Serial.println("MPU Accel Scale raw read: " + String(mpu_accel_scale));
  Serial.println("MPU Gyro Scale raw read: " + String(mpu_gyro_scale));
  Serial.println("MPU Sample Rate Divider raw read: " + String(mpu_sample_rate_divider));
  Serial.println("Data sending to server at " + String(data_send_frequency_hz) +" Hz");
  Serial.println("---------------------------------\n");

  // Kiểm tra tính hợp lệ và gán giá trị mặc định nếu cần
  bool config_changed = false; // Cờ để kiểm tra xem có cần lưu lại cấu hình không

  node_id.trim(); // Loại bỏ khoảng trắng ở đầu và cuối chuỗi node_id
  // Validate format and length for node_id (e.g., "name:type")  
  if (node_id.length() == 0 || node_id.length() > (NODE_ID_LENGTH - 1)) {
    Serial.println("Node ID invalid (empty or too long), setting default.");
    node_id = "Sensor_1"; // DEFAULT NODE ID
    config_changed = true;
  }
  
  node_type.trim(); // Loại bỏ khoảng trắng ở đầu và cuối chuỗi node_id
  // Validate format and length for node_id (e.g., "name:type")  
  if (node_type.length() == 0 || node_type.length() > (NODE_TYPE_LENGTH - 1)) {
    Serial.println("Node type invalid (empty or too long), setting default.");
    node_id = "MPU6050"; // DEFAULT NODE ID
    config_changed = true;
  }
  ap_ssid.trim();
  if (ap_ssid.length() == 0 || ap_ssid.length() > (AP_SSID_LENGTH - 1)) {
    Serial.println("AP SSID invalid or empty or too long, setting default.");
    ap_ssid = "ESP32_AP";
    config_changed = true;
  }
  ap_pass.trim();
  if (ap_pass.length() == 0 || ap_pass.length() > (AP_PASS_LENGTH - 1)) {
    Serial.println("AP Password invalid (empty or too long), setting default.");
    ap_pass = "12345678";
    config_changed = true;
  }
  
  if (serverIP == IPAddress(0,0,0,0) || serverIP[0] == 255 || serverIP[0] == 0) {
    Serial.println("Server IP invalid or 0.0.0.0, setting default.");
    serverIP = IPAddress(192, 168, 4, 1);
    config_changed = true;
  }
  
  // Ensure MPU scale values are within valid range (0-3 for MPU6050_ACCEL_FS_X and MPU6050_GYRO_FS_X)
  if (mpu_accel_scale > 3) {
    Serial.println("MPU Accel Scale invalid, setting default.");
    mpu_accel_scale = MPU6050_ACCEL_FS_8;
    config_changed = true;
  }
  if (mpu_gyro_scale > 3) {
    Serial.println("MPU Gyro Scale invalid, setting default.");
    mpu_gyro_scale = MPU6050_GYRO_FS_1000;
    config_changed = true;
  }
  if (mpu_sample_rate_divider > 255) {
    Serial.println("MPU Sample Rate Divider invalid, setting default.");
    mpu_sample_rate_divider = 7;
    config_changed = true;
  }

  if (data_send_frequency_hz == 0 || data_send_frequency_hz > 255) {
    Serial.println("Data sending frequency invalid (0 or >255), setting default 10Hz.");
    data_send_frequency_hz = 10;
    config_changed = true;
  }

  float mpu_actual_sample_rate_hz = 1000.0 / (1.0 + mpu_sample_rate_divider);
  if (data_send_frequency_hz > (uint8_t)floor(mpu_actual_sample_rate_hz)) {
      Serial.print("Warning: data_send_frequency_hz (");
      Serial.print(data_send_frequency_hz);
      Serial.print(" Hz) is faster than MPU sample rate (");
      Serial.print(mpu_actual_sample_rate_hz);
      Serial.println(" Hz). Adjusting to MPU sample rate.");
      data_send_frequency_hz = (uint8_t)floor(mpu_actual_sample_rate_hz);
      if (data_send_frequency_hz == 0) data_send_frequency_hz = 1;
      config_changed = true;
  }

  // Chỉ lưu lại cấu hình nếu có bất kỳ thay đổi nào
  if (config_changed) {
    saveConfig(node_id, node_type, ap_ssid, ap_pass, serverIP, mpu_accel_scale, mpu_gyro_scale, mpu_sample_rate_divider, data_send_frequency_hz);
    Serial.println("Default configurations applied and saved to EEPROM.");
  }
}

// Lưu cấu hình vào EEPROM
void saveConfig(String id, String type, String ssid, String pass, IPAddress ip, uint8_t accel_s, uint8_t gyro_s, uint8_t sample_div, uint8_t frequency_hz) {
  EEPROM.begin(EEPROM_SIZE); // Đảm bảo EEPROM được khởi tạo trước khi ghi

  writeStringToEEPROM(NODE_ID_OFFSET, NODE_ID_LENGTH, id); 
  writeStringToEEPROM(NODE_TYPE_OFFSET, NODE_TYPE_LENGTH, type); 
  writeStringToEEPROM(AP_SSID_OFFSET, AP_SSID_LENGTH, ssid);
  writeStringToEEPROM(AP_PASS_OFFSET, AP_PASS_LENGTH, pass);
  writeIPToEEPROM(SERVER_IP_OFFSET, ip);
  writeByteToEEPROM(MPU_ACCEL_SCALE_OFFSET, accel_s);
  writeByteToEEPROM(MPU_GYRO_SCALE_OFFSET, gyro_s);
  writeByteToEEPROM(MPU_SAMPLE_RATE_DIVIDER_OFFSET, sample_div);
  writeByteToEEPROM(DATA_SEND_FREQUENCY_OFFSET, frequency_hz);

  EEPROM.commit(); // Ghi thay đổi vào flash

  Serial.println("\n--- Config saved to EEPROM ---");
  Serial.println("Saved Node ID: '" + id + "'");
  Serial.println("Saved Type: '" + type + "'");
  Serial.println("Saved AP SSID: '" + ssid + "'");
  Serial.println("Saved AP Pass Length: " + String(pass.length()) + " (value not printed for security)");
  Serial.println("Saved Server IP: " + ip.toString());
  Serial.println("Saved MPU Accel Scale: " + String(accel_s));
  Serial.println("Saved MPU Gyro Scale: " + String(gyro_s));
  Serial.println("Saved MPU Sample Rate Divider: " + String(sample_div));
  Serial.println("Data is sending at " + String(frequency_hz) + " Hz");
  Serial.println("------------------------------\n");
}

// =======================================================================
// --- LED Functions ---
// =======================================================================

// Nháy LED một số lần nhất định
void blinkLED(int times, int delayMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW); // LED ON (thường là LOW cho ESP8266 built-in LED)
    delay(delayMs);
    digitalWrite(LED_PIN, HIGH); // LED OFF
    delay(delayMs);
  }
}

// Cập nhật trạng thái LED nhấp nháy định kỳ
void updateLED(int delayMs) {
  if (millis() - lastLedToggleTime >= delayMs) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
    lastLedToggleTime = millis();
  }
}

// =======================================================================
// --- Web Config Handlers ---
// =======================================================================

// Xử lý yêu cầu HTTP GET cho trang gốc (trang cấu hình)
void handleRoot() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>"
                "body { font-family: Arial; background:#f0f0f0; display:flex; justify-content:center; align-items:center; min-height:100vh; margin:0; }"
                "form { background:white; padding:20px; border-radius:8px; box-shadow:0 0 10px #ccc; max-width:380px; width:100%; box-sizing:border-box; }"
                "h2 { text-align: center; color: #333; margin-bottom: 20px; }"
                "label { display: block; margin-top: 10px; font-weight: bold; color: #555; }"
                "input[type='text'], input[type='password'], select, input[type='number'] { width:calc(100% - 22px); padding:10px; margin-top:5px; margin-bottom:10px; font-size:16px; box-sizing:border-box; border:1px solid #ddd; border-radius:4px; }"
                "button { width:100%; padding:10px; margin:10px 0; background-color:#4CAF50; color:white; border:none; border-radius:5px; cursor:pointer; font-size:16px; }"
                "button:hover { background-color:#45a049; }"
                ".config-section { border: 1px solid #e0e0e0; padding: 15px; border-radius: 6px; margin-top: 20px; background: #f9f9f9; }"
                ".config-section h3 { margin-top: 0; color: #333; text-align: center; border-bottom: 1px solid #ddd; padding-bottom: 10px; margin-bottom: 15px; }"
                "hr { border: none; border-top: 1px solid #ccc; margin: 20px 0; }"
                "</style></head><body>";
  html += "<form method='POST' action='/save'>";
  html += "<h2>Config Sensor Node</h2>";
  
  html += "<label for='node_id'>Node ID (e.g., 'Sensor_0123', max " + String(NODE_ID_LENGTH - 1) + " chars):</label><input type='text' id='node_id' name='node_id' value='" + node_id + "' maxlength='" + String(NODE_ID_LENGTH - 1) + "' required>";
  html += "<p style='font-size: 0.9em; color: #666; margin-top: -8px;'>*Enter a unique identifier for your sensor node.</p>";
  
  html += "<label for='node_type'>Node Type (e.g., 'MPU6050', max " + String(NODE_TYPE_LENGTH - 1) + " chars):</label><input type='text' id='node_type' name='node_type' value='" + node_type + "' maxlength='" + String(NODE_TYPE_LENGTH - 1) + "' required>";
  html += "<p style='font-size: 0.9em; color: #666; margin-top: -8px;'>*Describe the type of sensor (e.g., MPU6050, DHT11).</p>";

  html += "<hr>";
  html += "<label for='ap_ssid'>WiFi AP SSID (max " + String(AP_SSID_LENGTH - 1) + " chars):</label><input type='text' id='ap_ssid' name='ap_ssid' value='" + ap_ssid + "' maxlength='" + String(AP_SSID_LENGTH - 1) + "' required>";
  html += "<label for='ap_pass'>WiFi AP Password (max " + String(AP_PASS_LENGTH - 1) + " chars):</label><input type='password' id='ap_pass' name='ap_pass' value='" + ap_pass + "' maxlength='" + String(AP_PASS_LENGTH - 1) + "'>";
  html += "<label for='server_ip'>Server IP:</label><input type='text' id='server_ip' name='server_ip' value='" + serverIP.toString() + "' required pattern='^(?:[0-9]{1,3}\\.){3}[0-9]{1,3}$'>"; // Basic IP validation

  // MPU6050 Config fields
  html += "<div id='mpu6050_config_div' class='config-section'>"; // Always visible, as MPU6050 is the assumed sensor
  html += "<h3>MPU6050 Specific Configuration</h3>";
  html += "<label for='accel_scale'>Accelerometer Scale:</label><select id='accel_scale' name='accel_scale'>";
  html += String("")+ "<option value='0'" + (mpu_accel_scale == MPU6050_ACCEL_FS_2 ? " selected" : "") + ">+/- 2g</option>";
  html += String("")+"<option value='1'" + (mpu_accel_scale == MPU6050_ACCEL_FS_4 ? " selected" : "") + ">+/- 4g</option>";
  html += String("")+"<option value='2'" + (mpu_accel_scale == MPU6050_ACCEL_FS_8 ? " selected" : "") + ">+/- 8g</option>";
  html += String("")+"<option value='3'" + (mpu_accel_scale == MPU6050_ACCEL_FS_16 ? " selected" : "") + ">+/- 16g</option>";
  html += "</select>";

  html += "<label for='gyro_scale'>Gyroscope Scale:</label><select id='gyro_scale' name='gyro_scale'>";
  html += String("")+"<option value='0'" + (mpu_gyro_scale == MPU6050_GYRO_FS_250 ? " selected" : "") + ">+/- 250 deg/s</option>";
  html += String("")+"<option value='1'" + (mpu_gyro_scale == MPU6050_GYRO_FS_500 ? " selected" : "") + ">+/- 500 deg/s</option>";
  html += String("")+"<option value='2'" + (mpu_gyro_scale == MPU6050_GYRO_FS_1000 ? " selected" : "") + ">+/- 1000 deg/s</option>";
  html += String("")+"<option value='3'" + (mpu_gyro_scale == MPU6050_GYRO_FS_2000 ? " selected" : "") + ">+/- 2000 deg/s</option>";
  html += "</select>";

  html += "<label for='sample_rate_divider'>MPU Sample Rate Divider (0-255):</label><input type='number' id='sample_rate_divider' name='sample_rate_divider' min='0' max='255' value='" + String(mpu_sample_rate_divider) + "'>";
  html += "</div>"; // End of mpu6050_config_div


  html += "<hr>";
  html += "<label for='data_send_frequency'>Data Sending Frequency (1-255 Hz):</label><input type='number' id='data_send_frequency' name='data_send_frequency' min='1' max='255' value='" + String(data_send_frequency_hz) + "'>";

  html += "<button type='submit'>Save & Restart</button>";
  html += "</form>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// Xử lý yêu cầu HTTP POST khi người dùng lưu cấu hình
void handleSave() {
  node_id = server.arg("node_id"); // Read node_id directly from form
  node_type = server.arg("node_type"); // Read node_id directly from form
  ap_ssid = server.arg("ap_ssid");
  ap_pass = server.arg("ap_pass");

  String ipStr = server.arg("server_ip");
  IPAddress newServerIP;
  if (!newServerIP.fromString(ipStr)) {
    Serial.println("Invalid server IP format received from web config!");
    server.send(200, "text/html", "<html><body><h3>Invalid Server IP format! Please go back and correct.</h3></body></html>");
    return;
  }
  serverIP = newServerIP;

  // Parse MPU config from web form
  mpu_accel_scale = server.arg("accel_scale").toInt();
  mpu_gyro_scale = server.arg("gyro_scale").toInt();
  mpu_sample_rate_divider = server.arg("sample_rate_divider").toInt();
  data_send_frequency_hz = server.arg("data_send_frequency").toInt();

  // Basic validation for values coming from web
  if (mpu_accel_scale > 3) mpu_accel_scale = MPU6050_ACCEL_FS_8;
  if (mpu_gyro_scale > 3) mpu_gyro_scale = MPU6050_GYRO_FS_1000;
  if (mpu_sample_rate_divider > 255) mpu_sample_rate_divider = 7;

  if (data_send_frequency_hz == 0 || data_send_frequency_hz > 255) {
    data_send_frequency_hz = 10;
  }

  // Enforce data_send_frequency_hz <= MPU sample rate
  float mpu_actual_sample_rate_hz = 1000.0 / (1.0 + mpu_sample_rate_divider);
  if (data_send_frequency_hz > (uint8_t)floor(mpu_actual_sample_rate_hz)) {
      Serial.print("Warning: data_send_frequency_hz (");
      Serial.print(data_send_frequency_hz);
      Serial.print(" Hz) is faster than MPU sample rate (");
      Serial.print(mpu_actual_sample_rate_hz);
      Serial.println(" Hz). Adjusting to MPU sample rate.");
      data_send_frequency_hz = (uint8_t)floor(mpu_actual_sample_rate_hz);
      if (data_send_frequency_hz == 0) data_send_frequency_hz = 1;
  }

  // Truncate strings if they exceed the defined EEPROM lengths
  if (node_id.length() >= NODE_ID_LENGTH) {
    node_id = node_id.substring(0, NODE_ID_LENGTH - 1); // -1 for null terminator
  }
  if (node_type.length() >= NODE_TYPE_LENGTH) {
    node_type = node_type.substring(0, NODE_TYPE_LENGTH - 1); // -1 for null terminator
  }
  if (ap_ssid.length() >= AP_SSID_LENGTH) {
    ap_ssid = ap_ssid.substring(0, AP_SSID_LENGTH - 1);
  }
  if (ap_pass.length() >= AP_PASS_LENGTH) {
    ap_pass = ap_pass.substring(0, AP_PASS_LENGTH - 1);
  }

  
  saveConfig(node_id, node_type, ap_ssid, ap_pass, serverIP, mpu_accel_scale, mpu_gyro_scale, mpu_sample_rate_divider, data_send_frequency_hz);

  server.send(200, "text/html", "<html><body><h3>Configuration Saved. Restarting...</h3></body></html>");
  delay(200); // Add a small delay to allow browser to receive the response
  ESP.restart(); // Khởi động lại để áp dụng cấu hình mới
}

// =======================================================================
// --- WiFi Functions ---
// =======================================================================

// Kết nối ESP8266 với mạng WiFi (chế độ STA)
// Trả về true nếu kết nối thành công, false nếu thất bại
bool connectWiFi(int max_retries) {
  Serial.print("\nAttempting to connect to WiFi AP: ");
  Serial.println(ap_ssid);
  handshakeCompleted = false; // Reset handshake status on every new connection attempt

  // --- DEBUG: In ra SSID và Password ngay trước khi WiFi.begin() ---
  Serial.println("--- Debugging WiFi.begin() parameters ---");
  Serial.print("Target SSID: '"); Serial.print(ap_ssid); Serial.println("'");
  Serial.print("Target Pass: '"); Serial.print(ap_pass); Serial.println("'"); // In mật khẩu đầy đủ cho debug
  Serial.println("-----------------------------------------");

  // ********** CÁC THAY ĐỔI QUAN TRỌNG ĐỂ XÓA TRẠNG THÁI "KẸT" CỦA WIFI **********
  Serial.println("Resetting WiFi module for a clean start...");
  WiFi.mode(WIFI_OFF); // Tắt hoàn toàn WiFi
  delay(1000); // Cho phép module tắt hẳn
  WiFi.forceSleepWake(); // Buộc module đi ngủ và thức dậy (cũng giúp làm sạch trạng thái)
  delay(1000); // Cho phép module thức dậy
  WiFi.mode(WIFI_STA); // Chuyển sang chế độ Station (client)
  delay(1000); // Đảm bảo chế độ đã được đặt

  Serial.println("Ngắt mọi kết nối Wifi và quên AP đã lưu (lần 2 để chắc chắn)...");
  WiFi.disconnect(true, true);
  delay(1000); // Tăng thời gian chờ sau disconnect để ổn định hơn

  Serial.print("Đang kết nối tới ");
  Serial.println(ap_ssid);

  WiFi.begin(ap_ssid.c_str(), ap_pass.c_str()); // Bắt đầu kết nối WiFi
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < max_retries) {
    delay(500); // Đợi 0.5 giây
    retry++;
    Serial.print(".");
    updateLED(50); // Nháy nhanh LED để báo hiệu đang cố gắng kết nối
  }
  Serial.println(); // Xuống dòng sau khi in các dấu chấm

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected successfully!");
    Serial.print("Assigned IP: ");
    Serial.println(WiFi.localIP());
    udp.begin(UDP_PORT); // Khởi tạo UDP sau khi kết nối WiFi thành công
    lastServerHeartbeatReceiveTime = millis(); // Reset heartbeat timer upon successful WiFi connection
    consecutiveHeartbeatFailures = 0; // Reset heartbeat failure counter
    DataPacketCounter = 0; // Reset data packet counter
    return true;
  } else {
    Serial.println("WiFi connection FAILED!");
    Serial.print("WiFi Status Code: "); // In mã trạng thái WiFi khi thất bại
    Serial.println(WiFi.status());
    return false;
  }
}

// =======================================================================
// --- Handshake Functions ---
// =======================================================================

// Gửi gói tin HELLO tới Server để bắt đầu quá trình handshake (bao gồm MPU config)
void sendHandshake() {
  String handshakePacket = "HELLO:" + node_id +
                            ":" + node_type +
                            ":" + String(mpu_accel_scale) +
                            ":" + String(mpu_gyro_scale) +
                            ":" + String(mpu_sample_rate_divider);
                            ":" + String(data_send_frequency_hz);
  udp.beginPacket(serverIP, UDP_PORT);
  udp.print(handshakePacket);
  udp.endPacket();
  lastHandshakeSendTime = millis(); // Cập nhật thời gian gửi gói HELLO
  clientMillisAtHandshake = millis(); // Ghi lại millis() của client tại thời điểm gửi HELLO
  Serial.println("Sent HELLO for handshaking: " + handshakePacket);
}

// Hàm để áp dụng cấu hình MPU mới
void applyMpuConfig(uint8_t accel_s, uint8_t gyro_s, uint8_t sample_div) {
  Serial.println("Applying new MPU configuration...");
  mpu.setFullScaleAccelRange(accel_s);
  mpu.setFullScaleGyroRange(gyro_s);
  mpu.setRate(sample_div);

  mpu_accel_scale = accel_s; // Update global config vars
  mpu_gyro_scale = gyro_s;   // Update global config vars
  mpu_sample_rate_divider = sample_div; // Update global config vars


  Serial.print("New Accel Scale: "); Serial.println(mpu_accel_scale);
  Serial.print("New Gyro Scale: "); Serial.println(mpu_gyro_scale);
  Serial.print("New Sample Rate Divider: "); Serial.println(mpu_sample_rate_divider);
}


// Kiểm tra và xử lý các gói tin UDP đến (đặc biệt là gói WELCOME và CONFIG từ Server)
void checkUdpIncoming() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incomingPacket[255];
    int len = udp.read(incomingPacket, sizeof(incomingPacket) - 1);
    incomingPacket[len] = 0; // Đảm bảo chuỗi kết thúc bằng null
    String packetData = String(incomingPacket);

    // NEW: Handle SERVER_HEARTBEAT from server
    if (packetData.startsWith("SERVER_HEARTBEAT:")) {
      int firstColon = packetData.indexOf(":");
      if (firstColon != -1) {
        Serial.println("Received " + packetData +" from Server.");
        DataPacketCounter =0 ;
        lastServerHeartbeatReceiveTime = millis(); // Reset heartbeat timer
        consecutiveHeartbeatFailures = 0; // Reset failure counter
      }
    }
    else if (packetData.startsWith("WELCOME:")) {
      int firstColon = packetData.indexOf(":");
      int secondColon = packetData.indexOf(":", firstColon + 1);
      if (firstColon != -1 && secondColon != -1) {
        String nodeIdFromServer = packetData.substring(firstColon + 1, secondColon);
        String serverMillisStr = packetData.substring(secondColon + 1);

        if (nodeIdFromServer == node_id) { // Chỉ chấp nhận WELCOME cho node_id của mình
          serverMillisAtHandshake = serverMillisStr.toInt(); // LƯU GIÁ TRỊ MILLIS() CỦA SERVER
          handshakeCompleted = true; // Đánh dấu handshake đã hoàn tất
          DataPacketCounter = 0; // Reset data packet counter on successful handshake
          Serial.print("Received WELCOME from Server. Handshake completed! Server millis at handshake: ");
          Serial.println(serverMillisAtHandshake);
          lastServerHeartbeatReceiveTime = millis(); // Reset heartbeat timer as welcome also confirms server presence
          consecutiveHeartbeatFailures = 0; // Reset failure counter
        }
      }
    }
    // Handle CONFIG command from server
    else if (packetData.startsWith("CONFIG:")) {
      Serial.println("Received CONFIG command: " + packetData);
      // Expected format: CONFIG:<NodeID>:<AccelScale>:<GyroScale>:<SampleRateDivider>:<DataSendFrequency>

      int pos1 = packetData.indexOf(":");
      int pos2 = packetData.indexOf(":", pos1 + 1);
      int pos3 = packetData.indexOf(":", pos2 + 1);
      int pos4 = packetData.indexOf(":", pos3 + 1);
      int pos5 = packetData.indexOf(":", pos4 + 1);
      

      if (pos1 != -1 && pos2 != -1 && pos3 != -1 && pos4 != -1 && pos5 != -1) {
        String targetNodeId = packetData.substring(pos1 + 1, pos2);

        if (targetNodeId == node_id) { // Only process if command is for this node
          uint8_t newAccelScale = packetData.substring(pos2 + 1, pos3).toInt();
          uint8_t newGyroScale = packetData.substring(pos3 + 1, pos4).toInt();
          uint8_t newSampleRateDivider = packetData.substring(pos4 + 1, pos5).toInt();
          uint8_t newDataSendFrequency = packetData.substring(pos5 + 1).toInt();

          // Basic validation to prevent out-of-bounds values from server
          if (newAccelScale > 3) newAccelScale = mpu_accel_scale; // Keep current if invalid
          if (newGyroScale > 3) newGyroScale = mpu_gyro_scale;   // Keep current if invalid
          if ((newSampleRateDivider < 1)||(newSampleRateDivider > 255)) newSampleRateDivider = mpu_sample_rate_divider; // Keep current if invalid
          if ((newDataSendFrequency < 1)||(newDataSendFrequency > 255)) newDataSendFrequency = data_send_frequency_hz; // Keep current if invalid

          Serial.println("Applying new MPU settings from server command.");
          applyMpuConfig(newAccelScale, newGyroScale, newSampleRateDivider);
          saveConfig(node_id, node_type, ap_ssid, ap_pass, serverIP, newAccelScale, newGyroScale, newSampleRateDivider, newDataSendFrequency);
          delay(200);
          ESP.restart(); // Khởi động lại để áp dụng cấu hình mới
        }
      } else {
        Serial.println("Invalid CONFIG command format.");
      }
    }
    else {
      Serial.print("Received unknown UDP: ");
      Serial.println(packetData);
    }
  }
}

// Khởi động chế độ SoftAP để cấu hình WiFi qua Web
void startSoftAP_mode() {
  isSoftAP = true; // Đặt cờ đang ở chế độ SoftAP
  WiFi.mode(WIFI_AP); // Chuyển sang chế độ Access Point

  // Use node_id (which is machine_1:MPU6050) for the SoftAP SSID
  String ap_ssid_config = node_id;
  // Ensure the AP SSID isn't too long
  if (ap_ssid_config.length() > 31) { // Max SSID length is 31
    ap_ssid_config = ap_ssid_config.substring(0, 31);
  }

  // Provide a default password for SoftAP if node_id is too short for a password
  String ap_pass_config = (ap_ssid_config.length() >= 8) ? ap_ssid_config : "config123";
  if (ap_pass_config.length() > (AP_PASS_LENGTH - 1)) {
    ap_pass_config = ap_pass_config.substring(0, AP_PASS_LENGTH - 1);
  }

  WiFi.softAP(ap_ssid_config.c_str(), ap_pass_config.c_str());

  softAP_timeout_start = millis(); // Ghi lại thời điểm bắt đầu SoftAP

  Serial.println("\n--- Entering SoftAP mode for configuration ---");
  Serial.println("Connect to WiFi SSID: " + ap_ssid_config);
  Serial.println("Password (if any): " + ap_pass_config);
  Serial.println("Access configuration at IP: " + WiFi.softAPIP().toString());
  Serial.println("----------------------------------------------\n");

  server.on("/", handleRoot); // Đăng ký handler cho trang gốc
  server.on("/save", HTTP_POST, handleSave); // Đăng ký handler cho việc lưu cấu hình
  server.begin(); // Bắt đầu Web Server

  lastLedToggleTime = millis(); // Reset thời gian cho LED
  ledState = HIGH; // Bắt đầu ở trạng thái OFF (nháy chậm trong SoftAP)
  digitalWrite(LED_PIN, ledState);
}

// Helper function to get accelerometer scale factor (LSB/g) based on current setting
float getAccelScaleFactor() {
  switch (mpu_accel_scale) {
    case MPU6050_ACCEL_FS_2:   return 16384.0; // LSB/g for +/- 2g
    case MPU6050_ACCEL_FS_4:   return 8192.0;  // LSB/g for +/- 4g
    case MPU6050_ACCEL_FS_8:   return 4096.0;  // LSB/g for +/- 8g
    case MPU6050_ACCEL_FS_16: return 2048.0;  // LSB/g for +/- 16g
    default:                  return 4096.0;  // Default to 8g if unknown
  }
}

// Helper function to get gyroscope scale factor (LSB/deg/s) based on current setting
float getGyroScaleFactor() {
  switch (mpu_gyro_scale) {
    case MPU6050_GYRO_FS_250:   return 131.0; // LSB/deg/s for +/- 250 deg/s
    case MPU6050_GYRO_FS_500:   return 65.5;  // LSB/deg/s for +/- 500 deg/s
    case MPU6050_GYRO_FS_1000: return 32.8;  // LSB/deg/s for +/- 1000 deg/s
    case MPU6050_GYRO_FS_2000: return 16.4;  // LSB/deg/s for +/- 2000 deg/s
    default:                   return 32.8;  // Default to 1000 deg/s if unknown
  }
}

// =======================================================================
// --- Setup & Loop Functions ---
// =======================================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting up ESP8266 Sensor Node...");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn LED OFF initially

  Wire.begin(); // Initialize I2C for MPU6050

  // MPU6050 Initialization and Calibration using I2Cdevlib functions
  Serial.println("Initializing MPU6050...");
  mpu.initialize(); // Uses initialize() instead of begin()

  // Verify connection
  Serial.print("Testing MPU6050 connection...");
  if (mpu.testConnection()) {
    Serial.println("MPU6050 connection successful!");
  } else {
    Serial.println("MPU6050 connection failed! Check wiring.");
    // Consider going into SoftAP or halt if MPU is critical
    blinkLED(5, 500); // Blink error code
    // while(true); // Uncomment to halt on MPU failure
  }
  
  loadConfig(); // Load config from EEPROM on startup

  // Apply MPU configuration after loading from EEPROM
  applyMpuConfig(mpu_accel_scale, mpu_gyro_scale, mpu_sample_rate_divider);
  
  Serial.println("Calibrating MPU6050... Keep it still!");
  // The I2Cdevlib doesn't have a direct 'calcOffsets' method for raw data like some other libraries.
  // Calibration typically involves reading raw values and calculating/applying offsets.
  // For simplicity, we'll assume a basic auto-calibration provided by the library
  // or that the offsets are handled by the server for now.
  // If your library has a specific auto-calibration function, use it here.
  // For example, if you're using Electronic Cats' MPU6050 library, 'calibrate()' might exist.
  // For Jeff Rowberg's MPU6050, you might manually set offsets (mpu.setXGyroOffset(), etc.)
  // based on a separate calibration routine. For now, we'll rely on the raw data.
  
  // Calculate send interval from frequency
  if (data_send_frequency_hz > 0) {
    dataSendInterval = 1000 / data_send_frequency_hz; // in milliseconds
  } else {
    dataSendInterval = 1000; // Default to 1 second if 0Hz (should be validated to >0)
  }

  // Attempt WiFi connection
  if (!connectWiFi(20)) { // Try connecting 20 times (10 seconds)
    startSoftAP_mode(); // If failed, start SoftAP for config
  }

  // If in STA mode, but somehow UDP failed to begin (rare)
  if (WiFi.status() == WL_CONNECTED && !udp.begin(UDP_PORT)) {
      Serial.println("Failed to start UDP client. Restarting.");
      delay(2000);
      ESP.restart();
  }
}

void loop() {
  if (isSoftAP) {
    server.handleClient(); // Handle web requests in SoftAP mode
    updateLED(200); // Blink LED slowly in SoftAP mode

    // Check SoftAP timeout
    if (millis() - softAP_timeout_start >= SOFTAP_CONFIG_TIMEOUT_MINUTES * 60 * 1000) {
      if (WiFi.softAPgetStationNum() == 0) { // Check if any clients are connected to SoftAP
        Serial.println("\nSoftAP timeout reached and no clients connected. Restarting to try WiFi connection.");
        ESP.restart(); // Restart to try STA connection again
      } else {
        Serial.println("SoftAP timeout reached, but client(s) still connected. Keeping SoftAP active.");
        softAP_timeout_start = millis(); // Reset timeout to continue SoftAP
      }
    }
  } else {
    // In STA mode (connected to WiFi)
    updateLED(1000 / data_send_frequency_hz); // Blink LED at data sending frequency

    // Check for incoming UDP packets (for WELCOME or CONFIG commands)
    checkUdpIncoming();

    // Check for server heartbeat timeout
    if (millis() - lastServerHeartbeatReceiveTime > SERVER_HEARTBEAT_TIMEOUT) {
        consecutiveHeartbeatFailures++;
        Serial.print("Heartbeat timeout! Consecutive failures: ");
        Serial.println(consecutiveHeartbeatFailures);
        lastServerHeartbeatReceiveTime = millis(); // Reset timer to check again
        if (consecutiveHeartbeatFailures >= MAX_HEARTBEAT_FAILURES) {
            Serial.println("Max heartbeat failures reached. Assuming server is down or unreachable. Restarting WiFi.");
            handshakeCompleted = false; // Force re-handshake
            consecutiveHeartbeatFailures = 0; // Reset counter
            if (!connectWiFi(20)) { // Try reconnecting WiFi
                startSoftAP_mode(); // If WiFi reconnect fails, go to SoftAP
            }
        }
    }

    // --- Handshake Logic ---
    if (!handshakeCompleted) {
      if (millis() - lastHandshakeSendTime >= HANDSHAKE_RETRY_INTERVAL) {
        sendHandshake(); // Resend HELLO periodically until WELCOME is received
      }
    } else {
      // --- Data Sending Logic (only if handshake is complete) ---
      if (millis() - lastDataSendTime >= dataSendInterval) {
        lastDataSendTime = millis();

        // Variables to hold raw MPU data
        int16_t ax, ay, az;
        int16_t gx, gy, gz;
        int16_t tempRaw; // Raw temperature

        // Read MPU6050 data using getMotion6
        mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

        // Get raw temperature and convert to Celsius
        tempRaw = mpu.getTemperature(); // Gets raw temperature value
        float tempC = (tempRaw / 340.0) + 36.53; // Standard conversion for MPU6050 raw temp to Celsius

        // Convert raw values to g's and deg/s
        float accX = (float)ax / getAccelScaleFactor();
        float accY = (float)ay / getAccelScaleFactor();
        float accZ = (float)az / getAccelScaleFactor();
        float gyroX = (float)gx / getGyroScaleFactor();
        float gyroY = (float)gy / getGyroScaleFactor();
        float gyroZ = (float)gz / getGyroScaleFactor();


        // Calculate current server time
        unsigned long currentServerMillis = serverMillisAtHandshake + (millis() - clientMillisAtHandshake);

        // Construct the data string according to the protocol
        // Format: DATA:<NodeID>:<NodeType>:<Timestamp_ms>:<AccX>:<AccY>:<AccZ>:<GyroX>:<GyroY>:<GyroZ>:<TempC>
        char packetBuffer[255]; // Ensure buffer is large enough for all fields
        snprintf(packetBuffer, sizeof(packetBuffer),
                 "DATA:%s:%s:%lu:%.3f:%.3f:%.3f:%.3f:%.3f:%.3f:%.2f",
                 node_id.c_str(),node_type.c_str(), currentServerMillis,
                 accX, accY, accZ, gyroX, gyroY, gyroZ, tempC); // Use tempC here

        // Send the UDP packet to the configured server IP and port
        udp.beginPacket(serverIP, UDP_PORT);
        udp.write(packetBuffer);
        udp.endPacket();

        // For debugging on client side
        Serial.print("Sent: ");
        Serial.println(packetBuffer);

        DataPacketCounter++; // Increment counter for each data packet sent

        if (DataPacketCounter >= DATA_PACKET_RESET_THRESHOLD) {
            Serial.println("Data packet threshold reached, re-doing handshake.");
            handshakeCompleted = false; // Force re-handshake
            DataPacketCounter = 0; // Reset counter
        }
      }
    }
  }
}