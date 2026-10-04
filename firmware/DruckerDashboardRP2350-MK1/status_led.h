// WS2812-Status-LED auf dem Board (GP25)
//   blau = Start, gruen (gedimmt) = Netzwerk ok, orange blinkend = kein Netzwerk,
//   rot = W5500 nicht gefunden
#pragma once
#include <stdint.h>
namespace statusled {
enum State { BOOT, OK, NO_NET, ERROR };
void begin(int pin);
void set(State s);
void tick();   // aus loop() aufrufen
}
