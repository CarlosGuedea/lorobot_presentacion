#include "../libraries/RobotLoroButton/src/button_clock.h"
#include <cassert>
#include <iostream>

int main() {
  ButtonClock clock;
  assert(!clock.ready(0));
  assert(clock.synchronize(1700000000000ULL, 100, 108));
  assert(clock.ready(1000));
  assert(clock.timestamp(104) == 1700000000000ULL);
  assert(clock.timestamp(90) == 1699999999986ULL);
  assert(clock.uncertainty(108) <= 10);
  assert(clock.readyForPress(90, 108));
  assert(!clock.ready(30200));
  assert(clock.synchronize(1700000000000ULL, 100, 137));
  assert(clock.ready(1000));
  assert(clock.uncertainty(1000) <= 40);
  assert(!clock.synchronize(1700000000000ULL, 0, 2000));
  assert(!clock.ready(2001));
  assert(clock.synchronize(1700000000000ULL, 0xfffffff0, 0xfffffff8));
  assert(clock.ready(100));
  assert(clock.timestamp(0xfffffff4) == 1700000000000ULL);
  assert(clock.timestamp(4) == 1700000000016ULL);
  std::cout << "button clock OK\n";
}
