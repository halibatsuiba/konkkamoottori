#pragma once

#define WIFI_SSID "your-wifi-network"
#define WIFI_PASSWORD "your-wifi-password"
#define WEB_AUTH_USER "motor"
#define WEB_AUTH_PASSWORD "replace-with-a-long-unique-password"

// Defaults for the ESP32-C3-Zero UART1 pins: RX=GPIO4, TX=GPIO5.
#define SERVO_UART_RX_PIN 4
#define SERVO_UART_TX_PIN 5

// SERVO42C protocol: 0 = V1.0 (no checksum), 1 = V1.1 (checksum).
#define SERVO_HAS_CHECKSUM 0
#define SERVO_UART_BAUD 38400
#define SERVO_ADDRESS 0xE0
#define MAX_MOVE_PULSES 65535UL
