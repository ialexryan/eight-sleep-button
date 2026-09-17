#pragma once
#include <stdint.h>

enum class ButtonEvent { None, ShortPress, LongPress };

// Pure logic shared by the firmware and native tests. A boot-held button is inert.
class ButtonGate {
 public:
  ButtonEvent update(bool down, uint32_t now, bool blocked) {
    if (!initialized_) {
      initialized_ = true;
      raw_ = stable_ = down;
      changedAt_ = releasedAt_ = now;
    }
    if (raw_ != down) { raw_ = down; changedAt_ = now; }
    if (blocked && (raw_ || stable_)) suppressed_ = true;
    if (stable_ != raw_ && uint32_t(now - changedAt_) >= 40) {
      stable_ = raw_;
      if (stable_) {
        startedAt_ = now;
        suppressed_ = suppressed_ || !armed_ || blocked;
        longSent_ = false;
      } else {
        const uint32_t held = now - startedAt_;
        const bool shortPress = armed_ && !suppressed_ && !longSent_ &&
                                held >= 50 && held < 1200;
        releasedAt_ = now;
        suppressed_ = false;
        if (shortPress) return ButtonEvent::ShortPress;
      }
    }
    if (!stable_ && !raw_ && uint32_t(now - releasedAt_) >= 250) armed_ = true;
    if (stable_ && armed_ && !suppressed_ && !longSent_ &&
        uint32_t(now - startedAt_) >= 1500) {
      longSent_ = true;
      return ButtonEvent::LongPress;
    }
    return ButtonEvent::None;
  }
 private:
  bool initialized_ = false, raw_ = false, stable_ = false, armed_ = false;
  bool suppressed_ = false, longSent_ = false;
  uint32_t changedAt_ = 0, releasedAt_ = 0, startedAt_ = 0;
};
