#include "../firmware/eight_sleep_button/ButtonGate.h"
#include <cassert>
#include <cstdio>

int main() {
  ButtonGate b;
  assert(b.update(true, 0, false) == ButtonEvent::None);
  assert(b.update(true, 5000, false) == ButtonEvent::None);
  b.update(false, 5010, false);
  assert(b.update(false, 5060, false) == ButtonEvent::None);
  b.update(false, 5400, false);
  b.update(true, 5500, false); b.update(true, 5540, false);
  b.update(false, 5640, false);
  assert(b.update(false, 5680, false) == ButtonEvent::ShortPress);
  b.update(true, 5800, false); b.update(false, 5810, false);
  assert(b.update(false, 5900, false) == ButtonEvent::None); // bounce
  b.update(true, 6000, false); b.update(true, 6040, false);
  assert(b.update(true, 7600, false) == ButtonEvent::LongPress);
  b.update(false, 7700, false);
  assert(b.update(false, 7750, false) == ButtonEvent::None);
  b.update(true, 8000, true); b.update(true, 8050, true);
  b.update(false, 8200, false);
  assert(b.update(false, 8250, false) == ButtonEvent::None); // busy press discarded
  ButtonGate rollover;
  rollover.update(false, UINT32_MAX-300, false);
  rollover.update(false, UINT32_MAX-20, false);
  rollover.update(true, UINT32_MAX-10, false);
  rollover.update(true, 40, false);
  rollover.update(false, 150, false);
  assert(rollover.update(false, 200, false) == ButtonEvent::ShortPress);
  puts("Button safety tests passed");
}
