#include "EightSleepClient.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "Verified HTTPS requires the ESP-IDF certificate bundle. Do not disable TLS verification."
#endif

namespace {
constexpr uint32_t kRequestTimeoutMs = 4000;
constexpr uint32_t kPressLifetimeMs = 20000;
constexpr size_t kMaxResponseBytes = 48 * 1024;
constexpr time_t kEarliestClock = 1704067200;  // 2024-01-01 UTC.
constexpr char kAuthUrl[] = "https://auth-api.8slp.net/v1/tokens";
constexpr char kIdentityUrl[] = "https://client-api.8slp.net/v1/users/me";
// Public mobile-app identifiers, not account credentials. Source:
// github.com/j03wang/openhab-eightsleep/blob/main/docs/eightsleep-api.md
constexpr char kClientId[] = "0894c7f33bb94800a03f1f4df13a4f38";
constexpr char kClientSecret[] =
    "f0954a3ed5763ba3d06834c73731a32f15f168f47d4f164751275def86db0c76";

uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }

bool safePathId(const String& value) {
  if (value.isEmpty() || value.length() > 128) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

bool safeToken(const char* value) {
  if (!value) return false;
  const size_t length = strlen(value);
  if (length < 8 || length > 16384) return false;
  for (size_t i = 0; i < length; ++i) {
    if (static_cast<unsigned char>(value[i]) <= 32 ||
        static_cast<unsigned char>(value[i]) >= 127) return false;
  }
  return true;
}

bool matches(JsonVariantConst value, const String& expected) {
  return value.is<const char*>() && expected == value.as<const char*>();
}

bool success(int status) { return status >= 200 && status < 300; }

bool freshPress(uint32_t pressAtMs, uint32_t reserveMs = 0) {
  // Unsigned subtraction deliberately handles millis() wrapping after 49 days.
  return static_cast<uint32_t>(millis() - pressAtMs) <
         kPressLifetimeMs - reserveMs;
}

// Parse both forms of Retry-After without relying on the process timezone.
uint64_t retryDelayMs(const char* value) {
  if (!value || !*value) return 0;
  uint64_t seconds = 0;
  const char* cursor = value;
  while (*cursor >= '0' && *cursor <= '9') {
    seconds = std::min<uint64_t>(seconds * 10 + (*cursor++ - '0'), UINT32_MAX);
  }
  while (*cursor == ' ') ++cursor;
  if (cursor != value && !*cursor) return std::max<uint64_t>(seconds, 1) * 1000;

  struct tm parsed = {};
  char* end = strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &parsed);
  if (!end || *end || parsed.tm_year < 124 || parsed.tm_mon < 0 ||
      parsed.tm_mon > 11 || parsed.tm_mday < 1 || parsed.tm_mday > 31) return 0;
  // Gregorian civil date to days since 1970-01-01 (all calculations in UTC).
  int year = parsed.tm_year + 1900;
  const unsigned month = parsed.tm_mon + 1;
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + parsed.tm_mday - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const int64_t days = era * 146097LL + dayOfEra - 719468;
  const int64_t date = days * 86400 + parsed.tm_hour * 3600 +
                       parsed.tm_min * 60 + parsed.tm_sec;
  const int64_t delta = date - static_cast<int64_t>(time(nullptr));
  return static_cast<uint64_t>(std::max<int64_t>(1, delta)) * 1000;
}
}  // namespace

EightSleepClient::EightSleepClient(DeviceConfig& config,
                                 bool (*saveRefresh)(const String&))
    : _config(config), _saveRefresh(saveRefresh) {}

void EightSleepClient::reset() {
  _accessToken = "";
  _expiresAtMs = _refreshAtMs = _authNotBeforeMs = _apiNotBeforeMs = 0;
  _authFailures = _apiFailures = 0;
  _needsSetup = false;
  _diagnostic = usableConfig() ? "waiting for network" : "not provisioned";
}

bool EightSleepClient::usableConfig() const {
  return safePathId(_config.userId) && safePathId(_config.deviceId) &&
         (_config.side == "left" || _config.side == "right" || _config.side == "solo") &&
         safeToken(_config.refreshToken.c_str()) && _saveRefresh;
}

bool EightSleepClient::readyForNetwork() {
  if (_needsSetup) return false; // Preserve the actionable failure reason.
  if (!usableConfig()) {
    _diagnostic = "setup required";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    _diagnostic = "Wi-Fi unavailable; press discarded";
    return false;
  }
  if (time(nullptr) < kEarliestClock) {
    _diagnostic = "waiting for clock synchronization";
    return false;
  }
  return true;
}

esp_err_t EightSleepClient::onHttpEvent(esp_http_client_event_t* event) {
  auto* response = static_cast<Response*>(event->user_data);
  if (!response) return ESP_FAIL;
  if (event->event_id == HTTP_EVENT_HEADERS_SENT) response->sent = true;
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
      strcasecmp(event->header_key, "Retry-After") == 0) {
    response->retryAfterMs = retryDelayMs(event->header_value);
  }
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
    if (response->body.length() + static_cast<size_t>(event->data_len) > kMaxResponseBytes ||
        !response->body.concat(static_cast<const char*>(event->data), event->data_len)) {
      response->tooLarge = true;
      // Stop a streaming response immediately, including within perform().
      esp_http_client_close(event->client);
      return ESP_FAIL;
    }
  }
  if (nowMs() - response->startedMs >= kRequestTimeoutMs &&
      event->event_id != HTTP_EVENT_DISCONNECTED) {
    response->timedOut = true;
    esp_http_client_close(event->client);
    return ESP_FAIL;
  }
  return ESP_OK;
}

EightSleepClient::Response EightSleepClient::request(
    const String& url, esp_http_client_method_t method, const String* body,
    bool authenticated) {
  Response response;
  response.startedMs = nowMs();
  if (WiFi.status() != WL_CONNECTED || time(nullptr) < kEarliestClock) return response;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = method;
  config.timeout_ms = kRequestTimeoutMs;
  config.disable_auto_redirect = true;
  config.max_authorization_retries = -1;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.skip_cert_common_name_check = false;
  config.transport_type = HTTP_TRANSPORT_OVER_SSL;
  config.event_handler = onHttpEvent;
  config.user_data = &response;
  config.is_async = true;
  config.buffer_size = 1024;
  config.buffer_size_tx = 2048;
  config.user_agent = "eight-sleep-button/1";
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return response;
  bool headersOk = esp_http_client_set_header(client, "Accept", "application/json") == ESP_OK;
  if (authenticated) {
    const String authorization = "Bearer " + _accessToken;
    headersOk &= esp_http_client_set_header(client, "Authorization", authorization.c_str()) == ESP_OK;
  }
  if (body) {
    headersOk &= esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK;
    headersOk &= esp_http_client_set_post_field(client, body->c_str(), body->length()) == ESP_OK;
  }
  if (headersOk) {
    do {
      response.error = esp_http_client_perform(client);
      if (response.error != ESP_ERR_HTTP_EAGAIN || response.tooLarge || response.timedOut) break;
      if (nowMs() - response.startedMs >= kRequestTimeoutMs) {
        response.timedOut = true;
        break;
      }
      delay(10);  // Network worker only; the button/UI task remains responsive.
    } while (true);
    response.status = esp_http_client_get_status_code(client);
  }
  esp_http_client_cleanup(client);
  if (response.tooLarge || response.timedOut) response.error = ESP_FAIL;
  return response;
}

void EightSleepClient::authFailure(const char* message, uint64_t retryAfterMs) {
  if (_authFailures < 7) ++_authFailures;
  const uint64_t delayMs = std::min<uint64_t>(60000ULL << (_authFailures - 1), 3600000);
  _authNotBeforeMs = nowMs() + std::max(delayMs, retryAfterMs);
  _diagnostic = message;
}

void EightSleepClient::apiFailure(const Response& response) {
  if (response.status == 429) {
    _apiNotBeforeMs = nowMs() + std::max<uint64_t>(60000, response.retryAfterMs);
    _diagnostic = "service rate limit; press discarded";
  } else if (response.error != ESP_OK || response.status >= 500) {
    if (_apiFailures < 6) ++_apiFailures;
    const uint64_t delayMs = std::min<uint64_t>(15000ULL << (_apiFailures - 1), 300000);
    _apiNotBeforeMs = nowMs() + std::max(delayMs, response.retryAfterMs);
    _diagnostic = response.tooLarge ? "API response too large" : "network or service unavailable";
  } else {
    _diagnostic = "API request rejected";
  }
}

bool EightSleepClient::refresh() {
  JsonDocument payload;
  payload["client_id"] = kClientId;
  payload["client_secret"] = kClientSecret;
  payload["grant_type"] = "refresh_token";
  payload["refresh_token"] = _config.refreshToken;
  String body;
  serializeJson(payload, body);
  const Response response = request(kAuthUrl, HTTP_METHOD_POST, &body, false);
  if (response.error != ESP_OK || !success(response.status)) {
    // A refused refresh token requires deliberate local reprovisioning. No
    // password fallback and no repeated bad-credential login attempts.
    if (response.status == 400 || response.status == 401 || response.status == 403) {
      _needsSetup = true;
      _accessToken = "";
      _diagnostic = "refresh credential rejected; reprovision";
    } else {
      authFailure("authentication temporarily unavailable", response.retryAfterMs);
    }
    return false;
  }
  JsonDocument filter;
  filter["access_token"] = true;
  filter["refresh_token"] = true;
  filter["expires_in"] = true;
  filter["userId"] = true;
  JsonDocument token;
  if (deserializeJson(token, response.body, DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(8))) {
    authFailure("invalid authentication response");
    return false;
  }
  const char* access = token["access_token"].is<const char*>() ? token["access_token"].as<const char*>() : nullptr;
  const bool hasRefresh = !token["refresh_token"].isNull();
  const char* rotated = token["refresh_token"].is<const char*>() ? token["refresh_token"].as<const char*>() : nullptr;
  // The live refresh response omits userId. Retain the identity verified during
  // provisioning; a supplied identity must still match. Every activation also
  // checks /users/me against the configured user, device, and side before PUT.
  const bool hasIdentity = !token["userId"].isNull();
  if (!safeToken(access) || (hasRefresh && !safeToken(rotated)) ||
      (hasIdentity && !matches(token["userId"], _config.userId)) ||
      !token["expires_in"].is<double>() || token["expires_in"].is<bool>()) {
    _needsSetup = true;
    if (!safeToken(access)) _diagnostic = "refresh response: invalid access token";
    else if (hasRefresh && !safeToken(rotated)) _diagnostic = "refresh response: invalid rotated token";
    else if (hasIdentity && !matches(token["userId"], _config.userId)) _diagnostic = "refresh response: identity mismatch";
    else _diagnostic = "refresh response: expiry schema mismatch";
    return false;
  }
  // The API can encode expires_in as a decimal (the reference client's field
  // is Double). Accept JSON numbers only, retaining subsecond precision rather
  // than treating 72000.0 as a schema error or rounding its expiry upward.
  const double lifetime = token["expires_in"].as<double>();
  if (!std::isfinite(lifetime) || lifetime <= 0 || lifetime > 31536000) {
    authFailure("invalid token lifetime");
    return false;
  }
  if (rotated && _config.refreshToken != rotated) {
    // Save before acknowledging or using the access token. The callback must
    // commit atomically and durably; failure leaves all control disabled.
    if (!_saveRefresh(String(rotated))) {
      _needsSetup = true;
      _accessToken = "";
      _diagnostic = "credential storage failed; reprovision";
      return false;
    }
    _config.refreshToken = rotated;
  }
  _accessToken = access;
  const uint64_t lifetimeMs = static_cast<uint64_t>(lifetime * 1000);
  _expiresAtMs = nowMs() + lifetimeMs;
  const uint64_t marginMs = std::min<uint64_t>(300000, lifetimeMs / 10);
  _refreshAtMs = _expiresAtMs - marginMs;
  _authFailures = 0;
  // Respect the reported expiry, but an unexpectedly tiny lifetime must not
  // create a continuous successful-login loop while the appliance is idle.
  _authNotBeforeMs = lifetime < 60 ? nowMs() + 60000 : 0;
  _diagnostic = "authenticated";
  return true;
}

bool EightSleepClient::ensureToken(bool force) {
  if (_needsSetup) return false;
  if (force) {
    _accessToken = "";
    _expiresAtMs = _refreshAtMs = 0;
  }
  const uint64_t now = nowMs();
  const bool stillValid = !_accessToken.isEmpty() && now + kRequestTimeoutMs < _expiresAtMs;
  if (!force && stillValid && now < _refreshAtMs) return true;
  if (now < _authNotBeforeMs) {
    if (stillValid) return true;
    _diagnostic = "authentication backoff; press discarded";
    return false;
  }
  if (refresh()) return true;
  // An idle proactive refresh failure need not invalidate an unexpired token.
  return !_needsSetup && !_accessToken.isEmpty() && nowMs() + kRequestTimeoutMs < _expiresAtMs;
}

EightSleepClient::Response EightSleepClient::get(const String& url, bool& recovered401) {
  Response response = request(url, HTTP_METHOD_GET);
  if (response.status == 401 && !recovered401) {
    recovered401 = true;
    if (ensureToken(true)) response = request(url, HTTP_METHOD_GET);
  }
  if (response.status == 401 && !_needsSetup) {
    _accessToken = "";
    if (nowMs() >= _authNotBeforeMs) authFailure("access credential rejected; backing off");
  }
  return response;
}

bool EightSleepClient::identityMatches(const String& body) const {
  JsonDocument filter;
  filter["user"]["userId"] = true;
  filter["user"]["currentDevice"] = true;
  JsonDocument identity;
  if (deserializeJson(identity, body, DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(12))) return false;
  const JsonVariantConst user = identity["user"];
  return matches(user["userId"], _config.userId) &&
         matches(user["currentDevice"]["id"], _config.deviceId) &&
         matches(user["currentDevice"]["side"], _config.side);
}

String EightSleepClient::temperatureUrl() const {
  return "https://app-api.8slp.net/v1/users/" + _config.userId + "/temperature/";
}

EightSleepClient::TemperatureState EightSleepClient::temperatureState(const String& body) const {
  JsonDocument filter;
  filter["devices"][0]["device"] = true;
  filter["devices"][0]["currentState"]["type"] = true;
  JsonDocument aggregate;
  if (deserializeJson(aggregate, body, DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(12))) return TemperatureState::Invalid;
  if (!aggregate["devices"].is<JsonArrayConst>()) return TemperatureState::Invalid;
  TemperatureState state = TemperatureState::Invalid;
  unsigned matchesFound = 0;
  for (JsonObjectConst item : aggregate["devices"].as<JsonArrayConst>()) {
    const JsonVariantConst device = item["device"];
    if (!matches(device["deviceId"], _config.deviceId)) continue;
    if (!matches(device["side"], _config.side) ||
        !device["specialization"].is<const char*>() ||
        strcmp(device["specialization"].as<const char*>(), "pod") != 0) return TemperatureState::Invalid;
    ++matchesFound;
    if (!item["currentState"]["type"].is<const char*>()) return TemperatureState::Invalid;
    const char* type = item["currentState"]["type"].as<const char*>();
    if (strcmp(type, "hotFlash") == 0) {
      state = TemperatureState::Active;
    } else {
      static const char* normal[] = {"smart", "smart:initial", "smart:bedtime", "smart:final",
                                     "alarm", "off", "timeBased", "nap"};
      for (const char* candidate : normal) {
        if (strcmp(candidate, type) == 0) state = TemperatureState::Normal;
      }
      if (state != TemperatureState::Normal) return TemperatureState::Invalid;
    }
  }
  return matchesFound == 1 ? state : TemperatureState::Invalid;
}

ApiResult EightSleepClient::failureResult() const {
  if (_needsSetup || !usableConfig()) return ApiResult::NeedsSetup;
  if (nowMs() < _authNotBeforeMs || nowMs() < _apiNotBeforeMs) return ApiResult::Backoff;
  return ApiResult::Failed;
}

void EightSleepClient::maintain() {
  if (!readyForNetwork() || nowMs() < _authNotBeforeMs || nowMs() < _apiNotBeforeMs) return;
  ensureToken();  // Read/authentication only. Never activate on boot or reconnect.
}

ApiResult EightSleepClient::activate(uint32_t pressAtMs) {
  if (!freshPress(pressAtMs)) {
    _diagnostic = "stale press discarded";
    return ApiResult::Failed;
  }
  if (!readyForNetwork()) return failureResult();
  if (nowMs() < _apiNotBeforeMs) {
    _diagnostic = "service backoff; press discarded";
    return ApiResult::Backoff;
  }
  if (!ensureToken()) return failureResult();

  bool recovered401 = false;
  Response identity = get(kIdentityUrl, recovered401);
  if (identity.error != ESP_OK || !success(identity.status)) {
    apiFailure(identity);
    return failureResult();
  }
  if (!identityMatches(identity.body)) {
    _needsSetup = true;
    _diagnostic = "device or side changed; reprovision";
    return ApiResult::NeedsSetup;
  }
  if (!freshPress(pressAtMs)) {
    _diagnostic = "stale press discarded";
    return ApiResult::Failed;
  }
  const String base = temperatureUrl();
  Response before = get(base + "all", recovered401);
  if (before.error != ESP_OK || !success(before.status)) {
    apiFailure(before);
    return failureResult();
  }
  const TemperatureState initial = temperatureState(before.body);
  if (initial == TemperatureState::Invalid) {
    _diagnostic = "unrecognized temperature response; no action";
    return ApiResult::Failed;
  }
  if (initial == TemperatureState::Active) {
    _diagnostic = "rapid cooling already active; timer unchanged";
    return ApiResult::AlreadyActive;
  }
  if (!freshPress(pressAtMs, kRequestTimeoutMs)) {
    _diagnostic = "stale press discarded";
    return ApiResult::Failed;
  }

  // The sole bed mutation in this class: bodyless native Rapid Cooling. Never
  // replay it, even following 401, 429, a timeout, or a successful HTTP response.
  Response activation = request(base + "hot-flash-mode/activate", HTTP_METHOD_PUT);
  if (activation.status == 401) {
    _accessToken = "";
    authFailure("activation unauthorized; not retried");
    return ApiResult::Failed;
  }
  if (activation.status >= 400 && activation.status < 500 && activation.status != 408) {
    apiFailure(activation);
    return failureResult();
  }
  // State is the confirmation, not HTTP 2xx or a Wi-Fi connection. Poll reads
  // only, with a fixed budget; never queue a later write after uncertainty.
  // Even a transport error before HEADERS_SENT is conservatively ambiguous:
  // the server may have received bytes before the local write reported failure.
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (attempt) delay(600);
    Response after = get(base + "all", recovered401);
    if (after.error == ESP_OK && success(after.status)) {
      const TemperatureState observed = temperatureState(after.body);
      if (observed == TemperatureState::Active) {
        _apiFailures = 0;
        _diagnostic = "rapid cooling confirmed";
        return ApiResult::Confirmed;
      }
      if (observed == TemperatureState::Invalid) break;
    } else {
      apiFailure(after);
      break;  // Includes Retry-After: no further polling during backoff.
    }
  }
  if (activation.error != ESP_OK || activation.status >= 500) apiFailure(activation);
  _apiNotBeforeMs = std::max(_apiNotBeforeMs, nowMs() + 30000);
  _diagnostic = "activation unconfirmed; check app before another press";
  return ApiResult::Ambiguous;
}
