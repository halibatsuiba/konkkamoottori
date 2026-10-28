#include <Arduino.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include <cstdlib>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define WEB_AUTH_USER ""
#define WEB_AUTH_PASSWORD ""
#endif

#ifndef SERVO_UART_RX_PIN
#define SERVO_UART_RX_PIN 4
#endif

#ifndef SERVO_UART_TX_PIN
#define SERVO_UART_TX_PIN 5
#endif

#ifndef OLED_SDA_PIN
#define OLED_SDA_PIN 6
#endif

#ifndef OLED_SCL_PIN
#define OLED_SCL_PIN 7
#endif

#ifndef SERVO_UART_BAUD
#define SERVO_UART_BAUD 38400
#endif

#ifndef SERVO_ADDRESS
#define SERVO_ADDRESS 0xE0
#endif

// 0 = SERVO42C V1.0 protocol (no checksum, 16-bit pulse count).
// 1 = SERVO42C V1.1 protocol (checksum, 32-bit pulse count).
#ifndef SERVO_HAS_CHECKSUM
#define SERVO_HAS_CHECKSUM 0
#endif

#ifndef MAX_MOVE_PULSES
#define MAX_MOVE_PULSES 1000000UL
#endif

#if !SERVO_HAS_CHECKSUM && MAX_MOVE_PULSES > 65535UL
#undef MAX_MOVE_PULSES
#define MAX_MOVE_PULSES 65535UL
#endif

namespace {

HardwareSerial servoUart(1);
// The Grove OLED (SSD1308) is driven with the compatible SSD1306 driver.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN);

void showStatus(const String& line1, const String& line2 = "", const String& line3 = "",
                const String& line4 = "") {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  const String* lines[] = {&line1, &line2, &line3, &line4};
  for (int i = 0; i < 4; ++i) {
    oled.drawStr(0, 12 + i * 16, lines[i]->c_str());
  }
  oled.sendBuffer();
}
WebServer server(80);
bool serverStarted = false;
uint32_t lastWifiAttempt = 0;
bool wifiLost = false;
String i2cScanResult;

const char page[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Servo42C controller</title>
  <style>
    body{font:16px system-ui,sans-serif;max-width:28rem;margin:2rem auto;padding:0 1rem}
    label{display:block;margin:1rem 0 .35rem}
    input,select,button{box-sizing:border-box;font:inherit;padding:.65rem;width:100%}
    button{margin-top:1.2rem}
    #result{white-space:pre-wrap}
  </style>
</head>
<body>
  <h1>Servo42C controller</h1>
  <form id="move-form">
    <label for="pulses">Move pulse count</label>
    <input id="pulses" name="pulses" type="number" min="1" required>
    <label for="direction">Direction</label>
    <select id="direction" name="direction">
      <option value="forward">Forward</option>
      <option value="reverse">Reverse</option>
    </select>
    <label for="speed">Speed (1–127)</label>
    <input id="speed" name="speed" type="number" min="1" max="127" value="16" required>
    <button type="submit">Move</button>
    <button type="button" id="run">Run continuously (test)</button>
    <button type="button" id="stop">Stop</button>
  </form>
  <p id="result" role="status"></p>
  <script>
    const form = document.querySelector("#move-form");
    const result = document.querySelector("#result");
    async function send(path) {
      result.textContent = "Sending command…";
      try {
        const response = await fetch(path, {method: "POST", body: new FormData(form)});
        const data = await response.json();
        result.textContent = response.ok ? data.message : `Error: ${data.error}`;
      } catch {
        result.textContent = "Request failed. Check the Wi-Fi connection and try again.";
      }
    }
    document.querySelector("#run").addEventListener("click", () => send("/run"));
    document.querySelector("#stop").addEventListener("click", () => send("/stop"));
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      result.textContent = "Sending command…";
      try {
        const response = await fetch("/move", {method: "POST", body: new FormData(form)});
        const data = await response.json();
        result.textContent = response.ok ? data.message : `Error: ${data.error}`;
      } catch {
        result.textContent = "Request failed. Check the Wi-Fi connection and try again.";
      }
    });
  </script>
</body>
</html>
)HTML";

bool hasNetworkSettings() {
  return WIFI_SSID[0] != '\0' && WIFI_PASSWORD[0] != '\0' &&
         WEB_AUTH_USER[0] != '\0' && WEB_AUTH_PASSWORD[0] != '\0';
}

bool requireAuthentication() {
  if (server.authenticate(WEB_AUTH_USER, WEB_AUTH_PASSWORD)) {
    return true;
  }
  server.requestAuthentication();
  return false;
}

void sendJsonError(int statusCode, const char* message) {
  String body = "{\"error\":\"";
  body += message;
  body += "\"}";
  server.send(statusCode, "application/json", body);
}

bool parseUnsignedArgument(const String& text, uint32_t maximum, uint32_t& value) {
  if (text.isEmpty()) {
    return false;
  }

  char* end = nullptr;
  const unsigned long long parsed = strtoull(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || parsed == 0 || parsed > maximum) {
    return false;
  }

  value = static_cast<uint32_t>(parsed);
  return true;
}

uint8_t checksum(const uint8_t* bytes, size_t length) {
  uint16_t sum = 0;
  for (size_t i = 0; i < length; ++i) {
    sum += bytes[i];
  }
  return static_cast<uint8_t>(sum & 0xFF);
}

bool sendServoCommand(uint8_t* command, size_t length, uint8_t& result) {
  if (servoUart.available() > 0) {
    Serial.print("UART stale RX:");
    while (servoUart.available() > 0) {
      Serial.printf(" %02X", servoUart.read());
    }
    Serial.println();
  }

  // The last array byte is a checksum slot; V1.0 firmware has no checksum, so it is not sent.
  command[length - 1] = checksum(command, length - 1);
  if (!SERVO_HAS_CHECKSUM) {
    --length;
  }
  servoUart.write(command, length);
  servoUart.flush();

  uint8_t response[3];
  size_t received = 0;
  const uint32_t started = millis();
  uint32_t lastByteAt = started;
  while (received < sizeof(response) && millis() - started < 1500) {
    if (servoUart.available() > 0) {
      response[received++] = static_cast<uint8_t>(servoUart.read());
      lastByteAt = millis();
    } else if (received >= 2 && millis() - lastByteAt > 50) {
      break;
    }
    delay(1);
  }

  Serial.print("UART TX:");
  for (size_t i = 0; i < length; ++i) {
    Serial.printf(" %02X", command[i]);
  }
  Serial.print(" | RX:");
  if (received == 0) {
    Serial.print(" (nothing)");
  }
  for (size_t i = 0; i < received; ++i) {
    Serial.printf(" %02X", response[i]);
  }
  Serial.println();

  if (received < 2 || response[0] != SERVO_ADDRESS ||
      (SERVO_HAS_CHECKSUM && received == 3 && response[2] != checksum(response, 2))) {
    return false;
  }

  result = response[1];
  return true;
}

bool sendMoveCommand(uint32_t pulses, bool reverse, uint8_t speed, uint8_t& result) {
  uint8_t enable[] = {SERVO_ADDRESS, 0xF3, 0x01, 0};
  uint8_t enableResult = 0;
  if (!sendServoCommand(enable, sizeof(enable), enableResult) || enableResult != 0x01) {
    return false;
  }

#if SERVO_HAS_CHECKSUM
  uint8_t command[] = {
      SERVO_ADDRESS,
      0xFD,
      static_cast<uint8_t>((reverse ? 0x80 : 0x00) | speed),
      static_cast<uint8_t>((pulses >> 24) & 0xFF),
      static_cast<uint8_t>((pulses >> 16) & 0xFF),
      static_cast<uint8_t>((pulses >> 8) & 0xFF),
      static_cast<uint8_t>(pulses & 0xFF),
      0,
  };
#else
  uint8_t command[] = {
      SERVO_ADDRESS,
      0xFD,
      static_cast<uint8_t>((reverse ? 0x80 : 0x00) | speed),
      static_cast<uint8_t>((pulses >> 8) & 0xFF),
      static_cast<uint8_t>(pulses & 0xFF),
      0,
  };
#endif
  return sendServoCommand(command, sizeof(command), result);
}

void handleMove() {
  if (!requireAuthentication()) {
    return;
  }
  if (!server.hasArg("pulses") || !server.hasArg("direction") || !server.hasArg("speed")) {
    sendJsonError(400, "Provide pulses, direction, and speed.");
    return;
  }

  uint32_t pulses = 0;
  uint32_t speed = 0;
  if (!parseUnsignedArgument(server.arg("pulses"), MAX_MOVE_PULSES, pulses)) {
    sendJsonError(400, "Pulse count must be between 1 and MAX_MOVE_PULSES.");
    return;
  }
  if (!parseUnsignedArgument(server.arg("speed"), 127, speed)) {
    sendJsonError(400, "Speed must be between 1 and 127.");
    return;
  }

  const String direction = server.arg("direction");
  if (direction != "forward" && direction != "reverse") {
    sendJsonError(400, "Direction must be forward or reverse.");
    return;
  }

  uint8_t motorResult = 0;
  const String moveSummary = String(pulses) + (direction == "reverse" ? " rev" : " fwd") + " s" + speed;
  if (!sendMoveCommand(pulses, direction == "reverse", static_cast<uint8_t>(speed), motorResult)) {
    showStatus("Move failed", moveSummary, "No servo reply", WiFi.localIP().toString());
    sendJsonError(502, "No valid response from the SERVO42C. Check UART wiring, baud rate, address, and CR_UART mode.");
    return;
  }
  if (motorResult != 0x01) {
    showStatus("Move rejected", moveSummary, "", WiFi.localIP().toString());
    sendJsonError(502, "The SERVO42C rejected the move command.");
    return;
  }

  showStatus("Move accepted", moveSummary, "", WiFi.localIP().toString());
  server.send(200, "application/json", "{\"message\":\"Move command accepted by the SERVO42C.\"}");
}

bool enableServo() {
  uint8_t enable[] = {SERVO_ADDRESS, 0xF3, 0x01, 0};
  uint8_t enableResult = 0;
  return sendServoCommand(enable, sizeof(enable), enableResult) && enableResult == 0x01;
}

void sendServoResult(bool replied, uint8_t result, const char* okMessage, const char* action) {
  if (!replied) {
    showStatus(String(action) + " failed", "No servo reply", "", WiFi.localIP().toString());
    sendJsonError(502, "No valid response from the SERVO42C. Check UART wiring, baud rate, address, and CR_UART mode.");
  } else if (result != 0x01) {
    showStatus(String(action) + " rejected", "", "", WiFi.localIP().toString());
    sendJsonError(502, "The SERVO42C rejected the command.");
  } else {
    showStatus(String(action) + " accepted", "", "", WiFi.localIP().toString());
    String body = "{\"message\":\"";
    body += okMessage;
    body += "\"}";
    server.send(200, "application/json", body);
  }
}

void handleRun() {
  if (!requireAuthentication()) {
    return;
  }
  uint32_t speed = 0;
  if (!server.hasArg("speed") || !server.hasArg("direction") ||
      !parseUnsignedArgument(server.arg("speed"), 127, speed)) {
    sendJsonError(400, "Provide a speed from 1 to 127 and a direction.");
    return;
  }
  const String direction = server.arg("direction");
  if (direction != "forward" && direction != "reverse") {
    sendJsonError(400, "Direction must be forward or reverse.");
    return;
  }

  uint8_t command[] = {
      SERVO_ADDRESS, 0xF6,
      static_cast<uint8_t>((direction == "reverse" ? 0x80 : 0x00) | speed), 0};
  uint8_t result = 0;
  const bool replied = enableServo() && sendServoCommand(command, sizeof(command), result);
  sendServoResult(replied, result, "Running continuously. Press Stop to halt.", "Run");
}

void handleStop() {
  if (!requireAuthentication()) {
    return;
  }
  uint8_t command[] = {SERVO_ADDRESS, 0xF7, 0};
  uint8_t result = 0;
  const bool replied = sendServoCommand(command, sizeof(command), result);
  sendServoResult(replied, result, "Stop command accepted by the SERVO42C.", "Stop");
}

void startServerIfConnected() {
  if (serverStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  server.on("/", HTTP_GET, []() {
    if (requireAuthentication()) {
      server.send_P(200, "text/html", page);
    }
  });
  server.on("/move", HTTP_POST, handleMove);
  server.on("/run", HTTP_POST, handleRun);
  server.on("/stop", HTTP_POST, handleStop);
  server.onNotFound([]() {
    if (requireAuthentication()) {
      sendJsonError(404, "Not found.");
    }
  });
  server.begin();
  serverStarted = true;
  Serial.println(i2cScanResult);
  Serial.print("Connected. Controller ready at http://");
  Serial.println(WiFi.localIP());
  showStatus("Servo42C controller", "Wi-Fi connected", "Open in browser:", WiFi.localIP().toString());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 3000) {
    delay(10);
  }
  Serial.println("Booting SERVO42C controller...");
  servoUart.begin(SERVO_UART_BAUD, SERIAL_8N1, SERVO_UART_RX_PIN, SERVO_UART_TX_PIN);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  i2cScanResult = "I2C scan (SDA=GPIO" + String(OLED_SDA_PIN) + ", SCL=GPIO" + String(OLED_SCL_PIN) + "):";
  bool anyI2c = false;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      i2cScanResult += " 0x" + String(address, HEX);
      anyI2c = true;
    }
  }
  if (!anyI2c) {
    i2cScanResult += " no devices found";
  }
  Serial.println(i2cScanResult);
  oled.setI2CAddress(0x3C << 1);
  oled.begin();
  showStatus("Servo42C controller", "Booting...");

  if (!hasNetworkSettings()) {
    showStatus("Servo42C controller", "No Wi-Fi settings", "Edit wifi_config.h");
    Serial.println("Wi-Fi/controller credentials are not configured.");
    Serial.println("Copy include/wifi_config.example.h to include/wifi_config.h and set all four values.");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("Wi-Fi disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();
  showStatus("Servo42C controller", "Connecting to Wi-Fi", WIFI_SSID);
  Serial.println("Connecting to Wi-Fi...");
}

void loop() {
  if (!hasNetworkSettings()) {
    delay(1000);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    startServerIfConnected();
    if (wifiLost) {
      wifiLost = false;
      showStatus("Servo42C controller", "Wi-Fi connected", "Open in browser:", WiFi.localIP().toString());
    }
    if (serverStarted) {
      server.handleClient();
    }
  }
  if (WiFi.status() != WL_CONNECTED && (millis() - lastWifiAttempt >= 15000)) {
    lastWifiAttempt = millis();
    wifiLost = true;
    Serial.println("Wi-Fi not connected; retrying.");
    showStatus("Servo42C controller", "Wi-Fi lost", "Reconnecting...");
    WiFi.reconnect();
  }

  delay(2);
}
