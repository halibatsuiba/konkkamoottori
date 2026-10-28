#include <Arduino.h>
#include <WiFi.h>
#include "config.h"

WiFiServer server(TCP_PORT);
WiFiClient client;
String lineBuf;

// Send frame: addr, code, data..., checksum (sum of all bytes & 0xFF)
// Returns first response byte status (1 = ok), or -1 on timeout.
int motorSend(uint8_t code, const uint8_t *data, size_t len) {
  uint8_t buf[8];
  size_t n = 0;
  buf[n++] = MOTOR_ADDR;
  buf[n++] = code;
  for (size_t i = 0; i < len; i++) buf[n++] = data[i];
  uint8_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += buf[i];
  buf[n++] = sum;
  while (Serial1.available()) Serial1.read();
  Serial1.write(buf, n);
  Serial1.flush();

  uint8_t resp[3];
  size_t got = Serial1.readBytes(resp, 3);
  if (got < 3 || resp[0] != MOTOR_ADDR) return -1;
  return resp[1];
}

uint8_t speedByte(long speed) {
  uint8_t dir = speed < 0 ? 0x80 : 0x00;
  long mag = abs(speed);
  if (mag > 127) mag = 127;
  return dir | (uint8_t)mag;
}

String statusText(int st) {
  if (st < 0) return "ERR motor timeout";
  return st == 1 ? "OK" : "ERR motor refused";
}

String handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return "";
  line.toUpperCase();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
  rest.trim();

  if (cmd == "PING") return "PONG";
  if (cmd == "STOP") return statusText(motorSend(0xF7, nullptr, 0));
  if (cmd == "ENABLE") {
    uint8_t d = rest.toInt() ? 1 : 0;
    return statusText(motorSend(0xF3, &d, 1));
  }
  if (cmd == "RUN") {  // RUN <-127..127>
    uint8_t d = speedByte(rest.toInt());
    return statusText(motorSend(0xF6, &d, 1));
  }
  if (cmd == "MOVE") {  // MOVE <speed -127..127> <pulses>
    int s2 = rest.indexOf(' ');
    if (s2 < 0) return "ERR usage: MOVE <speed> <pulses>";
    long speed = rest.substring(0, s2).toInt();
    long pulses = rest.substring(s2 + 1).toInt();
    if (pulses <= 0) return "ERR pulses must be > 0";
    uint8_t d[5] = {speedByte(speed), (uint8_t)(pulses >> 24), (uint8_t)(pulses >> 16),
                    (uint8_t)(pulses >> 8), (uint8_t)pulses};
    return statusText(motorSend(0xFD, d, 5));
  }
  return "ERR unknown command (PING, ENABLE 0|1, RUN <speed>, MOVE <speed> <pulses>, STOP)";
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(MOTOR_BAUD, SERIAL_8N1, MOTOR_RX_PIN, MOTOR_TX_PIN);
  Serial1.setTimeout(200);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\nIP: %s, port %d\n", WiFi.localIP().toString().c_str(), TCP_PORT);
  server.begin();
}

void loop() {
  if (!client || !client.connected()) {
    WiFiClient incoming = server.accept();
    if (incoming) {
      client = incoming;
      lineBuf = "";
      client.println("konkkamoottori ready");
    }
    return;
  }
  while (client.available()) {
    char c = client.read();
    if (c == '\n') {
      String reply = handleCommand(lineBuf);
      lineBuf = "";
      if (reply.length()) client.println(reply);
    } else if (c != '\r' && lineBuf.length() < 64) {
      lineBuf += c;
    }
  }
}
