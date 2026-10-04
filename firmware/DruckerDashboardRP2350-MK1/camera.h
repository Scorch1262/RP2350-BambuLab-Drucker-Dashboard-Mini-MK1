// Kamera-Weiterleitung
//
//  - Bambu A1-Serie: proprietaeres JPEG-ueber-TLS (Port 6000) -> MJPEG
//    (multipart/x-mixed-replace) fuer ein <img>-Element, wie im Original.
//  - Bambu X1/P1/P2/H2/X2 (RTSPS Port 322) und externe RTSP(S)-Kameras:
//    Das Original hat hier FFmpeg nach MJPEG umkodiert - das ist auf einem
//    Mikrocontroller nicht moeglich. Stattdessen leitet das Board die
//    H.264-RTP-Pakete unveraendert an den Browser weiter; der Browser setzt
//    daraus fragmentiertes MP4 zusammen und spielt es per Media Source
//    Extensions ab (siehe rtspPlayer im Frontend).
#pragma once
#include <ArduinoJson.h>
#include <memory>
#include "httpd.h"

class PrinterConn;
class BambuConn;

namespace camera {

void serve_bambu_mjpeg(http::Request& r, std::shared_ptr<BambuConn> p);
void serve_bambu_rtsp(http::Request& r, std::shared_ptr<BambuConn> p);
void serve_external_rtsp(http::Request& r, const std::string& url, const std::string& user, const std::string& pass,
                         const std::string& label);
int active_streams();

} // namespace camera
