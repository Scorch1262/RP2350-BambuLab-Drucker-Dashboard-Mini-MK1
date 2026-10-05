// WS2812-Status-LED auf dem Board (GP25)
//   weiss = Programm laeuft, lila = Speicher/Einstellungen, gelb = W5500 laeuft,
//   wartet auf DHCP, blau = Dienste starten, gruen (gedimmt) = Netzwerk ok,
//   orange blinkend = kein Netzwerk, rot = W5500 nicht gefunden
#pragma once
#include <stdint.h>
namespace statusled {
enum State { BOOT, OK, NO_NET, ERROR, START, STORAGE, NET_WAIT };
void begin(int pin);
void set(State s);   // wirkt sofort
void tick();   // aus loop() aufrufen
}
