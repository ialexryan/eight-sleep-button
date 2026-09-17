#pragma once

#include <Arduino.h>
#include <esp_http_client.h>

#include "DeviceConfig.h"

enum class ApiResult {
  Confirmed,
  Restarted,
  Failed,
  Ambiguous,
  NeedsSetup,
  Backoff,
};

// Single-owner object: use only from the network worker. Provisioning must stop
// that worker before changing config and calling reset(). No password is stored.
class EightSleepClient {
 public:
  EightSleepClient(DeviceConfig& config, bool (*saveRefresh)(const String&));
  ApiResult activate(uint32_t pressAtMs);
  void maintain();
  void reset();
  const char* diagnostic() const { return _diagnostic; }

 private:
  struct Response {
    String body;
    int status = 0;
    esp_err_t error = ESP_FAIL;
    uint64_t startedMs = 0;
    uint64_t retryAfterMs = 0;
    bool sent = false;
    bool tooLarge = false;
    bool timedOut = false;
  };
  enum class TemperatureState { Invalid, Normal, Active };
  struct CycleTiming {
    uint64_t startedMs = 0;
    uint64_t untilMs = 0;
    bool valid = false;
  };

  DeviceConfig& _config;
  bool (*_saveRefresh)(const String&);
  String _accessToken;
  uint64_t _expiresAtMs = 0;
  uint64_t _refreshAtMs = 0;
  uint64_t _authNotBeforeMs = 0;
  uint64_t _apiNotBeforeMs = 0;
  uint8_t _authFailures = 0;
  uint8_t _apiFailures = 0;
  bool _needsSetup = false;
  const char* _diagnostic = "not provisioned";

  static esp_err_t onHttpEvent(esp_http_client_event_t* event);
  Response request(const String& url, esp_http_client_method_t method,
                   const String* body = nullptr, bool authenticated = true);
  Response get(const String& url, bool& recovered401);
  bool usableConfig() const;
  bool readyForNetwork();
  bool ensureToken(bool force = false);
  bool refresh();
  void authFailure(const char* message, uint64_t retryAfterMs = 0);
  void apiFailure(const Response& response);
  bool identityMatches(const String& body) const;
  TemperatureState temperatureState(const String& body, CycleTiming* timing = nullptr) const;
  String temperatureUrl() const;
  ApiResult failureResult() const;
};
