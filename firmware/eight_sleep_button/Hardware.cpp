#include "Hardware.h"

#include <M5Unified.h>

namespace {

// These are deliberately low starting values, to be checked in a dark room.
constexpr uint8_t kDisplayBrightness = 8;  // 0..255
constexpr uint8_t kLedBrightness = 8;      // 0..255
constexpr uint8_t kSpeakerVolume = 8;      // 0..255

bool deadlinePassed(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool probeEchoBase() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // The same temporary bus switch used by M5Unified's Atomic Echo callback.
  // A register read checks presence without starting the codec or microphone.
  // Restore the AtomS3R's internal I2C bus before any display/backlight writes.
  m5gfx::i2c::i2c_temporary_switcher_t bus(1, 38, 39);
  const auto result = m5gfx::i2c::readRegister8(1, 0x18, 0x00, 100000);
  bus.restore();
  return result.has_value();
#else
  return false;
#endif
}

uint32_t displayDuration(FeedbackStatus status) {
  switch (status) {
    case FeedbackStatus::Busy: return 1500;
    case FeedbackStatus::Success: return 1200;
    case FeedbackStatus::Failure: return 2500;
    case FeedbackStatus::Diagnostics: return 5000;
    case FeedbackStatus::Setup: return 4000;
  }
  return 1500;
}

const char* statusTitle(FeedbackStatus status) {
  switch (status) {
    case FeedbackStatus::Busy: return "Sending";
    case FeedbackStatus::Success: return "Cooling";
    case FeedbackStatus::Failure: return "Failed";
    case FeedbackStatus::Diagnostics: return "Status";
    case FeedbackStatus::Setup: return "Set up";
  }
  return "Status";
}

struct Color {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

Color statusColor(FeedbackStatus status) {
  switch (status) {
    case FeedbackStatus::Busy: return {150, 95, 25};
    case FeedbackStatus::Success: return {35, 140, 105};
    case FeedbackStatus::Failure: return {165, 45, 25};
    case FeedbackStatus::Diagnostics: return {75, 105, 145};
    case FeedbackStatus::Setup: return {140, 100, 30};
  }
  return {75, 105, 145};
}

void drawCentered(const char* text, int y, uint8_t size) {
  M5.Display.setTextSize(size);
  const int x = (M5.Display.width() - M5.Display.textWidth(text)) / 2;
  M5.Display.setCursor(x < 0 ? 0 : x, y);
  M5.Display.print(text);
}

}  // namespace

bool Hardware::begin(bool audioEnabled) {
  if (initialized_) return supported_;
  initialized_ = true;
  audioRequested_ = audioEnabled;

  // M5Unified saves and restores the display's brightness during begin(). Set
  // zero beforehand so initialization cannot restore its default full value.
  M5.Display.setBrightness(0);
  auto config = M5.config();
  config.serial_baudrate = 0;  // The application owns serial and its redaction.
  config.clear_display = true;
  config.internal_imu = false;
  config.external_imu = false;
  config.internal_rtc = false;
  config.external_rtc = false;
  config.internal_mic = false;
  config.internal_spk = false;
  config.external_display_value = 0;
  config.external_speaker_value = 0;
  config.external_speaker.atomic_echo = audioEnabled;
  config.led_brightness = 0;
  config.fallback_board = m5::board_t::board_unknown;
  M5.begin(config);

  // Atomic Echo speaker configuration also supplies a microphone pin mapping.
  // Explicitly invalidate it even though we never call Mic.begin()/record().
  auto microphone = M5.Mic.config();
  microphone.pin_data_in = -1;
  M5.Mic.config(microphone);

  const auto board = M5.getBoard();
  if (board == m5::board_t::board_M5AtomS3R) {
    boardName_ = "AtomS3R";
    hasDisplay_ = M5.Display.getBoard() == m5::board_t::board_M5AtomS3R &&
                  M5.Display.width() == 128 && M5.Display.height() == 128;
    supported_ = hasDisplay_;
    if (hasDisplay_) {
      // Both panel revisions are selected by M5GFX's hardware ID probe. Its
      // selected configuration tells us which driver was actually detected.
      const auto& panel = M5.Display.panel()->config();
      if (panel.memory_height == 132 && panel.offset_y == 1) {
        panelName_ = "ST7735 (M5GFX autodetect)";
      } else if (panel.offset_y == 32) {
        panelName_ = "GC9107 (M5GFX autodetect)";
      } else {
        panelName_ = "unrecognized M5GFX panel";
      }
      M5.Display.setTextWrap(false);
      M5.Display.fillScreen(TFT_BLACK);
    }
  } else if (board == m5::board_t::board_M5AtomS3Lite) {
    // Runtime feedback support is ready for a separately configured Lite build
    // (no PSRAM). Do not treat a failed S3R display probe as a Lite board.
    boardName_ = "AtomS3 Lite";
    supported_ = true;
    hasLed_ = M5.Led.begin();
    if (hasLed_) {
      M5.Led.setAutoDisplay(false);
      M5.Led.setBrightness(kLedBrightness);
      M5.Led.setAllColor(0, 0, 0);
      M5.Led.display();
    }
  }

  if (supported_) echoBasePresent_ = probeEchoBase();
  audioAvailable_ = audioEnabled && echoBasePresent_ && M5.Speaker.isEnabled();
  M5.Speaker.setVolume(kSpeakerVolume);
  if (!audioAvailable_) {
    // An unplugged or absent base is a normal supported configuration.
    auto speaker = M5.Speaker.config();
    speaker.pin_data_out = -1;
    M5.Speaker.config(speaker);
  }

  idle();
  return supported_;
}

void Hardware::show(FeedbackStatus status, const char* detail) {
  if (!initialized_) return;
  drawStatus(status, detail);
  visible_ = hasDisplay_ || hasLed_;
  visibleUntil_ = millis() + displayDuration(status);
  playConfirmation(status);
}

void Hardware::drawStatus(FeedbackStatus status, const char* detail) {
  const auto color = statusColor(status);
  if (hasLed_) {
    M5.Led.setAllColor(color.red, color.green, color.blue);
    M5.Led.display();
  }
  if (!hasDisplay_) return;

  M5.Display.setBrightness(0);
  M5.Display.wakeup();
  M5.Display.fillScreen(TFT_BLACK);
  const uint16_t foreground = M5.Display.color565(color.red, color.green, color.blue);
  M5.Display.setTextColor(foreground, TFT_BLACK);
  M5.Display.drawCircle(64, 31, 13, foreground);
  switch (status) {
    case FeedbackStatus::Success:
      M5.Display.drawLine(57, 31, 62, 36, foreground);
      M5.Display.drawLine(62, 36, 72, 25, foreground);
      break;
    case FeedbackStatus::Failure:
      M5.Display.drawLine(59, 26, 69, 36, foreground);
      M5.Display.drawLine(69, 26, 59, 36, foreground);
      break;
    case FeedbackStatus::Busy:
      M5.Display.fillCircle(59, 31, 1, foreground);
      M5.Display.fillCircle(64, 31, 1, foreground);
      M5.Display.fillCircle(69, 31, 1, foreground);
      break;
    default:
      M5.Display.drawLine(64, 26, 64, 33, foreground);
      M5.Display.drawPixel(64, 37, foreground);
      break;
  }
  drawCentered(statusTitle(status), 57, 2);
  if (detail != nullptr && detail[0] != '\0') {
    // A caller may pass a short, non-secret detail. Bound it to the display;
    // never print API responses, SSIDs, credentials, or tokens here.
    char line[21] = {};
    size_t length = 0;
    while (length < sizeof(line) - 1 && detail[length] != '\0') {
      const char c = detail[length];
      line[length++] = (c >= 32 && c <= 126) ? c : ' ';
    }
    drawCentered(line, 84, 1);
  }
  M5.Display.setBrightness(kDisplayBrightness);
}

void Hardware::playConfirmation(FeedbackStatus status) {
  if (!audioAvailable_ ||
      (status != FeedbackStatus::Success && status != FeedbackStatus::Failure)) {
    return;
  }
  // No tone for Wi-Fi readiness, boot, or request dispatch. A success tone is
  // reserved for a server-confirmed activation reported by the API layer.
  const bool success = status == FeedbackStatus::Success;
  const uint32_t duration = success ? 65 : 100;
  M5.Speaker.setVolume(kSpeakerVolume);
  if (M5.Speaker.tone(success ? 660.0f : 220.0f, duration, 0, true)) {
    speakerActive_ = true;
    speakerStopAt_ = millis() + duration + 150;
  }
}

void Hardware::tick() {
  const uint32_t now = millis();
  if (visible_ && deadlinePassed(now, visibleUntil_)) idle();
  if (speakerActive_ && deadlinePassed(now, speakerStopAt_)) {
    // Stop the I2S task and disable the base's amplifier between confirmations.
    M5.Speaker.end();
    speakerActive_ = false;
  }
}

void Hardware::idle() {
  visible_ = false;
  if (hasDisplay_) {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
  }
  if (hasLed_) {
    M5.Led.setAllColor(0, 0, 0);
    M5.Led.display();
  }
}

String Hardware::diagnostics() const {
  String result = "board=";
  result += boardName_;
  result += "; panel=";
  result += panelName_;
  result += "; base=";
  result += echoBasePresent_ ? "codec responds" : "not detected";
  result += "; audio=";
  result += !audioRequested_ ? "off" : (audioAvailable_ ? "enabled" : "unavailable");
  result += "; microphone=disabled";
  return result;
}
