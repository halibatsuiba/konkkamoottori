# ESP32-C3-Zero SERVO42C controller

This PlatformIO project provides a small Wi-Fi web interface for moving an
MKS SERVO42C by a requested number of pulses, direction, and speed. It sends
the SERVO42C V1.1 UART direct-position command. The displayed pulse count is
the driver's input pulse count; the resulting shaft angle depends on the
motor's step angle and the driver's subdivision setting.

## Configure Wi-Fi

1. Copy `include/wifi_config.example.h` to `include/wifi_config.h`.
2. Set `WIFI_SSID`, `WIFI_PASSWORD`, `WEB_AUTH_USER`, and
   `WEB_AUTH_PASSWORD` in the local copy. The local file is git-ignored; do
   not commit your network or web credentials.
3. Build and upload with PlatformIO. Open the serial monitor at 115200 baud
   to see the controller's IP address.
4. Open that IP address in a browser and sign in with the configured web
   credentials.

The controller uses HTTP Basic Authentication, which is not encrypted. Use
it only on a trusted local network; do not expose it directly to the Internet.

## Firmware updates over Wi-Fi

Build the `esp32-c3-zero` PlatformIO environment, then open the controller's
web page and use **Firmware update** to select
`.pio/build/esp32-c3-zero/firmware.bin`. The page uses the same web credentials
as the controller interface. The update is rejected while the motor is moving;
keep the controller powered until it restarts. After the restart, reload the
page and sign in again if prompted. Use the top menu to switch between Motor
control and Firmware update. Motor control shows a live log of recent commands
and detected motion.

The default ESP32 partition table provides two OTA application slots. The
firmware image must fit in an OTA slot; keep an OTA-capable partition table if
changing the board's partition configuration.

## SERVO42C setup and wiring

Before powering or wiring anything, check the pin labels and voltage levels
for the exact SERVO42C board revision and ESP32-C3-Zero. The ESP32 GPIO is
3.3 V and is not 5-V tolerant. Use a suitable level shifter if the driver's
UART TX signal exceeds 3.3 V.

For UART control, cross the UART data lines and share ground:

| ESP32-C3-Zero | SERVO42C UART |
| --- | --- |
| GPIO5 (TX) | RX |
| GPIO4 (RX) | TX |
| GND | GND |

Use the SERVO42C's documented UART connector/pinout; do not guess connector
positions. Power the motor and driver from their specified motor supply. Do
not connect motor-supply voltage to ESP32 GPIO or power pins. Turn power off
before connecting or disconnecting the driver's terminal.

On the SERVO42C display, use **Menu → Mode → CR_UART**. Set the UART baud rate
to **38400** and the serial address to **E0** (the documented default). The
motor should be calibrated according to the MKS manual before normal use.
The motor's direction and subdivision settings affect the resulting motion.

The command protocol defaults to SERVO42C **V1.0**: no checksum and a 16-bit
pulse count (1–65535 per move). Observed replies from a real driver
(`E0 01` with no checksum byte) matched V1.0. For V1.1 firmware, set
`#define SERVO_HAS_CHECKSUM 1` in `include/wifi_config.h` to use the checksum
format and 32-bit pulse counts. A wrong protocol can make the driver accept a
command but move 0 pulses, so check the `UART TX/RX` serial log if a move
does nothing.

## Python script

[motor.py](motor.py) sends a move to the ESP32 web server (standard library only):

```
python motor.py forward 10 3200 --host <esp32-ip>
```

Arguments: `direction` (`forward`/`reverse`), `speed` (1-127), `steps` (1-65535).
Web credentials are read from `--user`/`--password`, `MOTOR_USER`/`MOTOR_PASSWORD`,
or `include/wifi_config.h`. `MOTOR_HOST` can replace `--host`.

## OLED display

A Grove OLED 128x64 (SSD1308, I2C address 0x3C) shows boot, Wi-Fi and IP
address status, plus the result of the latest move/run/stop command. The last
row always shows the latest command received over Wi-Fi (e.g. `Cmd: Move 3200 fwd s1`). It uses
the U8g2 library with its SSD1306 driver, which is compatible.

| ESP32-C3-Zero | Grove OLED |
| --- | --- |
| GPIO6 | SDA |
| GPIO7 | SCL |
| 3V3 | VCC |
| GND | GND |

Power the display from 3.3 V so the I2C lines stay at ESP32 levels. Change the
pins with `OLED_SDA_PIN` / `OLED_SCL_PIN` in `include/wifi_config.h`.

## Firmware settings

The local `include/wifi_config.h` can also override:

- `OLED_SDA_PIN` / `OLED_SCL_PIN` (defaults: GPIO6 / GPIO7)
- `SERVO_UART_RX_PIN` / `SERVO_UART_TX_PIN` (defaults: GPIO4 / GPIO5)
- `SERVO_UART_BAUD` (default: 38400)
- `SERVO_ADDRESS` (default: `0xE0`)
- `SERVO_HAS_CHECKSUM` (default: 0 = V1.0 protocol; 1 = V1.1)
- `MAX_MOVE_PULSES` (default safety cap: 1,000,000; limited to 65,535 for V1.0)

The web page accepts pulse counts from 1 through `MAX_MOVE_PULSES` and
speeds from 1 through 127. A successful response means the driver accepted
the command; it does not verify that the motor completed the move.
