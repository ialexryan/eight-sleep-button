#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <time.h>
#include "ButtonGate.h"
#include "DeviceConfig.h"
#include "EightSleepClient.h"
#include "Hardware.h"

DeviceConfig deviceConfig;
Hardware hardware;
ButtonGate button;
Preferences preferences;
SemaphoreHandle_t configMutex;
QueueHandle_t requests, results;
std::atomic<bool> configured{false}, requestBusy{false}, testMode{false};
std::atomic<uint32_t> pressCount{0}, completedCount{0};
uint32_t provisionUntil = 0;
bool provisionWindow = false;
struct Request { uint32_t at; bool disconnect; };
struct Result { ApiResult outcome; };

String configJson(const DeviceConfig& c) {
  JsonDocument d;
  d["ssid"] = c.ssid; d["password"] = c.password;
  d["refresh_token"] = c.refreshToken; d["user_id"] = c.userId;
  d["device_id"] = c.deviceId; d["side"] = c.side; d["audio"] = c.audio;
  String json; serializeJson(d, json); return json;
}

bool validConfig(const DeviceConfig& c) {
  return c.ssid.length() > 0 && c.ssid.length() <= 32 && c.password.length() <= 63 &&
         c.refreshToken.length() >= 10 && c.refreshToken.length() <= 4096 &&
         c.userId.length() > 0 && c.userId.length() <= 128 &&
         c.deviceId.length() > 0 && c.deviceId.length() <= 128 &&
         (c.side == "left" || c.side == "right" || c.side == "solo");
}

DeviceConfig parseConfig(JsonDocument& d) {
  DeviceConfig c;
  c.ssid = d["ssid"].as<String>(); c.password = d["password"].as<String>();
  c.refreshToken = d["refresh_token"].as<String>(); c.userId = d["user_id"].as<String>();
  c.deviceId = d["device_id"].as<String>(); c.side = d["side"].as<String>();
  c.audio = d["audio"] | false;
  return c;
}

bool storeConfig(const DeviceConfig& c) {
  String value = configJson(c);
  // NVS commits one blob atomically; flash update never stores half a token set.
  return preferences.putString("config", value) == value.length();
}

bool saveRefresh(const String& token) {
  DeviceConfig replacement = deviceConfig;
  replacement.refreshToken = token;
  return storeConfig(replacement); // caller updates its in-memory token after success
}
EightSleepClient api(deviceConfig, saveRefresh);

bool clockReady() { return time(nullptr) > 1735689600; }

void networkTask(void*) {
  uint32_t lastWifiAttempt = 0, wifiDelay = 15000;
  bool firstAttempt = true;
  for (;;) {
    Request request{};
    if (xQueueReceive(requests, &request, pdMS_TO_TICKS(25)) == pdTRUE) {
      Result result{ApiResult::Failed};
      xSemaphoreTake(configMutex, portMAX_DELAY);
      if (request.disconnect) {
        WiFi.disconnect(false, false);
        firstAttempt = true;
        Serial.println("{\"event\":\"wifi_test_disconnected\"}");
      } else if (testMode.load()) {
        // Diagnostic simulation exercises physical debounce/overlap without cloud writes.
        vTaskDelay(pdMS_TO_TICKS(2000));
        result.outcome = ApiResult::Confirmed;
        Serial.println("{\"event\":\"simulated_result\"}");
      } else if (configured.load() && WiFi.status() == WL_CONNECTED && clockReady()) {
        result.outcome = api.activate(request.at);
      }
      xSemaphoreGive(configMutex);
      if (!request.disconnect) {
        completedCount.fetch_add(1);
        xQueueSend(results, &result, 0);
      } else requestBusy.store(false);
    }
    if (!configured.load() || requestBusy.load()) continue;
    if (xSemaphoreTake(configMutex, 0) != pdTRUE) continue;
    const uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (firstAttempt || uint32_t(now - lastWifiAttempt) >= wifiDelay) {
        firstAttempt = false; lastWifiAttempt = now;
        WiFi.disconnect(false, false);
        WiFi.begin(deviceConfig.ssid.c_str(), deviceConfig.password.c_str());
        wifiDelay = std::min(uint32_t(60000), wifiDelay * 2);
        Serial.println("{\"event\":\"wifi_connecting\"}");
      }
    } else {
      wifiDelay = 15000;
      if (clockReady() && !testMode.load()) api.maintain();
    }
    xSemaphoreGive(configMutex);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void showStatus() {
  JsonDocument d;
  d["event"] = "status"; d["firmware"] = "0.1.0";
  d["configured"] = configured.load(); d["wifi"] = WiFi.status() == WL_CONNECTED;
  d["hostname"] = WiFi.STA.getHostname();
  d["time_synced"] = clockReady(); d["busy"] = requestBusy.load();
  d["test_mode"] = testMode.load(); d["presses"] = pressCount.load();
  d["completed"] = completedCount.load(); d["uptime_s"] = millis() / 1000;
  d["hardware"] = hardware.diagnostics(); d["free_heap"] = ESP.getFreeHeap();
  if (xSemaphoreTake(configMutex, 0) == pdTRUE) {
    d["api"] = api.diagnostic();
    xSemaphoreGive(configMutex);
  }
  String line; serializeJson(d, line); line += '\n';
  Serial.print(line);
}

bool setupAllowed() {
  return !configured.load() ||
         (provisionWindow && int32_t(provisionUntil - millis()) > 0);
}

void serialCommand(const String& line) {
  JsonDocument d;
  if (deserializeJson(d, line)) {
    Serial.println("{\"event\":\"invalid_command\"}"); return;
  }
  const String op = d["op"] | "";
  if (op == "status") { showStatus(); return; }
  if (op == "feedback") {
    if (requestBusy.load()) {
      Serial.println("{\"event\":\"busy\"}"); return;
    }
    const String state = d["state"] | "diagnostics";
    if (state == "started") hardware.preview(FeedbackStatus::Success, "Started");
    else if (state == "active") hardware.preview(FeedbackStatus::Success, "Already active");
    else if (state == "busy") hardware.preview(FeedbackStatus::Busy);
    else if (state == "failure") hardware.preview(FeedbackStatus::Failure, "Check app");
    else if (state == "diagnostics") hardware.preview(FeedbackStatus::Diagnostics, "Display test");
    else { Serial.println("{\"event\":\"invalid_command\"}"); return; }
    Serial.println("{\"event\":\"feedback_test\",\"simulated\":true}"); return;
  }
  if (!setupAllowed()) {
    Serial.println("{\"event\":\"hold_button_to_configure\"}"); return;
  }
  if (requestBusy.load() || xSemaphoreTake(configMutex, 0) != pdTRUE) {
    Serial.println("{\"event\":\"busy\"}"); return;
  }
  if (op == "provision") {
    DeviceConfig replacement = parseConfig(d);
    if (!validConfig(replacement)) Serial.println("{\"event\":\"invalid_config\"}");
    else if (!storeConfig(replacement)) Serial.println("{\"event\":\"storage_error\"}");
    else {
      Serial.println("{\"event\":\"provisioned\"}");
      Serial.flush();
      delay(100);
      ESP.restart(); // all network tasks restart with the new identity; no activation
    }
  } else if (op == "test_mode") {
    testMode.store(d["enabled"] | false);
    Serial.println(testMode.load() ? "{\"event\":\"test_mode_on\"}" : "{\"event\":\"test_mode_off\"}");
  } else if (op == "disconnect") {
    Request request{millis(), true}; requestBusy.store(true);
    if (xQueueSend(requests, &request, 0) != pdTRUE) requestBusy.store(false);
  } else if (op == "erase") {
    if (preferences.remove("config")) {
      Serial.println("{\"event\":\"erased\"}"); Serial.flush(); delay(100); ESP.restart();
    } else Serial.println("{\"event\":\"storage_error\"}");
  } else Serial.println("{\"event\":\"unknown_command\"}");
  xSemaphoreGive(configMutex);
}

void readSerial() {
  static String line;
  static bool overflow = false;
  static uint32_t lastByte = 0;
  // Never echo input: provisioning lines contain credentials.
  unsigned budget = 256;
  while (Serial.available() && budget--) {
    const char c = Serial.read(); lastByte = millis();
    if (c == '\n') {
      if (!overflow && line.length()) serialCommand(line);
      line = ""; overflow = false;
    } else if (c != '\r' && !overflow) {
      if (line.length() >= 8192) { line = ""; overflow = true; }
      else line += c;
    }
  }
  if ((line.length() || overflow) && uint32_t(millis() - lastByte) > 10000) {
    line = ""; overflow = false;
  }
}

void setup() {
  // A provisioning JSON record is larger than HWCDC's 256-byte default queue.
  // Reserve the full bounded record before starting USB so bursts cannot drop it.
  Serial.setRxBufferSize(8193);
  Serial.setTxBufferSize(2048);
  Serial.begin(115200); // never wait for a connected USB host
  Serial.setTxTimeoutMs(0); // diagnostic backpressure must never stall the button
  pinMode(41, INPUT_PULLUP);
  configMutex = xSemaphoreCreateMutex();
  requests = xQueueCreate(1, sizeof(Request));
  results = xQueueCreate(1, sizeof(Result));
  if (!configMutex || !requests || !results) { for (;;) delay(1000); }
  preferences.begin("eightbutton", false);
  String saved = preferences.getString("config", "");
  JsonDocument d;
  if (!deserializeJson(d, saved)) {
    deviceConfig = parseConfig(d); configured.store(validConfig(deviceConfig));
  }
  saved = ""; d.clear();
  if (!hardware.begin(deviceConfig.audio)) {
    configured.store(false);
    Serial.println("{\"event\":\"unsupported_hardware\"}");
  }
  WiFi.persistent(false);
  WiFi.setHostname("eight-sleep-button");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false); // explicit bounded reconnect schedule
  WiFi.setSleep(true);
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org", "time.google.com");
  if (xTaskCreatePinnedToCore(networkTask, "cloud", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("{\"event\":\"worker_failed\"}");
    for (;;) delay(1000);
  }
  Serial.println("{\"event\":\"boot\",\"activation\":false}");
  showStatus();
}

void loop() {
  if (provisionWindow && int32_t(millis() - provisionUntil) >= 0) provisionWindow = false;
  hardware.tick();
  readSerial();
  Result result;
  if (xQueueReceive(results, &result, 0) == pdTRUE) {
    const bool ok = result.outcome == ApiResult::Confirmed || result.outcome == ApiResult::AlreadyActive;
    hardware.show(ok ? FeedbackStatus::Success : FeedbackStatus::Failure,
                  testMode.load() ? "TEST only" : (ok ?
                    (result.outcome == ApiResult::AlreadyActive ? "Already active" : "Started") :
                    (result.outcome == ApiResult::Ambiguous ? "Check app" : "Try again")));
    Serial.printf("{\"event\":\"result\",\"code\":%d,\"simulated\":%s}\n",
                  int(result.outcome), testMode.load() ? "true" : "false");
    requestBusy.store(false);
  }
  ButtonEvent event = button.update(digitalRead(41) == LOW, millis(), requestBusy.load());
  if (event == ButtonEvent::LongPress) {
    provisionWindow = true; provisionUntil = millis() + 60000;
    hardware.show(FeedbackStatus::Diagnostics,
                  !configured.load() ? "Setup needed" : (WiFi.status() == WL_CONNECTED ? "Wi-Fi OK" : "No Wi-Fi"));
    showStatus();
  } else if (event == ButtonEvent::ShortPress) {
    if ((!configured.load() && !testMode.load()) ||
        (!testMode.load() && (WiFi.status() != WL_CONNECTED || !clockReady()))) {
      hardware.show(FeedbackStatus::Failure, configured.load() ? "Offline" : "Setup needed");
      Serial.println("{\"event\":\"press_discarded_not_ready\"}");
    } else if (xSemaphoreTake(configMutex, 0) == pdTRUE) {
      Request request{millis(), false};
      requestBusy.store(true);
      if (xQueueSend(requests, &request, 0) == pdTRUE) {
        pressCount.fetch_add(1);
        hardware.show(FeedbackStatus::Busy);
        Serial.println("{\"event\":\"press_accepted\"}");
      } else requestBusy.store(false);
      xSemaphoreGive(configMutex);
    } else {
      hardware.show(FeedbackStatus::Failure, "Try again");
      Serial.println("{\"event\":\"press_discarded_busy\"}");
    }
  }
  delay(5);
}
