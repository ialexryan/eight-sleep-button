#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <cassert>
#include <deque>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "EightSleepClient.h"

uint64_t fakeNowMs = 1000;
struct Reply {
  esp_http_client_method_t method;
  std::string suffix;
  int status = 200;
  std::string body;
  esp_err_t error = ESP_OK;
  bool sent = true;
  uint32_t elapsed = 0;
  std::string retryAfter;
  bool hang = false;
};
struct Recorded {
  esp_http_client_method_t method;
  std::string url, body;
  std::map<std::string, std::string> headers;
};
struct MockHttpClient {
  esp_http_client_config_t config;
  Recorded request;
  int status = 0;
  bool closed = false;
  bool recorded = false;
};
std::deque<Reply> replies;
std::vector<Recorded> recorded;
std::vector<std::string> saved;
bool saveSucceeds = true;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
  assert(config->crt_bundle_attach == esp_crt_bundle_attach);
  assert(!config->skip_cert_common_name_check);
  assert(config->transport_type == HTTP_TRANSPORT_OVER_SSL);
  assert(config->disable_auto_redirect && config->max_authorization_retries == -1);
  assert(config->is_async && config->timeout_ms <= 4000);
  auto* client = new MockHttpClient;
  client->config = *config;
  client->request.method = config->method;
  client->request.url = config->url;
  assert(client->request.url.rfind("https://", 0) == 0);
  return client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char* name, const char* value) {
  client->request.headers[name] = value;
  return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char* data, int length) {
  client->request.body.assign(data, length);
  return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
  assert(!replies.empty());
  const Reply reply = replies.front();
  assert(reply.method == client->request.method);
  const auto& url = client->request.url;
  assert(url.size() >= reply.suffix.size() &&
         url.compare(url.size() - reply.suffix.size(), reply.suffix.size(), reply.suffix) == 0);
  if (!client->recorded) {
    recorded.push_back(client->request);
    client->recorded = true;
  }
  if (reply.hang) {
    fakeNowMs += 1000;
    return ESP_ERR_HTTP_EAGAIN;
  }
  replies.pop_front();
  fakeNowMs += reply.elapsed;
  client->status = reply.status;
  esp_http_client_event_t event{};
  event.client = client;
  event.user_data = client->config.user_data;
  if (reply.sent) {
    event.event_id = HTTP_EVENT_HEADERS_SENT;
    client->config.event_handler(&event);
  }
  if (!reply.retryAfter.empty()) {
    event.event_id = HTTP_EVENT_ON_HEADER;
    event.header_key = const_cast<char*>("Retry-After");
    event.header_value = const_cast<char*>(reply.retryAfter.c_str());
    client->config.event_handler(&event);
  }
  if (!reply.body.empty()) {
    event.event_id = HTTP_EVENT_ON_DATA;
    event.data = const_cast<char*>(reply.body.data());
    event.data_len = static_cast<int>(reply.body.size());
    client->config.event_handler(&event);
  }
  return client->closed ? ESP_FAIL : reply.error;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { return client->status; }
esp_err_t esp_http_client_close(esp_http_client_handle_t client) { client->closed = true; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { delete client; return ESP_OK; }

bool saveToken(const String& token) {
  if (saveSucceeds) saved.push_back(token.c_str());
  return saveSucceeds;
}
DeviceConfig config() {
  DeviceConfig result;
  result.ssid = "fixture-network";
  result.password = "fixture-wifi-password";
  result.refreshToken = "fixture-refresh-original";
  result.userId = "fixture-user";
  result.deviceId = "fixture-device";
  result.side = "right";
  return result;
}
void resetFixture() {
  assert(replies.empty());
  recorded.clear(); saved.clear(); saveSucceeds = true;
  fakeNowMs = 1000; WiFi.state = WL_CONNECTED;
}
Reply auth(std::string rotated = "fixture-refresh-rotated", unsigned expires = 72000) {
  return {HTTP_METHOD_POST, "/tokens", 200,
          "{\"userId\":\"fixture-user\",\"access_token\":\"fixture-access-token\",\"refresh_token\":\"" +
              rotated + "\",\"expires_in\":" + std::to_string(expires) + "}"};
}
Reply identity(std::string side = "right") {
  return {HTTP_METHOD_GET, "/users/me", 200,
          "{\"user\":{\"userId\":\"fixture-user\",\"currentDevice\":{\"id\":\"fixture-device\",\"side\":\"" + side + "\"}}}"};
}
Reply temperature(std::string state = "smart:bedtime", std::string side = "right") {
  return {HTTP_METHOD_GET, "/temperature/all", 200,
          "{\"devices\":[{\"device\":{\"deviceId\":\"fixture-device\",\"side\":\"" + side +
              "\",\"specialization\":\"pod\"},\"currentState\":{\"type\":\"" + state + "\"}}]}"};
}
Reply put(int status = 204, esp_err_t error = ESP_OK) {
  return {HTTP_METHOD_PUT, "/hot-flash-mode/activate", status, "", error};
}
void beforePut() { replies = {auth(), identity(), temperature()}; }
unsigned count(esp_http_client_method_t method) {
  unsigned found = 0;
  for (const auto& call : recorded) if (call.method == method) ++found;
  return found;
}
void test(const char* name, void (*run)()) {
  resetFixture(); run(); assert(replies.empty());
  for (const auto& call : recorded) {
    if (call.method == HTTP_METHOD_PUT) assert(call.body.empty());
    if (call.method == HTTP_METHOD_POST) {
      JsonDocument payload;
      assert(!deserializeJson(payload, call.body));
      assert(payload["grant_type"] == "refresh_token");
      assert(payload["password"].isNull() && payload["username"].isNull());
      assert(call.headers.count("Authorization") == 0);
    } else assert(call.headers.count("Authorization") == 1);
  }
  std::cout << "PASS " << name << '\n';
}

int main() {
  test("confirmed native bodyless activation and persisted rotation", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    beforePut(); replies.push_back(put()); replies.push_back(temperature("hotFlash"));
    assert(client.activate(millis()) == ApiResult::Confirmed);
    assert(count(HTTP_METHOD_PUT) == 1 && saved.size() == 1);
    assert(c.refreshToken == "fixture-refresh-rotated");
  });
  test("already-active cycle is never restarted", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth(), identity(), temperature("hotFlash")};
    assert(client.activate(millis()) == ApiResult::AlreadyActive);
    assert(count(HTTP_METHOD_PUT) == 0);
  });
  test("identity-side mismatch fails closed", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth(), identity("left")};
    assert(client.activate(millis()) == ApiResult::NeedsSetup);
    client.maintain(); assert(count(HTTP_METHOD_PUT) == 0);
  });
  test("temperature-side mismatch fails closed", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth(), identity(), temperature("smart", "left")};
    assert(client.activate(millis()) == ApiResult::Failed);
    assert(count(HTTP_METHOD_PUT) == 0);
  });
  test("unknown temperature state fails closed", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth(), identity(), temperature("new-unknown-state")};
    assert(client.activate(millis()) == ApiResult::Failed);
    assert(count(HTTP_METHOD_PUT) == 0);
  });
  test("non-string temperature state fails closed", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    Reply invalid = temperature();
    const auto at = invalid.body.find("\"smart:bedtime\"");
    invalid.body.replace(at, strlen("\"smart:bedtime\""), "true");
    replies = {auth(), identity(), invalid};
    assert(client.activate(millis()) == ApiResult::Failed && count(HTTP_METHOD_PUT) == 0);
  });
  test("multiple matching pods fail closed", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    Reply duplicate = temperature();
    JsonDocument aggregate; deserializeJson(aggregate, duplicate.body);
    aggregate["devices"].as<JsonArray>().add(aggregate["devices"][0]);
    duplicate.body.clear(); serializeJson(aggregate, duplicate.body);
    replies = {auth(), identity(), duplicate};
    assert(client.activate(millis()) == ApiResult::Failed && count(HTTP_METHOD_PUT) == 0);
  });
  test("stale press never authenticates or writes", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    const auto press = millis(); fakeNowMs += 21000;
    assert(client.activate(press) == ApiResult::Failed && recorded.empty());
  });
  test("ambiguous write followed by active state is confirmed without replay", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    beforePut(); replies.push_back(put(0, ESP_FAIL)); replies.push_back(temperature("hotFlash"));
    assert(client.activate(millis()) == ApiResult::Confirmed);
    assert(count(HTTP_METHOD_PUT) == 1);
  });
  test("partial header write error still checks state without replay", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    Reply partial = put(0, ESP_FAIL); partial.sent = false;
    beforePut(); replies.push_back(partial); replies.push_back(temperature("hotFlash"));
    assert(client.activate(millis()) == ApiResult::Confirmed && count(HTTP_METHOD_PUT) == 1);
  });
  test("unconfirmed write blocks immediate repetition", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    beforePut(); replies.push_back(put(0, ESP_FAIL));
    replies.push_back(temperature()); replies.push_back(temperature());
    assert(client.activate(millis()) == ApiResult::Ambiguous);
    assert(client.activate(millis()) == ApiResult::Backoff);
    assert(count(HTTP_METHOD_PUT) == 1);
  });
  test("PUT 401 is never replayed", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    beforePut(); replies.push_back(put(401));
    assert(client.activate(millis()) == ApiResult::Failed);
    client.maintain(); assert(count(HTTP_METHOD_POST) == 1 && count(HTTP_METHOD_PUT) == 1);
  });
  test("GET 401 refreshes once and saves the rotated token", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth(), {HTTP_METHOD_GET, "/users/me", 401}, auth("fixture-refresh-again"),
               identity(), temperature(), put(), temperature("hotFlash")};
    assert(client.activate(millis()) == ApiResult::Confirmed);
    assert(count(HTTP_METHOD_POST) == 2 && saved.size() == 2 && count(HTTP_METHOD_PUT) == 1);
    JsonDocument payload; deserializeJson(payload, recorded[2].body);
    assert(payload["refresh_token"] == "fixture-refresh-rotated");
  });
  test("invalid grant stops all automatic login attempts", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {{HTTP_METHOD_POST, "/tokens", 400, "{\"error\":\"invalid_grant\"}"}};
    assert(client.activate(millis()) == ApiResult::NeedsSetup);
    fakeNowMs += 86400000; client.maintain(); assert(recorded.size() == 1);
  });
  test("rotation storage failure prevents bed control", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth()}; saveSucceeds = false;
    assert(client.activate(millis()) == ApiResult::NeedsSetup);
    assert(c.refreshToken == "fixture-refresh-original" && recorded.size() == 1);
  });
  test("refresh response identity mismatch fails closed", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    Reply wrong = auth();
    const auto at = wrong.body.find("fixture-user");
    wrong.body.replace(at, strlen("fixture-user"), "another-user");
    replies = {wrong};
    assert(client.activate(millis()) == ApiResult::NeedsSetup && saved.empty());
    client.maintain(); assert(recorded.size() == 1);
  });
  test("refresh response without rotation retains existing credential", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    Reply retained = auth();
    JsonDocument token; deserializeJson(token, retained.body); token.remove("refresh_token");
    retained.body.clear(); serializeJson(token, retained.body);
    replies = {retained}; client.maintain();
    assert(saved.empty() && c.refreshToken == "fixture-refresh-original");
  });
  test("idle refresh respects actual token expiry and never activates", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth("fixture-refresh-first", 600)}; client.maintain();
    fakeNowMs += 539000; client.maintain(); assert(recorded.size() == 1);
    fakeNowMs += 2000; replies.push_back(auth("fixture-refresh-second", 600)); client.maintain();
    assert(count(HTTP_METHOD_POST) == 2 && count(HTTP_METHOD_PUT) == 0);
  });
  test("429 Retry-After prevents queued or early activation", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    Reply rateLimit = identity(); rateLimit.status = 429; rateLimit.retryAfter = "120";
    replies = {auth(), rateLimit};
    assert(client.activate(millis()) == ApiResult::Backoff);
    fakeNowMs += 119000; assert(client.activate(millis()) == ApiResult::Backoff);
    fakeNowMs += 2000; client.maintain(); assert(count(HTTP_METHOD_PUT) == 0);
    replies = {identity(), temperature(), put(), temperature("hotFlash")};
    assert(client.activate(millis()) == ApiResult::Confirmed);
  });
  test("HTTP operation has an absolute bounded async deadline", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    Reply stalled = auth(); stalled.hang = true; replies = {stalled};
    const uint64_t started = fakeNowMs; client.maintain();
    assert(fakeNowMs - started <= 4050 && count(HTTP_METHOD_POST) == 1);
    replies.clear();
  });
  test("oversized response stops immediately and never writes", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    Reply enormous = identity(); enormous.body.assign(48 * 1024 + 1, 'x');
    replies = {auth(), enormous};
    assert(client.activate(millis()) == ApiResult::Backoff && count(HTTP_METHOD_PUT) == 0);
  });
  test("disconnect and reconnect do not replay a failed press", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    WiFi.state = 0; assert(client.activate(millis()) == ApiResult::Failed);
    WiFi.state = WL_CONNECTED; replies = {auth()}; client.maintain();
    assert(count(HTTP_METHOD_PUT) == 0);
  });
  test("tiny actual token lifetime cannot cause a login storm", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth("fixture-refresh-short", 1)}; client.maintain();
    for (unsigned i = 0; i < 59; ++i) { fakeNowMs += 1000; client.maintain(); }
    assert(count(HTTP_METHOD_POST) == 1);
  });
  test("authentication failures back off exponentially", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {{HTTP_METHOD_POST, "/tokens", 500}}; client.maintain();
    fakeNowMs += 59000; client.maintain(); assert(recorded.size() == 1);
    fakeNowMs += 1000; replies.push_back({HTTP_METHOD_POST, "/tokens", 500}); client.maintain();
    fakeNowMs += 119000; client.maintain(); assert(recorded.size() == 2);
    fakeNowMs += 1000; replies.push_back(auth()); client.maintain(); assert(recorded.size() == 3);
  });
  test("repeated GET 401 cannot loop refreshing", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth(), {HTTP_METHOD_GET, "/users/me", 401}, auth("fixture-refresh-again"),
               {HTTP_METHOD_GET, "/users/me", 401}};
    assert(client.activate(millis()) == ApiResult::Backoff);
    client.maintain(); assert(count(HTTP_METHOD_POST) == 2 && count(HTTP_METHOD_PUT) == 0);
  });
  test("long auth recovery discards the aging press before its write", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    replies = {auth(), {HTTP_METHOD_GET, "/users/me", 401}, auth("fixture-refresh-again"),
               identity(), temperature()};
    for (auto& reply : replies) reply.elapsed = 3500;
    assert(client.activate(millis()) == ApiResult::Failed && count(HTTP_METHOD_PUT) == 0);
  });
  test("millis wrap preserves short-press freshness", [] {
    auto c = config(); EightSleepClient client(c, saveToken);
    fakeNowMs = UINT32_MAX - 100;
    const auto press = millis(); fakeNowMs += 500;
    beforePut(); replies.push_back(put()); replies.push_back(temperature("hotFlash"));
    assert(client.activate(press) == ApiResult::Confirmed && count(HTTP_METHOD_PUT) == 1);
  });
}
