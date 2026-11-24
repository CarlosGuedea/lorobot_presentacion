#include "../esp32cam_web/video_state.h"
#include <cassert>
#include <chrono>
#include <future>
#include <thread>
#include <iostream>

int main() {
  VideoRunState state;
  assert(!state.running(true));
  state.start();
  assert(state.running(true));
  assert(!state.running(false));
  // El cliente esta pausado por una foto; WiFi ya se reconecto cuando se pide
  // parar. La tarea debe salir aunque no envie frames ni se cierre el socket.
  auto worker = std::async(std::launch::async, [&]() {
    while (state.running(true)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  state.stop();
  assert(worker.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
  worker.get();
  state.start();
  assert(state.running(true));
  state.stop();
  std::cout << "Video: parada y reinicio correctos\n";
}
