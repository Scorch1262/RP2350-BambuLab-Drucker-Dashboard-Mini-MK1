// Konfiguration (entspricht config.json des Python-Originals, gespeichert
// im LittleFS des RP2350). Gleiches Format wie MK6 - eine config.json vom
// PC/Router laesst sich im Einstellungen-Modus importieren.
#pragma once
#include <ArduinoJson.h>
#include <string>
#include "plat.h"

#define APP_NAME "Drucker Dashboard RP2350"
#define APP_VERSION "1.1.2"
#define APP_BASE "MK6 v2.5.5"

namespace cfg {

// Bekannte Druckertypen (wie KNOWN_TYPES im Original)
bool is_known_type(const std::string& t);
bool is_formlabs(const std::string& t);
bool is_creality(const std::string& t);
bool is_bambu_family(const std::string& f);

void init();                    // laden (oder Standard anlegen) + Migration
plat::Mutex& mtx();             // VOR jedem Zugriff auf doc() sperren!
JsonDocument& doc();
void mark_dirty();              // speichert verzoegert (aus der Hauptschleife)
void save_if_dirty();           // nur aus der Hauptschleife (LittleFS ist nicht threadsicher)
bool dirty();

// Hilfen (Aufrufer haelt mtx())
JsonObject find_printer(const std::string& id);
JsonObject find_by_id(const char* list, const std::string& id);
bool group_exists(const std::string& id);

std::string export_json();
bool import_json(const std::string& text, std::string* err, std::string* warnings);

// Defaults fuer neue Felder ergaenzen (wie load_config() im Original)
void migrate(JsonDocument& d);

} // namespace cfg
