#pragma once
#include <cstdint>
#include <cstring>
#include <string>

// Just the String/time surface used by the production API client and real
// ArduinoJson. Networking and time are deterministic in this host harness.
class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  String(const std::string& value) : value_(value) {}
  String& operator=(const char* value) { value_ = value ? value : ""; return *this; }
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }
  bool isEmpty() const { return value_.empty(); }
  char operator[](size_t index) const { return value_[index]; }
  bool concat(const char* value, size_t length) { value_.append(value, length); return true; }
  bool concat(const char* value) { return concat(value, strlen(value)); }
  friend String operator+(const String& left, const String& right) {
    return String(left.value_ + right.value_);
  }
  friend bool operator==(const String& left, const String& right) { return left.value_ == right.value_; }
  friend bool operator!=(const String& left, const String& right) { return !(left == right); }
 private:
  std::string value_;
};

extern uint64_t fakeNowMs;
inline uint32_t millis() { return static_cast<uint32_t>(fakeNowMs); }
inline void delay(uint32_t ms) { fakeNowMs += ms; }
