#include "../esp32cam_web/video_snapshot.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

bool failAllocation = false;
void *allocate(size_t n) { return failAllocation ? nullptr : malloc(n); }

int main() {
  VideoSnapshots history(allocate);
  uint8_t a[] = {0xff,0xd8,1,0xff,0xd9};
  uint8_t b[] = {0xff,0xd8,2,0xff,0xd9};
  assert(history.sourceAt(100) == SnapshotSource::Camera);
  history.begin(100);
  assert(history.sourceAt(200) == SnapshotSource::Video);
  assert(!history.copyAt(200));
  assert(history.store(a,sizeof(a),150,180));
  assert(history.store(b,sizeof(b),250,280));
  auto selected = history.copyAt(220);
  assert(selected && selected.size == sizeof(a) && selected.data[2] == 1);
  // Historial puede continuar y sobrescribir; la foto elegida no cambia.
  a[2] = 99;
  for (unsigned i=0;i<10;++i) history.store(b,sizeof(b),300+i*200,310+i*200);
  assert(selected.data[2] == 1);
  assert(!history.copyAt(220)); // candidato expulsado: no tomar uno posterior
  assert(!history.copyAt(5000)); // ultimo frame demasiado antiguo
  assert(history.copyAt(3600)); // 1500ms exactos desde adquisicion
  assert(!history.copyAt(3601));
  assert(!history.store(b,65537,100,100));
  failAllocation = true;
  assert(!history.store(b,sizeof(b),2400,2410));
  assert(!history.copyAt(2200));
  failAllocation = false;
  history.end(2500);
  assert(history.sourceAt(2600) == SnapshotSource::Camera);
  assert(history.sourceAt(2400) == SnapshotSource::Video);
  assert(history.copyAt(2400)); // se procesa despues de cerrar el video
  assert(!history.copyAt(2600));
  history.begin(3000);
  assert(history.sourceAt(2900) == SnapshotSource::Unavailable);
  assert(!history.copyAt(3100));
  history.begin(0xfffffff0);
  assert(history.store(b,sizeof(b),0xfffffff5,0xfffffffa));
  assert(history.copyAt(10));
  assert(history.sourceAt(10) == SnapshotSource::Video);
  std::cout << "Fotograma: seleccion temporal, copia, memoria y sesiones correctas\n";
}
