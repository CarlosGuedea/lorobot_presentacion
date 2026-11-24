#pragma once
#include <atomic>

class VideoRunState {
 public:
  void start() { stopping.store(false); }
  void stop() { stopping.store(true); }
  bool running(bool wifiConnected) const { return wifiConnected && !stopping.load(); }
 private:
  std::atomic<bool> stopping{true};
};
