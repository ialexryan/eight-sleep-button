#pragma once
#include <Arduino.h>

// Personal values live in NVS, never in the firmware image or this header.
struct DeviceConfig {
  String ssid, password, refreshToken, userId, deviceId, side;
  bool audio = false;
};
