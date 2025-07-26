#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h> // Thư viện WebServer cho ESP32
// #include <time.h>      // KHÔNG CẦN THIẾT NỮA vì ESP32 không còn đồng bộ thời gian chuẩn
#include <vector>      // Để lưu trữ thông tin các node
#include <algorithm>   // Để sử dụng std::remove_if

#define AP_SSID "ESP32_AP"      // Tên mạng Wi-Fi (SSID) mà ESP32 tạo ra
#define AP_PASSWORD "12345678"  // Mật khẩu mạng Wi-Fi
#define UDP_PORT 1234          // Cổng UDP mà cả Server và Client sử dụng
#define MAX_CLIENT_TIMEOUT_SECONDS 60 // Thời gian (giây) một client được coi là mất kết nối

// Cấu hình IP tĩnh cho ESP32 khi là AP
IPAddress AP_LOCAL_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

WiFiUDP udp;
WebServer server(80); // Đối tượng WebServer cho ESP32

// Struct để lưu trữ thông tin của từng node sensor
struct SensorNode {
    String id;
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
    String timestamp; // Chuỗi timestamp nhận trực tiếp từ client (dạng mm:ss:us)
    unsigned long last_seen_millis; // Thời điểm cuối cùng nhận được dữ liệu từ node (Millis của ESP32)
};

std::vector<SensorNode> connectedNodes; // Danh sách các node đang kết nối và hoạt động

// --- Time Functions ---
// Hàm getContinuousTimeString() cũ đã bị loại bỏ vì không cần thiết nữa.
// Server không gửi lại bất kỳ timestamp nào cho client qua handshake.

// --- Web Server Handlers ---
void handleRoot() {
    String html = "<html><head><meta http-equiv='refresh' content='5'>" // Tự động refresh sau 5 giây
                  "<style>"
                  "body { font-family: Arial, sans-serif; background-color: #f4f4f4; margin: 20px; color: #333; }"
                  ".container { max-width: 900px; margin: auto; background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }"
                  "h1 { text-align: center; color: #0056b3; }"
                  "p { text-align: center; color: #555; }"
                  "table { width: 100%; border-collapse: collapse; margin-top: 20px; }"
                  "th, td { padding: 10px; border: 1px solid #ddd; text-align: left; }"
                  "th { background-color: #007bff; color: white; }"
                  "tr:nth-child(even) { background-color: #f2f2f2; }"
                  "</style></head><body>"
                  "<div class='container'>"
                  "<h1>ESP32 Sensor Data Dashboard</h1>"
                  "<p>Server Uptime: " + String(millis() / 1000) + " seconds</p>" // Hiển thị thời gian hoạt động của Server
                  "<table>"
                  "<tr><th>Node ID</th><th>AccX</th><th>AccY</th><th>AccZ</th><th>GyroX</th><th>GyroY</th><th>GyroZ</th><th>Timestamp (Node)</th></tr>";

    // Duyệt qua danh sách các node và thêm vào bảng
    for (const auto& node : connectedNodes) {
        html += "<tr>";
        html += "<td>" + node.id + "</td>";
        html += "<td>" + String(node.ax) + "</td>";
        html += "<td>" + String(node.ay) + "</td>";
        html += "<td>" + String(node.az) + "</td>";
        html += "<td>" + String(node.gx) + "</td>";
        html += "<td>" + String(node.gy) + "</td>";
        html += "<td>" + String(node.gz) + "</td>";
        html += "<td>" + node.timestamp + "</td>"; // Hiển thị timestamp gốc từ node (mm:ss:us)
        html += "</tr>";
    }

    html += "</table></div></body></html>";
    server.send(200, "text/html", html);
}

// --- Setup Function ---
void setup() {
    Serial.begin(115200); // Khởi động Serial để gửi dữ liệu về PC
    delay(1000);
    Serial.println("\n--- ESP32 Server Starting ---");

    // Khởi tạo ESP32 làm Access Point
    Serial.print("Setting up Access Point ");
    Serial.print(AP_SSID);
    WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.println("...DONE!");
    Serial.print("AP IP Address: ");
    Serial.println(WiFi.softAPIP());

    // Khởi động UDP Server
    udp.begin(UDP_PORT);
    Serial.print("UDP Server listening on port: ");
    Serial.println(UDP_PORT);

    // Khởi động Web Server
    server.on("/", handleRoot);
    server.begin();
    Serial.println("Web Server started on port 80.");
}

// --- Loop Function ---
void loop() {
    // Xử lý các yêu cầu Web Server (cho dashboard)
    server.handleClient();

    // Lắng nghe gói UDP đến từ các ESP8266 Client
    int packetSize = udp.parsePacket();
    if (packetSize) {
        IPAddress remoteIp = udp.remoteIP();
        int remotePort = udp.remotePort();
        char incomingPacket[255]; // Buffer để lưu gói tin UDP
        int len = udp.read(incomingPacket, sizeof(incomingPacket) - 1);
        incomingPacket[len] = 0; // Đảm bảo chuỗi kết thúc bằng null

        String packetData = String(incomingPacket);
        
        // In gói tin nhận được ra Serial Monitor (để PC đọc)
        Serial.print("Received UDP from ");
        Serial.print(remoteIp);
        Serial.print(":");
        Serial.print(remotePort);
        Serial.print(" -> ");
        Serial.println(packetData); // <-- Dữ liệu gốc (bao gồm ts mm:ss:us từ client) được in ra đây!

        // --- Xử lý Handshake ---
        if (packetData.startsWith("HANDSHAKE:")) {
            String nodeId = packetData.substring(packetData.indexOf(":") + 1);
            Serial.print("Handshake request from Node ID: ");
            Serial.println(nodeId);

            // KHÔNG CÒN GỬI LẠI TIMESTAMP ĐỒNG BỘ NÀO CHO CLIENT NỮA
            // Client ESP8266 giờ không cần phản hồi này để tạo timestamp.
            // Nếu bạn muốn gửi một phản hồi nào đó cho handshake,
            // ví dụ: "OK" hoặc uptime của server, bạn có thể thêm vào đây.
            // Hiện tại, chúng ta không gửi gì.

            // Cập nhật hoặc thêm node vào danh sách (để hiển thị trên web dashboard)
            bool nodeExists = false;
            for (auto& node : connectedNodes) {
                if (node.id == nodeId) {
                    node.last_seen_millis = millis(); // Cập nhật thời gian cuối cùng thấy node
                    // Timestamp của node không thay đổi trong handshake
                    nodeExists = true;
                    Serial.println("Updated handshake for existing node: " + nodeId);
                    break;
                }
            }
            if (!nodeExists) {
                SensorNode newNode;
                newNode.id = nodeId;
                // Khởi tạo giá trị mặc định cho dữ liệu MPU cho node mới
                newNode.ax = 0; newNode.ay = 0; newNode.az = 0;
                newNode.gx = 0; newNode.gy = 0; newNode.gz = 0;
                newNode.timestamp = "00:00:000000"; // Giá trị mặc định cho đến khi nhận được dữ liệu đầu tiên
                newNode.last_seen_millis = millis();
                connectedNodes.push_back(newNode);
                Serial.println("Added new node via handshake: " + nodeId);
            }

        } else {
            // --- Xử lý dữ liệu JSON từ sensor ---
            String id_val = "";
            int16_t ax_val = 0, ay_val = 0, az_val = 0;
            int16_t gx_val = 0, gy_val = 0, gz_val = 0;
            String ts_val = ""; // Biến này sẽ giữ timestamp mm:ss:us từ ESP8266 Client

            // Phân tích JSON thủ công
            // (Giữ nguyên phần parsing này vì nó hoạt động tốt và không dùng thư viện JSON)
            int startIndex = packetData.indexOf("\"id\":\"") + 6;
            int endIndex = packetData.indexOf("\"", startIndex);
            if (startIndex > 5 && endIndex > startIndex) id_val = packetData.substring(startIndex, endIndex);

            startIndex = packetData.indexOf("\"ax\":") + 5;
            endIndex = packetData.indexOf(",", startIndex);
            if (startIndex > 4 && endIndex > startIndex) ax_val = packetData.substring(startIndex, endIndex).toInt();

            startIndex = packetData.indexOf("\"ay\":") + 5;
            endIndex = packetData.indexOf(",", startIndex);
            if (startIndex > 4 && endIndex > startIndex) ay_val = packetData.substring(startIndex, endIndex).toInt();

            startIndex = packetData.indexOf("\"az\":") + 5;
            endIndex = packetData.indexOf(",", startIndex);
            if (startIndex > 4 && endIndex > startIndex) az_val = packetData.substring(startIndex, endIndex).toInt();

            startIndex = packetData.indexOf("\"gx\":") + 5;
            endIndex = packetData.indexOf(",", startIndex);
            if (startIndex > 4 && endIndex > startIndex) gx_val = packetData.substring(startIndex, endIndex).toInt();

            startIndex = packetData.indexOf("\"gy\":") + 5;
            endIndex = packetData.indexOf(",", startIndex);
            if (startIndex > 4 && endIndex > startIndex) gy_val = packetData.substring(startIndex, endIndex).toInt();

            startIndex = packetData.indexOf("\"gz\":") + 5;
            endIndex = packetData.indexOf(",", startIndex);
            if (startIndex > 4 && endIndex > startIndex) gz_val = packetData.substring(startIndex, endIndex).toInt();

            startIndex = packetData.indexOf("\"ts\":\"") + 6;
            endIndex = packetData.indexOf("\"", startIndex); // Tìm dấu " kết thúc chuỗi timestamp
            if (startIndex > 5 && endIndex > startIndex) ts_val = packetData.substring(startIndex, endIndex);

            // Cập nhật dữ liệu cho node tương ứng trong danh sách (để hiển thị trên web dashboard)
            bool nodeFound = false;
            for (auto& node : connectedNodes) {
                if (node.id == id_val) {
                    node.ax = ax_val;
                    node.ay = ay_val;
                    node.az = az_val;
                    node.gx = gx_val;
                    node.gy = gy_val;
                    node.gz = gz_val;
                    node.timestamp = ts_val; // <-- LƯU CHUỖI TIMESTAMP GỐC TỪ CLIENT CHO DASHBOARD
                    node.last_seen_millis = millis(); // Cập nhật thời gian cuối cùng thấy node
                    nodeFound = true;
                    break;
                }
            }

            if (!nodeFound) {
                // Nếu nhận được dữ liệu từ một node mới mà chưa từng handshake (trường hợp hiếm)
                Serial.println("Received data from unknown node ID: " + id_val + ". Adding to list.");
                SensorNode newNode;
                newNode.id = id_val;
                newNode.ax = ax_val;
                newNode.ay = ay_val;
                newNode.az = az_val;
                newNode.gx = gx_val;
                newNode.gy = gy_val;
                newNode.gz = gz_val;
                newNode.timestamp = ts_val;
                newNode.last_seen_millis = millis();
                connectedNodes.push_back(newNode);
            }
        }
    }

    // --- Xóa các node đã timeout ---
    connectedNodes.erase(std::remove_if(connectedNodes.begin(), connectedNodes.end(),
                                         [](const SensorNode& node) {
                                             if (millis() - node.last_seen_millis > (MAX_CLIENT_TIMEOUT_SECONDS * 1000UL)) {
                                                 Serial.println("Node " + node.id + " timed out and removed.");
                                                 return true; // Trả về true để xóa node này
                                             }
                                             return false; // Trả về false để giữ node này
                                         }),
                                connectedNodes.end());

    delay(10); // Một chút delay để không chiếm dụng CPU quá nhiều
}