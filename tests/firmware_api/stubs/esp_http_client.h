#pragma once
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_HTTP_EAGAIN = 1;
enum esp_http_client_method_t { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT };
enum { HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_EVENT_DISCONNECTED };
enum { HTTP_TRANSPORT_OVER_SSL = 1 };
struct MockHttpClient;
using esp_http_client_handle_t = MockHttpClient*;
struct esp_http_client_event_t {
  int event_id;
  esp_http_client_handle_t client;
  void* data;
  int data_len;
  void* user_data;
  char* header_key;
  char* header_value;
};
struct esp_http_client_config_t {
  const char* url = nullptr;
  esp_http_client_method_t method = HTTP_METHOD_GET;
  int timeout_ms = 0;
  bool disable_auto_redirect = false;
  int max_authorization_retries = 0;
  esp_err_t (*crt_bundle_attach)(void*) = nullptr;
  bool skip_cert_common_name_check = true;
  int transport_type = 0;
  esp_err_t (*event_handler)(esp_http_client_event_t*) = nullptr;
  void* user_data = nullptr;
  bool is_async = false;
  int buffer_size = 0;
  int buffer_size_tx = 0;
  const char* user_agent = nullptr;
};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char*, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
