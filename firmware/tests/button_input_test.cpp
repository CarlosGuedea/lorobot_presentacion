#include "../libraries/RobotLoroButton/src/button_input.h"
#include <cassert>
#include <iostream>

int main() {
  ButtonInput b;
  b.begin(true, 0);
  b.recordLevel(false, 100);
  b.recordLevel(true, 250);
  assert(b.poll(400)); // pulso completo durante HTTP
  assert(b.pressedMillis() == 100);
  assert(!b.poll(401));
  b.begin(true, 0);
  b.recordLevel(false, 100);
  b.recordLevel(true, 105); // rebote corto
  b.recordLevel(false, 115);
  assert(!b.poll(200));
  assert(b.poll(215));
  assert(b.pressedMillis() == 115);
  assert(!b.poll(10000)); // mantener presionado no repite
  b.recordLevel(true, 10100);
  b.recordLevel(false, 10250);
  assert(b.poll(10350));
  b.begin(false, 0); // iniciar pulsado exige soltar
  assert(!b.poll(50));
  b.recordLevel(true, 100);
  b.recordLevel(false, 250);
  assert(b.poll(350));
  b.begin(true, 0);
  b.recordLevel(false, 100);
  b.recordLevel(true, 105);
  assert(!b.poll(200)); // esperar no convierte rebote en pulsacion
  b.begin(true, 0);
  b.recordLevel(false, 100);
  b.recordLevel(true, 250);
  b.recordLevel(false, 400);
  b.recordLevel(true, 550); // conserva la primera pulsacion pendiente
  assert(b.poll(700));
  assert(b.pressedMillis() == 100);
  b.recordLevel(false, 900);
  assert(b.poll(1000));
  assert(b.pressedMillis() == 900);
  b.begin(true, 0);
  b.recordLevel(false, 100);
  b.recordLevel(true, 250);
  b.discardPending(); // ocupado: no capturar una imagen tardia al acabar el envio
  assert(!b.poll(400));
  b.recordLevel(false, 500);
  assert(b.poll(600));
  b.discardPending(); // sigue pulsado al dejar de estar ocupado
  assert(!b.poll(1000));
  b.recordLevel(true, 1100);
  b.recordLevel(false, 1250);
  assert(b.poll(1350));
  b.begin(true, 0xfffffff0);
  b.recordLevel(false, 0xfffffff5);
  assert(!b.poll(4));
  assert(!b.poll(25));
  assert(b.poll(90));
  std::cout << "button input OK\n";
}
