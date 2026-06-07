# atoms3r_claude_notify

A small desktop hardware notifier for Claude Code: an M5Stack ATOM S3R that watches Claude's state and displays a pixel-art eye animation for standby, thinking, done, and waiting-for-input.

## What it is

When Claude Code starts a long task, you stop looking at the terminal and start doing something else. This device sits on the desk, sees the state change over Wi-Fi (a Claude Code hook pings an HTTP endpoint on the device), and shows you when Claude is thinking, when it is done, and when it needs your attention.

## Hardware

- M5Stack ATOM S3R (ESP32-S3, built-in BMI270 IMU, GC9107 1.14" TFT, LP5562 RGB LED)
- USB-C power, Wi-Fi only (no BLE in this build)
- Button on GPIO 41 for state acknowledgement

## Stack

- ESP-IDF v6 in C
- Custom GC9107 driver for the small TFT (pixel-art rendering, no graphics library overhead)
- BMI270 IMU read for tap and tilt gestures
- `esp_http_server` listens for state POSTs from a Claude Code hook

## Status

Personal hardware project, working on my desk. Wi-Fi credentials kept in a gitignored `secrets.h`; an `example` file is committed. Not packaged for distribution.
