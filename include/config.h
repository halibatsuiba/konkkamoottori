#pragma once

#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"

#define TCP_PORT 5000

// UART to MKS SERVO42C (connect TX->RX, RX->TX, GND). Servo must be in UART mode, 9600 baud.
#define MOTOR_TX_PIN 4
#define MOTOR_RX_PIN 5
#define MOTOR_BAUD 9600
#define MOTOR_ADDR 0xE0
