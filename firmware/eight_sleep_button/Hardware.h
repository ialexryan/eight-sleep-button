#pragma once

#include <Arduino.h>

enum class FeedbackStatus : uint8_t {
  Busy,
  Success,
  Failure,
  Diagnostics,
  Setup,
};

// Only the main/UI task should call this class. Network work must not block tick().
// No button handling or Eight Sleep API calls belong in this module.
class Hardware {
 public:
  // Audio is opt-in. No startup sound, and the microphone is never started.
  // Returns false if neither a supported display nor an AtomS3 Lite was detected.
  bool begin(bool audioEnabled = false);
  void show(FeedbackStatus status, const char* detail = nullptr);
  void tick();
  void idle();

  // Short, non-secret diagnostics; safe for serial output.
  String diagnostics() const;
  bool hasDisplay() const { return hasDisplay_; }
  bool audioAvailable() const { return audioAvailable_; }
  bool echoBasePresent() const { return echoBasePresent_; }
  const char* boardName() const { return boardName_; }
  const char* panelName() const { return panelName_; }

 private:
  void drawStatus(FeedbackStatus status, const char* detail);
  void playConfirmation(FeedbackStatus status);

  bool initialized_ = false;
  bool supported_ = false;
  bool hasDisplay_ = false;
  bool hasLed_ = false;
  bool echoBasePresent_ = false;
  bool audioRequested_ = false;
  bool audioAvailable_ = false;
  bool visible_ = false;
  bool speakerActive_ = false;
  uint32_t visibleUntil_ = 0;
  uint32_t speakerStopAt_ = 0;
  const char* boardName_ = "unknown";
  const char* panelName_ = "none";
};
