#!/usr/bin/env python3
"""Control the SERVO42C through the ESP32 web server.

Example: python motor.py forward 10 3200 --host 192.168.1.50
Credentials come from --user/--password, the MOTOR_USER/MOTOR_PASSWORD
environment variables, or include/wifi_config.h (WEB_AUTH_USER/WEB_AUTH_PASSWORD).
"""
import argparse
import base64
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "include", "wifi_config.h")


def read_config_define(name):
    try:
        with open(CONFIG_PATH, encoding="utf-8") as config:
            match = re.search(r'^\s*#define\s+' + name + r'\s+"([^"]*)"', config.read(), re.M)
        return match.group(1) if match else None
    except OSError:
        return None


def main():
    parser = argparse.ArgumentParser(description="Move the SERVO42C via the ESP32 web server.")
    parser.add_argument("direction", choices=["forward", "reverse"])
    parser.add_argument("speed", type=int, help="speed 1-127")
    parser.add_argument("steps", type=int, help="pulse count 1-65535 (3200 = one turn at 16 microsteps)")
    parser.add_argument("--host", default=os.environ.get("MOTOR_HOST"), help="ESP32 IP address (or MOTOR_HOST)")
    parser.add_argument("--user", default=os.environ.get("MOTOR_USER") or read_config_define("WEB_AUTH_USER"))
    parser.add_argument("--password", default=os.environ.get("MOTOR_PASSWORD") or read_config_define("WEB_AUTH_PASSWORD"))
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    if not args.host:
        parser.error("--host (or MOTOR_HOST) is required")
    if not 1 <= args.speed <= 127:
        parser.error("speed must be between 1 and 127")
    if not 1 <= args.steps <= 65535:
        parser.error("steps must be between 1 and 65535")

    request = urllib.request.Request(
        f"http://{args.host}/move",
        data=urllib.parse.urlencode(
            {"direction": args.direction, "speed": args.speed, "pulses": args.steps}
        ).encode(),
        method="POST",
    )
    if args.user is not None:
        token = base64.b64encode(f"{args.user}:{args.password or ''}".encode()).decode()
        request.add_header("Authorization", "Basic " + token)

    # Bypass system proxy auto-detection, which can add seconds on Windows.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    started = time.monotonic()
    try:
        with opener.open(request, timeout=args.timeout) as response:
            body = response.read().decode()
        print(f"Request took {time.monotonic() - started:.2f} s", file=sys.stderr)
    except urllib.error.HTTPError as error:
        body = error.read().decode()
        try:
            body = json.loads(body).get("message", body)
        except ValueError:
            pass
        print(f"Error {error.code}: {body}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError) as error:
        print(f"Connection failed: {error}", file=sys.stderr)
        return 1

    try:
        body = json.loads(body).get("message", body)
    except ValueError:
        pass
    print(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
