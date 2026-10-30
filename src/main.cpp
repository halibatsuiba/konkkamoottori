#include <Arduino.h>
#include <Update.h>
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

bool motorOk = false;

void drawWifiLogo(int centerX, int baseY) {
  oled.drawDisc(centerX, baseY, 1);
  for (int radius = 3; radius <= 9; radius += 3) {
    for (int degrees = -45; degrees <= 45; ++degrees) {
      const float angle = degrees * PI / 180.0f;
      oled.drawPixel(centerX + lroundf(radius * sinf(angle)), baseY - lroundf(radius * cosf(angle)));
    }
  }
}

// Motor icon: body with a front flange and a shaft.
void drawMotorIcon(int x, int y) {
  oled.drawFrame(x, y + 1, 9, 9);
  oled.drawBox(x + 9, y + 2, 1, 7);
  oled.drawBox(x + 10, y + 4, 4, 3);
}

// Round arrow: clockwise for forward, counter-clockwise for reverse.
void drawRoundArrow(int centerX, int centerY, bool reverse) {
  const int radius = 5;
  const int side = reverse ? -1 : 1;
  for (int degrees = 0; degrees <= 270; degrees += 6) {
    const float angle = degrees * PI / 180.0f;
    oled.drawPixel(centerX + side * lroundf(radius * sinf(angle)), centerY - lroundf(radius * cosf(angle)));
  }
  const int tipX = centerX - side * radius;
  oled.drawTriangle(tipX - 2, centerY, tipX + 2, centerY, tipX, centerY - 4);
}

// The icon is shown while the encoder value changes. Direction comes from the last move/run command.
bool motorMoving = false;
bool motorReverse = false;
uint32_t lastEncoderPollAt = 0;
uint32_t lastEncoderChangeAt = 0;
bool encoderValid = false;
uint16_t lastEncoderValue = 0;

// Line 1 is the status line: motor icon, motion arrow and Wi-Fi logo. The last row shows the latest Wi-Fi command.
String lastCommand;
String statusLines[3];

// A browser counts as connected while its page keeps polling /ping.
const uint32_t BROWSER_TIMEOUT_MS = 8000;
uint32_t lastBrowserSeenAt = 0;
bool browserShown = false;

bool browserConnected() {
  return lastBrowserSeenAt != 0 && millis() - lastBrowserSeenAt < BROWSER_TIMEOUT_MS;
}

// PC icon: monitor with a stand.
void drawPcIcon(int x, int y) {
  oled.drawFrame(x, y, 12, 8);
  oled.drawBox(x + 5, y + 8, 2, 1);
  oled.drawBox(x + 3, y + 9, 6, 1);
}

void drawScreen() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  browserShown = browserConnected();
  if (browserShown) {
    drawPcIcon(88, 0);
  }
  if (motorOk) {
    drawMotorIcon(0, 0);
    if (motorMoving) {
      drawRoundArrow(26, 5, motorReverse);
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    drawWifiLogo(116, 9);
  }
  static const String none;
  const String* lines[] = {&statusLines[0], &statusLines[1], &statusLines[2], motorMoving ? &lastCommand : &none};
  for (int i = 0; i < 4; ++i) {
    oled.drawUTF8(0, 24 + i * 12, lines[i]->c_str());
  }
  oled.sendBuffer();
}

void showStatus(const String& line1 = "", const String& line2 = "", const String& line3 = "") {
  statusLines[0] = line1;
  statusLines[1] = line2;
  statusLines[2] = line3;
  drawScreen();
}

void startMotion(bool reverse) {
  motorReverse = reverse;
}

void stopMotion() {
  motorMoving = false;
}
WebServer server(80);
bool serverStarted = false;
bool firmwareUpdateStarted = false;
bool firmwareUpdateFailed = false;
bool firmwareUpdateComplete = false;
int firmwareUpdateStatusCode = 500;
String firmwareUpdateError;
const size_t MOTOR_LOG_CAPACITY = 20;
String motorLogEntries[MOTOR_LOG_CAPACITY];
size_t motorLogNext = 0;
size_t motorLogCount = 0;
uint32_t lastWifiAttempt = 0;
bool wifiLost = false;

void addMotorLog(const String& message) {
  const uint32_t seconds = millis() / 1000;
  char timestamp[16];
  snprintf(timestamp, sizeof(timestamp), "%02lu:%02lu:%02lu",
           static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds / 60) % 60),
           static_cast<unsigned long>(seconds % 60));
  motorLogEntries[motorLogNext] = String("[") + timestamp + "] " + message;
  motorLogNext = (motorLogNext + 1) % MOTOR_LOG_CAPACITY;
  if (motorLogCount < MOTOR_LOG_CAPACITY) {
    ++motorLogCount;
  }
}

const char page[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Seponpyöritin</title>
  <style>
    body{font:16px system-ui,sans-serif;max-width:28rem;margin:2rem auto;padding:0 1rem}
    label{display:block;margin:1rem 0 .35rem}
    input,select,button{box-sizing:border-box;font:inherit;padding:.65rem;width:100%}
    button{margin-top:1.2rem}
    nav{display:flex;align-items:center;gap:.75rem;border-bottom:1px solid #ccc;padding-bottom:.75rem}
    nav label{margin:0}
    nav select{width:auto;flex:1}
    section{margin-top:1.5rem}
    #motor-log{box-sizing:border-box;width:100%;height:14rem;overflow:auto;background:#111;color:#eee;padding:.75rem;white-space:pre-wrap}
    #update-progress{display:block;width:100%;margin-top:1rem}
    #result{white-space:pre-wrap}
  </style>
</head>
<body>
  <h1>Seponpyöritin</h1>
  <nav aria-label="Main menu">
    <label for="page-select">Menu</label>
    <select id="page-select">
      <option value="motor-page">Motor control</option>
      <option value="firmware-page">Firmware update</option>
    </select>
  </nav>
  <section id="motor-page">
    <h2>Motor control</h2>
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
    <h3>Live log</h3>
    <pre id="motor-log" role="log" aria-live="polite">Waiting for motor activity…</pre>
  </section>
  <section id="firmware-page" hidden>
    <h2>Firmware update</h2>
    <p>Select a firmware .bin file built for this controller. Keep the controller powered while it updates.</p>
    <form id="update-form">
      <label for="firmware">Firmware file</label>
      <input id="firmware" name="firmware" type="file" accept=".bin,application/octet-stream" required>
      <button id="update-button" type="submit">Upload firmware</button>
    </form>
    <progress id="update-progress" max="100" value="0" hidden></progress>
    <p id="update-progress-label" aria-live="polite"></p>
    <p id="update-result" role="status"></p>
  </section>
  <script>
    const form = document.querySelector("#move-form");
    const result = document.querySelector("#result");
    const updateForm = document.querySelector("#update-form");
    const updateResult = document.querySelector("#update-result");
    const updateButton = document.querySelector("#update-button");
    const updateProgress = document.querySelector("#update-progress");
    const updateProgressLabel = document.querySelector("#update-progress-label");
    const pageSelect = document.querySelector("#page-select");
    const motorPage = document.querySelector("#motor-page");
    const firmwarePage = document.querySelector("#firmware-page");
    const motorLog = document.querySelector("#motor-log");
    function showSelectedPage() {
      const showMotor = pageSelect.value === "motor-page";
      motorPage.hidden = !showMotor;
      firmwarePage.hidden = showMotor;
      if (showMotor) refreshMotorLog();
    }
    pageSelect.addEventListener("change", showSelectedPage);
    async function refreshMotorLog() {
      if (motorPage.hidden) return;
      try {
        const response = await fetch("/log", {cache: "no-store"});
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const entries = await response.text();
        const wasAtBottom = motorLog.scrollHeight - motorLog.scrollTop - motorLog.clientHeight < 24;
        motorLog.textContent = entries || "No motor activity yet.";
        if (wasAtBottom) motorLog.scrollTop = motorLog.scrollHeight;
      } catch {
        motorLog.textContent = "Live log unavailable. Check the Wi-Fi connection and reload.";
      }
    }
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
    updateForm.addEventListener("submit", async (event) => {
      event.preventDefault();
      updateButton.disabled = true;
      updateProgress.hidden = false;
      updateProgress.value = 0;
      updateProgressLabel.textContent = "Uploading: 0%";
      updateResult.textContent = "Uploading firmware…";
      const request = new XMLHttpRequest();
      request.upload.addEventListener("progress", (progressEvent) => {
        if (!progressEvent.lengthComputable) {
          updateProgress.removeAttribute("value");
          updateProgressLabel.textContent = "Uploading firmware…";
          return;
        }
        const percent = Math.round(progressEvent.loaded * 100 / progressEvent.total);
        updateProgress.value = percent;
        updateProgressLabel.textContent = `Uploading: ${percent}%`;
      });
      request.upload.addEventListener("load", () => {
        updateProgress.value = 100;
        updateProgressLabel.textContent = "Upload complete. Installing firmware…";
        updateResult.textContent = "Keep the controller powered while the firmware is installed.";
      });
      request.addEventListener("load", () => {
        let data;
        try {
          data = JSON.parse(request.responseText);
        } catch {
          updateResult.textContent = "Upload failed. Check the connection and try again.";
          updateButton.disabled = false;
          updateProgressLabel.textContent = "Update failed.";
          return;
        }
        if (request.status >= 200 && request.status < 300) {
          updateProgressLabel.textContent = "Update complete. Controller restarting…";
          updateResult.textContent = `${data.message} Reconnect after the controller restarts.`;
        } else {
          updateResult.textContent = `Error: ${data.error || "Firmware update failed."}`;
          updateButton.disabled = false;
          updateProgressLabel.textContent = "Update failed.";
        }
      });
      request.addEventListener("error", () => {
        updateResult.textContent = "Upload failed or the controller restarted. Check the connection before retrying.";
        updateButton.disabled = false;
        updateProgressLabel.textContent = "Update failed.";
      });
      request.open("POST", "/update");
      request.send(new FormData(updateForm));
    });
    const ping = () => fetch("/ping").catch(() => {});
    ping();
    setInterval(ping, 3000);
    refreshMotorLog();
    setInterval(refreshMotorLog, 1000);
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
  while (servoUart.available() > 0) {
    servoUart.read();
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
  const size_t expected = SERVO_HAS_CHECKSUM ? sizeof(response) : 2;
  while (received < expected && millis() - started < 1500) {
    if (servoUart.available() > 0) {
      response[received++] = static_cast<uint8_t>(servoUart.read());
      lastByteAt = millis();
    } else if (received >= 2 && millis() - lastByteAt > 50) {
      break;
    }
    delay(1);
  }

  if (received < 2 || response[0] != SERVO_ADDRESS ||
      (SERVO_HAS_CHECKSUM && received == 3 && response[2] != checksum(response, 2))) {
    return false;
  }

  result = response[1];
  return true;
}

// Reads the encoder (E0 30). The last two reply bytes are the 16-bit value.
bool readEncoder(uint16_t& value) {
  while (servoUart.available() > 0) {
    servoUart.read();
  }
  servoUart.write(static_cast<uint8_t>(SERVO_ADDRESS));
  servoUart.write(static_cast<uint8_t>(0x30));
  servoUart.flush();

  uint8_t reply[8];
  size_t received = 0;
  const uint32_t started = millis();
  uint32_t lastByteAt = started;
  while (received < sizeof(reply) && millis() - started < 100) {
    if (servoUart.available() > 0) {
      reply[received++] = static_cast<uint8_t>(servoUart.read());
      lastByteAt = millis();
    } else if (received >= 2 && millis() - lastByteAt > 10) {
      break;
    }
    delay(1);
  }
  if (received < 2) {
    return false;
  }
  value = static_cast<uint16_t>((reply[received - 2] << 8) | reply[received - 1]);
  return true;
}

// Polls the encoder and updates the moving flag; redraws the screen when it changes.
void updateMotionFromEncoder() {
  if (!motorOk || millis() - lastEncoderPollAt < 150) {
    return;
  }
  lastEncoderPollAt = millis();
  uint16_t value = 0;
  if (!readEncoder(value)) {
    return;
  }
  const uint32_t now = millis();
  if (encoderValid) {
    const int16_t delta = static_cast<int16_t>(value - lastEncoderValue);
    if (abs(delta) > 8) {
      lastEncoderChangeAt = now;
    }
  }
  encoderValid = true;
  lastEncoderValue = value;
  const bool moving = lastEncoderChangeAt != 0 && now - lastEncoderChangeAt < 400;
  if (moving != motorMoving) {
    motorMoving = moving;
    addMotorLog(moving ? "Motor motion detected." : "Motor stopped.");
    drawScreen();
  }
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
  lastCommand = "Cmd: Move " + String(pulses) + (direction == "reverse" ? " rev" : " fwd") + " s" + speed;
  if (!sendMoveCommand(pulses, direction == "reverse", static_cast<uint8_t>(speed), motorResult)) {
    showStatus("Move failed: no reply", WiFi.localIP().toString());
    addMotorLog("Move failed: no valid response from the SERVO42C.");
    sendJsonError(502, "No valid response from the SERVO42C. Check UART wiring, baud rate, address, and CR_UART mode.");
    return;
  }
  if (motorResult != 0x01) {
    showStatus("Move rejected", WiFi.localIP().toString());
    addMotorLog("Move rejected by the SERVO42C.");
    sendJsonError(502, "The SERVO42C rejected the move command.");
    return;
  }

  startMotion(direction == "reverse");
  addMotorLog("Move accepted: " + String(pulses) + " pulses " +
              (direction == "reverse" ? "reverse" : "forward") + ", speed " + String(speed) + ".");
  showStatus(WiFi.localIP().toString());
  server.send(200, "application/json", "{\"message\":\"Move command accepted by the SERVO42C.\"}");
}

bool enableServo() {
  uint8_t enable[] = {SERVO_ADDRESS, 0xF3, 0x01, 0};
  uint8_t enableResult = 0;
  return sendServoCommand(enable, sizeof(enable), enableResult) && enableResult == 0x01;
}

void sendServoResult(bool replied, uint8_t result, const char* okMessage, const char* action) {
  if (!replied) {
    showStatus(String(action) + " failed: no reply", WiFi.localIP().toString());
    addMotorLog(String(action) + " failed: no valid response from the SERVO42C.");
    sendJsonError(502, "No valid response from the SERVO42C. Check UART wiring, baud rate, address, and CR_UART mode.");
  } else if (result != 0x01) {
    showStatus(String(action) + " rejected", WiFi.localIP().toString());
    addMotorLog(String(action) + " rejected by the SERVO42C.");
    sendJsonError(502, "The SERVO42C rejected the command.");
  } else {
    showStatus(WiFi.localIP().toString());
    addMotorLog(String(action) + " command accepted by the SERVO42C.");
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
  lastCommand = "Cmd: Run " + String(direction == "reverse" ? "rev" : "fwd") + " s" + speed;
  const bool replied = enableServo() && sendServoCommand(command, sizeof(command), result);
  if (replied && result == 0x01) {
    startMotion(direction == "reverse");
  }
  sendServoResult(replied, result, "Running continuously. Press Stop to halt.", "Run");
}

void handleStop() {
  if (!requireAuthentication()) {
    return;
  }
  uint8_t command[] = {SERVO_ADDRESS, 0xF7, 0};
  uint8_t result = 0;
  lastCommand = "Cmd: Stop";
  const bool replied = sendServoCommand(command, sizeof(command), result);
  if (replied && result == 0x01) {
    stopMotion();
  }
  sendServoResult(replied, result, "Stop command accepted by the SERVO42C.", "Stop");
}

void failFirmwareUpdate(const char* message, int statusCode = 500) {
  firmwareUpdateFailed = true;
  firmwareUpdateStatusCode = statusCode;
  firmwareUpdateError = message;
  if (firmwareUpdateStarted) {
    Update.abort();
    firmwareUpdateStarted = false;
  }
  addMotorLog(String("Firmware update failed: ") + firmwareUpdateError);
}

void handleFirmwareUpdateUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    firmwareUpdateStarted = false;
    firmwareUpdateFailed = false;
    firmwareUpdateComplete = false;
    firmwareUpdateStatusCode = 500;
    firmwareUpdateError = "";
    if (!server.authenticate(WEB_AUTH_USER, WEB_AUTH_PASSWORD)) {
      firmwareUpdateFailed = true;
      return;
    }
    if (motorMoving) {
      failFirmwareUpdate("Stop the motor before updating firmware.", 409);
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      failFirmwareUpdate(Update.errorString());
      return;
    }
    firmwareUpdateStarted = true;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (firmwareUpdateStarted && !firmwareUpdateFailed &&
        Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      failFirmwareUpdate(Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (firmwareUpdateStarted && !firmwareUpdateFailed) {
      if (!Update.end(true)) {
        failFirmwareUpdate(Update.errorString());
      } else {
        firmwareUpdateStarted = false;
        firmwareUpdateComplete = true;
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    failFirmwareUpdate("Firmware upload was interrupted.");
  }
}

void handleFirmwareUpdateResult() {
  if (!requireAuthentication()) {
    return;
  }
  if (firmwareUpdateFailed) {
    if (firmwareUpdateStatusCode == 409) {
      sendJsonError(409, "Stop the motor before updating firmware.");
    } else {
      sendJsonError(500, "Firmware update failed. Check the serial log.");
    }
    return;
  }
  if (!firmwareUpdateComplete) {
    sendJsonError(400, "No firmware file was uploaded.");
    return;
  }

  addMotorLog("Firmware update complete; restarting.");
  server.send(200, "application/json", "{\"message\":\"Firmware installed.\"}");
  delay(250);
  ESP.restart();
}

void startServerIfConnected() {
  if (serverStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  server.on("/ping", HTTP_GET, []() {
    if (requireAuthentication()) {
      lastBrowserSeenAt = millis();
      if (!browserShown) {
        drawScreen();
      }
      server.send(200, "text/plain", "ok");
    }
  });
  server.on("/log", HTTP_GET, []() {
    if (!requireAuthentication()) {
      return;
    }
    String body;
    for (size_t i = 0; i < motorLogCount; ++i) {
      const size_t index = (motorLogNext + MOTOR_LOG_CAPACITY - motorLogCount + i) % MOTOR_LOG_CAPACITY;
      body += motorLogEntries[index];
      body += '\n';
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", body);
  });
  server.on("/", HTTP_GET, []() {
    if (requireAuthentication()) {
      server.send_P(200, "text/html", page);
    }
  });
  server.on("/move", HTTP_POST, handleMove);
  server.on("/run", HTTP_POST, handleRun);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/update", HTTP_POST, handleFirmwareUpdateResult, handleFirmwareUpdateUpload);
  server.onNotFound([]() {
    if (requireAuthentication()) {
      sendJsonError(404, "Not found.");
    }
  });
  server.begin();
  serverStarted = true;
  addMotorLog("Controller connected to Wi-Fi.");
  showStatus(WiFi.localIP().toString());
}

}  // namespace

void setup() {
  servoUart.begin(SERVO_UART_BAUD, SERIAL_8N1, SERVO_UART_RX_PIN, SERVO_UART_TX_PIN);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  oled.setI2CAddress(0x3C << 1);
  oled.begin();
  showStatus("Booting...");

  // Read-only enable-status query: it does not move or enable the motor.
  uint8_t statusCommand[] = {SERVO_ADDRESS, 0x3A, 0};
  uint8_t statusResult = 0;
  motorOk = sendServoCommand(statusCommand, sizeof(statusCommand), statusResult);
  showStatus("Booting...");

  if (!hasNetworkSettings()) {
    showStatus("No Wi-Fi settings", "Edit wifi_config.h");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();
  showStatus("Connecting to Wi-Fi", WIFI_SSID);
}

void loop() {
  updateMotionFromEncoder();
  if (browserShown && !browserConnected()) {
    drawScreen();
  }
  if (!hasNetworkSettings()) {
    delay(1000);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    startServerIfConnected();
    if (wifiLost) {
      wifiLost = false;
      showStatus(WiFi.localIP().toString());
    }
    if (serverStarted) {
      server.handleClient();
    }
  }
  if (WiFi.status() != WL_CONNECTED && (millis() - lastWifiAttempt >= 15000)) {
    lastWifiAttempt = millis();
    wifiLost = true;
    showStatus("Wi-Fi lost", "Reconnecting...");
    WiFi.reconnect();
  }

  delay(2);
}
