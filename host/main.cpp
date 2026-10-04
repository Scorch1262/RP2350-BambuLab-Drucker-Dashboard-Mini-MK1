// Host-Testprogramm: startet die Dashboard-Logik auf Linux.
//   ./dashboard_host [--port 8080] [--data ./data]
#include <stdlib.h>
#include <string.h>
#include <string>
#include "app_main.h"
#include "config.h"
#include "plat.h"

extern std::string g_host_data_dir;
extern uint16_t g_host_port_override;

int main(int argc, char** argv) {
    uint16_t port = 8080;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) g_host_data_dir = argv[++i];
    }
    g_host_port_override = port;
    logf("[HOST] Testbetrieb, Daten in %s, Port %u", g_host_data_dir.c_str(), port);
    cfg::init();
    app::start(port);
    for (;;) {
        app::tick();
        plat::sleep_ms(100);
    }
}
