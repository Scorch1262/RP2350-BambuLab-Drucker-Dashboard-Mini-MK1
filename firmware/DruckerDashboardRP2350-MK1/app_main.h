// Gemeinsamer Start/Hauptschleife fuer Geraet und Host-Test
#pragma once
#include <stdint.h>

namespace app {
void start(uint16_t http_port);   // nach Netzwerk-Start aufrufen
void tick();                      // regelmaessig aus der Hauptschleife (z. B. alle 100 ms)
}
