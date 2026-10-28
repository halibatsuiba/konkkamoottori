# konkkamoottori
Pyoritin multiturn-konkalle

PlatformIO (pioarduino) project for ESP32-C3-Zero driving a MKS SERVO42C stepper over UART.
Edit `include/config.h` (WiFi, pins), build with `pio run`, upload with `pio run -t upload`.

Connect over TCP (default port 5000), send newline-terminated commands:
`PING`, `ENABLE 0|1`, `RUN <-127..127>`, `MOVE <speed> <pulses>`, `STOP`.
Wiring: GPIO4 (TX) -> servo RX, GPIO5 (RX) -> servo TX, common GND.
