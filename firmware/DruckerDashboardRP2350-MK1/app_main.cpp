#include "app_main.h"
#include "api.h"
#include "config.h"
#include "extras.h"
#include "httpd.h"
#include "plat.h"
#include "printers.h"
#include "tls.h"
#include "bambu.h"
#include "camera.h"
#include "pollers.h"

namespace app {

static uint32_t g_last_hk = 0;

void start(uint16_t http_port) {
    // Alle modulweiten Mutexe/Tabellen anlegen, BEVOR weitere Aufgaben laufen
    // (die Module legen sie sonst beim ersten Aufruf an - das waere ein Wettlauf).
    log_dump();
    bambu::housekeeping();
    ultimaker::housekeeping();
    extras::state_text();
    printers::all();
    camera::active_streams();
    tls::global_init();
    api::register_routes();
    http::start(http_port);
    if (plat::safe_mode()) {
        logf("[SYS] ABGESICHERTER MODUS: Das Board ist mehrmals hintereinander abgestuerzt (letzter Neustart: %s). "
             "Drucker-, Kamera- und MQTT-Verbindungen werden NICHT gestartet, damit die Seite erreichbar bleibt. "
             "Nach einem Neustart ueber Einstellungen -> System laeuft wieder alles normal.",
             plat::boot_reason().c_str());
    } else {
        printers::start_all();
        extras::restart();
    }
    logf("[SYS] %s v%s (portiert von %s) laeuft auf %s.", APP_NAME, APP_VERSION, APP_BASE, plat::platform_name());
}

void tick() {
    cfg::save_if_dirty();
    if (plat::millis() - g_last_hk > 5000) {
        g_last_hk = plat::millis();
        api::housekeeping();
    }
}

} // namespace app
