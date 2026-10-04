// Zweiter, unabhaengiger MQTT-Broker fuer Sensoren/Schalter (ExtrasMqttManager)
#pragma once
#include <string>
#include <ArduinoJson.h>

namespace extras {

void restart();     // (neu) starten mit den aktuellen Einstellungen aus cfg
// Liefert zuletzt empfangenen Wert; false falls (noch) keiner
bool get_value(const std::string& topic, std::string& out);
void list_topics(JsonArray out, size_t limit = 300);
bool publish(const std::string& topic, const std::string& payload);
std::string state_text();   // fuer Statusanzeige im Einstellungen-Modus

} // namespace extras
