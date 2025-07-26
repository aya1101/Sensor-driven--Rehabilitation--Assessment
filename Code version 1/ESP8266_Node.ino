#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <MPU6050.h>

#define LED_PIN 2         // Chân LED tích hợp trên ESP8266 (thường là GPIO2)
#define EEPROM_SIZE 100   // Tăng kích thước EEPROM để lưu thêm IP Server (96 + 4 bytes)
#define UDP_PORT 1234     // Cổng UDP dùng để giao tiếp

// THAY ĐỔI THEO YÊU CẦU: Thời gian SoftAP sẽ chờ cấu hình trước khi thử kết nối lại (phút)
#define SOFTAP_CONFIG_TIMEOUT_MINUTES 1

MPU6050 mpu;                  // Đối tượng MPU6050
WiFiUDP udp;                  // Đối tượng UDP
ESP8266WebServer server(80); // Đối tượng Web Server cho cấu hình

String node_id = "";          // ID của Node, đọc từ EEPROM
String ap_ssid = "";          // SSID của mạng WiFi cần kết nối (AP của ESP32), đọc từ EEPROM
String ap_pass = "";          // Mật khẩu mạng WiFi, đọc từ EEPROM
bool isSoftAP = false;        // Cờ báo hiệu đang ở chế độ SoftAP (cấu hình)

IPAddress serverIP; // Địa chỉ IP của Server (ESP32 AP), đọc từ EEPROM hoặc cấu hình qua web

unsigned long softAP_timeout_start = 0; // Thời điểm bắt đầu chế độ SoftAP

// Biến trạng thái LED và thời điểm cuối cùng thay đổi trạng thái
unsigned long lastLedToggleTime = 0;
bool ledState = HIGH; // Mặc định LED OFF (cho NodeMCU và ESP-01)

// --- EEPROM Functions ---
void writeStringToEEPROM(int start, int length, String data) {
  for (int i = 0; i < length; i++) {
    if (i < data.length()) EEPROM.write(start + i, data[i]);
    else EEPROM.write(start + i, 0);
  }
  EEPROM.commit();
}

String readStringFromEEPROM(int start, int int_length) {
  String result = "";
  for (int i = 0; i < int_length; i++) {
    char c = char(EEPROM.read(start + i));
    if (c == 0) break;
    result += c;
  }
  return result;
}

void writeIPToEEPROM(int start, IPAddress ip) {
  for (int i = 0; i < 4; i++) {
    EEPROM.write(start + i, ip[i]);
  }
  EEPROM.commit();
}

IPAddress readIPFromEEPROM(int start) {
  byte ipBytes[4];
  for (int i = 0; i < 4; i++) {
    ipBytes[i] = EEPROM.read(start + i);
  }
  return IPAddress(ipBytes[0], ipBytes[1], ipBytes[2], ipBytes[3]);
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  node_id = readStringFromEEPROM(0, 32);
  ap_ssid = readStringFromEEPROM(32, 32);
  ap_pass = readStringFromEEPROM(64, 32);
  serverIP = readIPFromEEPROM(96);

  // --- Giá trị mặc định (Default values) ---
  if (node_id == "") node_id = "SensorNode_01"; // <--- THAY ĐỔI CÁI NÀY CHO MỖI CLIENT!
  if (ap_ssid == "") ap_ssid = "ESP32_AP";
  if (ap_pass == "") ap_pass = "12345678";
  if (serverIP == IPAddress(0,0,0,0)) serverIP = IPAddress(192, 168, 4, 1);
}

void saveConfig(String id, String ssid, String pass, IPAddress ip) {
  EEPROM.begin(EEPROM_SIZE);
  writeStringToEEPROM(0, 32, id);
  writeStringToEEPROM(32, 32, ssid);
  writeStringToEEPROM(64, 32, pass);
  writeIPToEEPROM(96, ip);
  EEPROM.commit();
  Serial.println("Config saved to EEPROM.");
}

// --- LED Functions ---
// Hàm blinkLED giờ chỉ dùng cho các sự kiện tức thời (ví dụ: khởi động, handshake)
void blinkLED(int times, int delayMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW); // LED ON
    delay(delayMs);
    digitalWrite(LED_PIN, HIGH); // LED OFF
    delay(delayMs);
  }
}

// Hàm cập nhật trạng thái LED dựa trên chế độ (non-blocking)
void updateLED(int delayMs) {
  if (millis() - lastLedToggleTime >= delayMs) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
    lastLedToggleTime = millis();
  }
}

// --- Time Functions ---
String getRelativeTime_mm_ss_us() {
  unsigned long ms = millis();

  // Đảm bảo không quá 1 giờ (3.600.000 ms = 60 phút)
  // Nếu vượt quá, nó sẽ quay vòng về 0
  ms %= 3600000UL;

  unsigned long totalSeconds = ms / 1000UL;
  unsigned long minutes = totalSeconds / 60;
  unsigned long seconds = totalSeconds % 60;
  unsigned long microseconds = (ms % 1000UL) * 1000UL;

  char buffer[15];
  sprintf(buffer, "%02lu:%02lu:%06lu", minutes, seconds, microseconds);
  return String(buffer);
}

// --- Web Config Handlers ---
void handleRoot() {
  String html = "<html><head><style>"
                "body { font-family: Arial; background:#f0f0f0; display:flex; justify-content:center; align-items:center; height:100vh; margin:0; }"
                "form { background:white; padding:20px; border-radius:8px; box-shadow:0 0 10px #ccc; width:320px; }"
                "input { width:100%; padding:10px; margin:10px 0; font-size:16px; box-sizing:border-box; }"
                "button { width:100%; padding:10px; margin:10px 0; background-color:#4CAF50; color:white; border:none; border-radius:5px; cursor:pointer; }"
                "button:hover { background-color:#45a049; }"
                "</style></head><body>";
  html += "<form method='POST' action='/save'>";
  html += "<h2>Config Sensor Node</h2>";
  html += "Node ID:<input name='node_id' value='" + node_id + "'>";
  html += "ESP32 AP SSID:<input name='ap_ssid' value='" + ap_ssid + "'>";
  html += "ESP32 AP Password:<input type='password' name='ap_pass' value='" + ap_pass + "'>";
  html += "ESP32 Server IP:<input name='server_ip' value='" + serverIP.toString() + "'>";
  html += "<button type='submit'>Save & Restart</button>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  node_id = server.arg("node_id");
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

  saveConfig(node_id, ap_ssid, ap_pass, serverIP);
  server.send(200, "text/html", "<html><body><h3>Configuration Saved. Restarting...</h3></body></html>");
  delay(1000);
  ESP.restart();
}

// --- WiFi Functions ---
// Hàm kết nối WiFi có tham số số lần thử tối đa
bool connectWiFi(int max_retries) {
  Serial.print("Attempting to connect to WiFi AP: ");
  Serial.println(ap_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ap_ssid.c_str(), ap_pass.c_str());
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < max_retries) { // Dùng max_retries
    delay(500);
    retry++;
    Serial.print(".");
    blinkLED(1, 50); // Nháy nhanh trong quá trình thử kết nối
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected successfully!");
    Serial.print("Assigned IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("WiFi connection FAILED!");
    return false;
  }
}

// --- Handshake ---
bool performHandshake() {
  Serial.println("Performing handshake (sending Node ID) to server...");
  Serial.print("Sending HANDSHAKE to ");
  Serial.print(serverIP);
  Serial.print(":");
  Serial.println(UDP_PORT);

  udp.beginPacket(serverIP, UDP_PORT);
  udp.print("HANDSHAKE:" + node_id);
  udp.endPacket();
  blinkLED(3, 100); // Nháy nhanh 3 lần sau handshake

  Serial.println("Handshake message sent. No time sync expected/needed from server for client's timestamp.");
  return true;
}

void startSoftAP() {
  isSoftAP = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(node_id.c_str());
  softAP_timeout_start = millis();

  Serial.println("Entering SoftAP mode for configuration.");
  Serial.println("Connect to WiFi SSID: " + node_id);
  Serial.println("Access configuration at IP: " + WiFi.softAPIP().toString());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  // Không dùng blinkLED ở đây, mà dùng updateLED trong loop
  lastLedToggleTime = millis(); // Reset thời gian nháy cho chế độ SoftAP
  ledState = HIGH; // Bắt đầu ở trạng thái OFF (để nháy)
  digitalWrite(LED_PIN, ledState); // Đảm bảo LED tắt ban đầu
}

// --- Setup Function ---
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Mặc định LED tắt (cho ESP-01 và NodeMCU)
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- ESP8266 Sensor Node Starting ---");

  // Khởi tạo MPU6050
  Wire.begin();
  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("MPU6050 connection FAILED or not detected.");
    digitalWrite(LED_PIN, LOW); // LED ON báo lỗi
    delay(2000);
    digitalWrite(LED_PIN, HIGH); // LED OFF sau khi báo lỗi
  } else {
    Serial.println("MPU6050 connected successfully.");

    // --- CẤU HÌNH MPU6050 CHO ĐO TỐC ĐỘ CHẠY BỘ ---
    mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_8);
    Serial.println("Accelerometer range set to +/- 8g");

    mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_1000);
    Serial.println("Gyroscope range set to +/- 1000 deg/s");

    mpu.setRate(7); // 125 Hz internal sample rate
    Serial.println("MPU6050 internal sampling rate set to 125 Hz.");
    // ----------------------------------------------------
  }

  loadConfig();
  EEPROM.begin(EEPROM_SIZE);

  Serial.println("Node ID: " + node_id);
  Serial.println("Configured ESP32 AP SSID: " + ap_ssid);
  Serial.println("Configured ESP32 AP Password: " + ap_pass);
  Serial.println("Configured ESP32 Server IP: " + serverIP.toString());

  // THAY ĐỔI: Lần đầu khởi động, chỉ thử 20 lần (10 giây)
  if (!connectWiFi(20)) {
    Serial.println("Initial WiFi connection failed after 20 retries. Transitioning to SoftAP for configuration.");
    startSoftAP();
  } else {
    udp.begin(UDP_PORT);
    performHandshake();
    // Sau khi kết nối và handshake thành công, LED sẽ được quản lý bởi updateLED trong loop
    lastLedToggleTime = millis(); // Reset thời gian nháy
    ledState = LOW; // Bắt đầu ở trạng thái ON
    digitalWrite(LED_PIN, ledState); // Đảm bảo LED bật ban đầu
  }
}

// --- Loop Function ---
void loop() {
  if (isSoftAP) {
    server.handleClient();
    updateLED(250); // Nháy 2Hz (250ms ON, 250ms OFF) khi ở SoftAP

    // THAY ĐỔI: Thời gian timeout SoftAP chỉ còn 1 phút
    if (millis() - softAP_timeout_start > (SOFTAP_CONFIG_TIMEOUT_MINUTES * 60 * 1000UL)) {
      Serial.println("SoftAP timeout. Attempting to connect to AP again.");
      isSoftAP = false;
      WiFi.softAPdisconnect(true);
      delay(100);
      WiFi.mode(WIFI_STA);

      // THAY ĐỔI: Sau timeout SoftAP, thử kết nối WiFi 5 lần
      if (!connectWiFi(5)) {
        Serial.println("Reconnect to AP failed after SoftAP timeout (5 retries). Re-entering SoftAP mode.");
        startSoftAP(); // Quay lại SoftAP nếu không kết nối được
      } else {
        udp.begin(UDP_PORT);
        performHandshake();
        lastLedToggleTime = millis(); // Reset thời gian nháy
        ledState = LOW; // Bắt đầu ở trạng thái ON
        digitalWrite(LED_PIN, ledState); // Đảm bảo LED bật ban đầu
      }
    }
    return; // Thoát khỏi loop nếu đang ở SoftAP
  }

  // Nếu không ở SoftAP, kiểm tra trạng thái WiFi và gửi dữ liệu
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected or failed. Attempting to reconnect...");
    updateLED(500); // Nháy chậm (1Hz) khi đang cố gắng reconnect
    delay(1000); // Đợi thêm 1s trước khi thử lại

    // THAY ĐỔI: Khi mất kết nối, thử kết nối WiFi 5 lần
    if (connectWiFi(5)) {
      Serial.println("WiFi reconnected.");
      udp.begin(UDP_PORT);
      performHandshake();
      lastLedToggleTime = millis(); // Reset thời gian nháy
      ledState = LOW; // Bắt đầu ở trạng thái ON
      digitalWrite(LED_PIN, ledState); // Đảm bảo LED bật ban đầu
    } else {
      Serial.println("WiFi reconnect FAILED after 5 retries. Transitioning to SoftAP mode.");
      startSoftAP(); // Vào SoftAP nếu không thể reconnect
      return;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Luôn nháy LED 10Hz khi đang kết nối và gửi dữ liệu
    updateLED(50); // Nháy 10Hz (50ms ON, 50ms OFF)

    Serial.print("Sending sensor data to ");
    Serial.print(WiFi.SSID());
    Serial.println("...");

    String tsStr = getRelativeTime_mm_ss_us();

    int16_t ax, ay, az, gx, gy, gz;
    if (mpu.testConnection()) {
      mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
      // LED đã được quản lý bởi updateLED(50) ở trên, không cần bật/tắt riêng lẻ
    } else {
      ax = ay = az = gx = gy = gz = 0;
      Serial.println("MPU6050 connection lost or failed. Sending zero data.");
      // Có thể nháy LED khác hoặc giữ LED sáng báo lỗi MPU
      // Ví dụ: digitalWrite(LED_PIN, LOW); // Giữ LED sáng để báo lỗi MPU
    }

    String json = "{";
    json += "\"id\":\"" + node_id + "\",";
    json += "\"ax\":" + String(ax) + ",";
    json += "\"ay\":" + String(ay) + ",";
    json += "\"az\":" + String(az) + ",";
    json += "\"gx\":" + String(gx) + ",";
    json += "\"gy\":" + String(gy) + ",";
    json += "\"gz\":" + String(gz) + ",";
    json += "\"ts\":\"" + tsStr + "\"";
    json += "}";

    udp.beginPacket(serverIP, UDP_PORT);
    udp.print(json);
    udp.endPacket();

    Serial.println("Data sent: " + json);

    // Đảm bảo delay này phù hợp với tần số lấy mẫu MPU6050 và tần số gửi
    // Nếu bạn muốn gửi 10Hz, thì delay nên là 100ms
    delay(100);
  }
}