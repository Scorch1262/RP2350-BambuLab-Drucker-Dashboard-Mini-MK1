// REST-API (Portierung der Flask-Routen aus app.py, ohne Verlauf/Warteschlange)
#pragma once

namespace api {
void register_routes();
void housekeeping();   // periodisch aus der Hauptschleife
}
