#include <WiFi.h>
#include <WiFiUdp.h>
#include <vector>

const char* AP_SSID = "ESP32_AP";
const char* AP_PASSWORD = "12345678";

#define UDP_PORT 1234
IPAddress broadcastIP;

const unsigned long SERVER_HEARTBEAT_INTERVAL = 5000;
const unsigned long CLIENT_TIMEOUT_MS = 30000; // 30s timeout

WiFiUDP Udp;

struct ClientInfo {
    String nodeId;
    String nodeType;
    IPAddress clientIP;
    unsigned int clientPort;
    unsigned long lastSeenMillis;
    uint8_t accelScale;
    uint8_t gyroScale;
    uint8_t sampleRateDivider;
    uint8_t dataSendFrequency;
};

std::vector<ClientInfo> connectedClients;
unsigned long lastHeartbeatBroadcastTime = 0;

int findClientIndexByNodeId(const String& nodeId) {
    for (size_t i = 0; i < connectedClients.size(); ++i) {
        if (connectedClients[i].nodeId == nodeId) return i;
    }
    return -1;
}

int findClientIndexByIPAndPort(const IPAddress& ip, unsigned int port) {
    for (size_t i = 0; i < connectedClients.size(); ++i) {
        if (connectedClients[i].clientIP == ip && connectedClients[i].clientPort == port)
            return i;
    }
    return -1;
}

void handleIncomingUdpPacket() {
    int packetSize = Udp.parsePacket();
    if (packetSize <= 0) return;

    IPAddress remoteIp = Udp.remoteIP();
    unsigned int remotePort = Udp.remotePort();
    char incomingPacket[255];
    int len = Udp.read(incomingPacket, sizeof(incomingPacket) - 1);

    if (len <= 0) return;
    incomingPacket[len] = '\0';
    String packetData = String(incomingPacket);
        Serial.print(packetData);


    if (packetData.startsWith("HELLO:")) {
        Serial.println("Received HELLO from " + remoteIp.toString());

        int pos1 = packetData.indexOf(":");
        int pos2 = packetData.indexOf(":", pos1 + 1);

        if (pos1 != -1 && pos2 != -1) {
            String nodeId = packetData.substring(pos1 + 1, pos2);
            String nodeType = packetData.substring(pos2 + 1);
            int clientIndex = findClientIndexByNodeId(nodeId);

            if (clientIndex == -1) {
                ClientInfo newClient;
                newClient.nodeId = nodeId;
                newClient.nodeType = nodeType;
                newClient.clientIP = remoteIp;
                newClient.clientPort = remotePort;
                newClient.lastSeenMillis = millis();
                newClient.accelScale = 0;
                newClient.gyroScale = 0;
                newClient.sampleRateDivider = 0;
                newClient.dataSendFrequency = 0;
                connectedClients.push_back(newClient);
                Serial.println("New client added: " + nodeId);
            } else {
                connectedClients[clientIndex].clientIP = remoteIp;
                connectedClients[clientIndex].clientPort = remotePort;
                connectedClients[clientIndex].lastSeenMillis = millis();
                Serial.println("Client info updated for: " + nodeId);
            }

            String welcomePacket = "WELCOME:" + nodeId + ":" + String(millis());
            Udp.beginPacket(remoteIp, remotePort);
            Udp.print(welcomePacket);
            Udp.endPacket();
            Serial.println("SUCCESS: Sent WELCOME to " + nodeId);
        } else {
            Serial.println("ERROR: Malformed HELLO packet.");
        }
    } else if (packetData.startsWith("{") && packetData.endsWith("}")) {
        int clientIndex = findClientIndexByIPAndPort(remoteIp, remotePort);
        if (clientIndex != -1) {
            connectedClients[clientIndex].lastSeenMillis = millis();
        }
        Serial.println(packetData); // Gửi ra Python
    } else if (!packetData.startsWith("SERVER_HEARTBEAT")) {
        Serial.print("Unknown UDP packet from ");
        Serial.print(remoteIp);
        Serial.print(": '");
        Serial.print(packetData);
        Serial.println("'");
    } 
}

void broadcastHeartbeat() {
    if (broadcastIP == IPAddress(0, 0, 0, 0)) return;

    String heartbeatPacket = "SERVER_HEARTBEAT:" + String(millis());
    Udp.beginPacket(broadcastIP, UDP_PORT);
    Udp.print(heartbeatPacket);
    Udp.endPacket();
}

void handleSerialInput() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();

        if (command.startsWith("CONFIG:")) {
            int pos1 = command.indexOf(":");
            int pos2 = command.indexOf(":", pos1 + 1);
            int pos3 = command.indexOf(":", pos2 + 1);
            int pos4 = command.indexOf(":", pos3 + 1);
            int pos5 = command.indexOf(":", pos4 + 1);

            if (pos1 != -1 && pos2 != -1 && pos3 != -1 && pos4 != -1 && pos5 != -1) {
                String nodeId = command.substring(pos1 + 1, pos2);
                String a = command.substring(pos2 + 1, pos3);
                String g = command.substring(pos3 + 1, pos4);
                String s = command.substring(pos4 + 1, pos5);
                String f = command.substring(pos5 + 1);

                int accel = a.toInt();
                int gyro = g.toInt();
                int sample = s.toInt();
                int freq = f.toInt();

                if (accel < 0 || gyro < 0 || sample < 0 || freq <= 0) {
                    Serial.println("ERROR: Invalid CONFIG values.");
                    return;
                }

                int idx = findClientIndexByNodeId(nodeId);
                if (idx != -1) {
                    String packet = "CONFIG:" + nodeId + ":" + a + ":" + g + ":" + s + ":" + f;
                    Udp.beginPacket(connectedClients[idx].clientIP, connectedClients[idx].clientPort);
                    Udp.print(packet);
                    Udp.endPacket();
                    Serial.println("Sent CONFIG to " + nodeId + ": " + packet);
                } else {
                    Serial.println("ERROR: Node ID not found.");
                }
            } else {
                Serial.println("ERROR: Invalid CONFIG format.");
            }
        } else if (command == "LIST_CLIENTS") {
            if (connectedClients.empty()) {
                Serial.println("No clients connected.");
            } else {
                Serial.println("--- Connected Clients ---");
                for (auto& c : connectedClients) {
                    Serial.print("Node: " + c.nodeId);
                    Serial.print(", IP: " + c.clientIP.toString());
                    Serial.print(", Port: " + String(c.clientPort));
                    Serial.print(", Last Seen: " + String(millis() - c.lastSeenMillis) + "ms");
                    Serial.println(", Accel: " + String(c.accelScale) + ", Gyro: " + String(c.gyroScale) + ", SRD: " + String(c.sampleRateDivider));
                }
            }
        } else {
            Serial.println("Unknown command: " + command);
        }
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial); 

    Serial.println("\n--- ESP32 Server Starting ---");

    WiFi.softAP(AP_SSID, AP_PASSWORD);
    IPAddress myIP = WiFi.softAPIP();
    broadcastIP = WiFi.softAPBroadcastIP();

    Serial.print("ESP32 IP: ");
    Serial.println(myIP);

    Udp.begin(UDP_PORT);
    Serial.print("Listening on UDP port ");
    Serial.println(UDP_PORT);

    Serial.println("Enter commands (e.g., LIST_CLIENTS, CONFIG:Node:0:1:7:20)");
}

void loop() {
    handleIncomingUdpPacket();

    if (millis() - lastHeartbeatBroadcastTime >= SERVER_HEARTBEAT_INTERVAL) {
        broadcastHeartbeat();
        lastHeartbeatBroadcastTime = millis();
    }

    handleSerialInput();
    for (int i = connectedClients.size() - 1; i >= 0; --i) {
        if (millis() - connectedClients[i].lastSeenMillis > CLIENT_TIMEOUT_MS) {
            Serial.println("Client timeout: " + connectedClients[i].nodeId);
            connectedClients.erase(connectedClients.begin() + i);
        }
    }
}
