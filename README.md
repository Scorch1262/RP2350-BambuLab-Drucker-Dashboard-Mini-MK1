# Drucker Dashboard RP2350 (MK1)

Firmware-Version des **Drucker Dashboard MK6 (v2.5.5)** für das
**Seengreat RP2350-Mini-ETH** (RP2350A + W5500-Ethernet). Statt auf einem
PC/Router läuft das Dashboard direkt auf dem Board – Netzwerkkabel und
USB-Strom anschließen, fertig.

| Druckertyp | Anbindung | Kamera | Druck per Drag & Drop |
|---|---|:-:|:-:|
| **Bambu Lab** (X1/A1/H2/P1/P2/X2) | MQTT/TLS | ✅ (siehe 6.) | ✅ (`.gcode.3mf`, mit AMS-Dialog) |
| **Ultimaker** (UM3, S-Serie, Factor 4) | REST-API | ✅ | ✅ (`.gcode`, nach Kopplung) |
| **OctoPrint** | REST-API | ✅ | – |
| **Creality** (K1/K1C/K1 Max/K1 SE, Klipper) | Moonraker | ✅ | – |
| **Formlabs** (Drucker/Wash L/Cure L) | PreFormServer | – | – |

Dazu wie bisher: Räume/Gruppen, externe RTSP-Kameras, MQTT-Sensoren und
-Schalter (auch ohne Drucker), Sparklines, 1–4-spaltiges Layout und
Einstellungen-Modus.

**Gegenüber MK6 entfernt (auf Wunsch):** Druckauftrags-**Verlauf** und
**Warteschlange** – und damit auch das Vorschaubild neben dem
Fortschrittsbalken (das kam aus dem Verlauf). Ist ein Drucker gerade
beschäftigt, lehnt das Dashboard einen neuen Auftrag deshalb mit einem
Hinweis ab, statt ihn einzureihen.

---

## 1. Firmware aufspielen

1. **BOOT**-Taste gedrückt halten, Board per USB-C anschließen (oder bei
   angeschlossenem Board BOOT halten und kurz **RUN/Reset** drücken).
2. Es erscheint ein Laufwerk **RP2350**.
3. `DruckerDashboardRP2350-MK1-v1.0.2.uf2` darauf ziehen – das Board startet
   neu.

Spätere Updates gehen auch ohne BOOT-Taste: *Einstellungen → System →
Firmware-Update (.bin)* mit der `.bin`-Datei (nicht der `.uf2`).

## 2. Erster Start

* Netzwerkkabel anschließen. Das Board holt sich per **DHCP** eine
  IP-Adresse.
* Aufruf im Browser:
  **`http://drucker-dashboard.local/`** (mDNS) oder über die IP-Adresse,
  die der Router anzeigt. Port **80** (nicht mehr 8000).
* Alternativ zeigt der serielle Monitor (USB, 115200 Baud) die IP an.

**Status-LED (Startphasen):**

| Farbe | Bedeutung |
|---|---|
| weiß | Programm läuft (erste Zeile im Programm erreicht) |
| lila | Speicher / Einstellungen werden geladen |
| gelb | W5500 läuft, wartet auf IP-Adresse (DHCP) |
| blau | Zeit, mDNS, Webserver und Drucker werden gestartet |
| grün (gedimmt) | Netzwerk ok – Dashboard erreichbar |
| orange blinkend | Netzwerkverbindung verloren / keine IP |
| rot | W5500 antwortet nicht |
| aus | Programm startet gar nicht |

**Nicht erreichbar? So eingrenzen:**

1. Seriellen Monitor öffnen (Arduino IDE oder z. B. PuTTY, USB-Port des
   Boards, **115200 Baud**) und den Reset-Taster drücken. Erwartet wird:
   ```
   ==== Drucker Dashboard RP2350 v1.0.2 startet ====
   Letzter Neustart: Einschalten
   [NET] W5500 gestartet, MAC ...
   [NET] IP-Adresse: 192.168.x.y  ->  http://192.168.x.y/
   ```
2. Die LED-Farbe zeigt, in welcher Startphase das Board stehen bleibt
   (Tabelle oben); der serielle Monitor zeigt die Schritte `[1/5]` bis `[5/5]`.
3. **Orange blinkend** = kein Link oder keine DHCP-Adresse: Kabel, Switch-Port
   und DHCP-Server prüfen (in der Fritz!Box o. ä. nach „drucker-dashboard“
   suchen).
4. `.local`-Namen funktionieren nicht in jedem Netz (z. B. manche
   Android-Geräte, VLANs) – dann die IP-Adresse verwenden.
5. Unter Einstellungen → System wird der **letzte Neustartgrund** angezeigt
   (z. B. „Watchdog“ oder „Stack-Überlauf in Aufgabe …“).

## 3. Bestehende Konfiguration übernehmen

*Einstellungen → System → „config.json laden“* nimmt die `config.json` der
PC-/Router-Version (MK6) direkt an: Drucker, Räume, Kameras, Sensoren und
Ultimaker-Kopplungen werden übernommen, `history_max_jobs` wird ignoriert.
*„config.json sichern“* lädt die aktuelle Board-Konfiguration herunter.

Nach dem Import bitte prüfen:

* **Formlabs:** Unter *Einstellungen → Allgemein & Netzwerk* die
  **PreFormServer-Adresse** auf die IP des PCs setzen, auf dem
  `PreFormServer.exe --port 44388` läuft. `localhost` funktioniert auf
  dem Board nicht (der Import weist darauf hin).

## 4. Einstellungen (neu gegenüber MK6)

*Einstellungen → Allgemein & Netzwerk:* PreFormServer-Adresse, DHCP oder
feste IP (IP, Maske, Gateway, DNS), Gerätename (mDNS), Zeitserver,
Zeitzone (POSIX-Format, Standard Deutschland
`CET-1CEST,M3.5.0,M10.5.0/3`). Netzwerk-/Zeitänderungen gelten nach einem
Neustart.

*Einstellungen → System:* Version, IP/MAC, freier Speicher,
**Diagnose-Log** (ersetzt die Server-Konsole, z. B. `[MK6-MQTT]`- und
`[MK6]`-FTPS-Zeilen), Konfiguration sichern/laden, Firmware-Update,
Neustart.

## 5. Drucken per Drag & Drop

**Bambu:** `.gcode.3mf` auf die Karte ziehen. Neu: Die Datei wird **im
Browser** ausgewertet (Filamente von Plate 1, AMS-Vorschlag – identische
Regeln wie MK6: Farbtoleranz 30, `ASA` ≠ `ASA-CF`, AMS HT = 128 …). Nach
„Drucken starten“ lädt der Browser die Datei aufs Board, das sie
**ohne Zwischenspeichern** per FTPS (Port 990) an den Drucker weiterreicht
und danach `project_file` per MQTT sendet. Der Fortschrittsbalken zeigt den
echten Übertragungsstand.

* Wie in MK6: bis zu **3 Versuche** mit wechselndem FTPS-Profil
  (`x1`: TLS 1.2 + Sitzungs-Wiederverwendung, `a1`: TLS 1.3 erlaubt, ohne
  Wiederverwendung, Datenverbindung ohne TLS-Abschluss), MQTT wird während
  des Uploads getrennt, `553` → Hinweis auf SD-Karte/USB-Stick,
  H2/P2 → `ftp:///`-Pfad, H2 → `ams_mapping2`.
* Die Datei liegt nur im Browser: Bei einem Fehler einfach erneut
  „Drucken starten“ klicken. Das Browserfenster muss bis zum Ende offen
  bleiben.

**Ultimaker:** wie bisher „Jetzt koppeln“, dann `.gcode` auf die Karte
ziehen (Digest-Auth, ohne Auth bei Nachbauten wie *Ultimaker Connect
Raspi*).

## 6. Kameras

| Kamera | Umsetzung auf dem Board |
|---|---|
| Bambu **A1-Serie** (Port 6000) | wie MK6 als MJPEG weitergereicht |
| Bambu **X1/P1/P2/H2/X2** (RTSPS 322) | H.264 wird durchgereicht, der Browser zeigt es an |
| **Externe RTSP(S)-Kameras** | ebenso (Benutzername/Passwort, Basic oder Digest) |
| OctoPrint / Creality / Ultimaker | Browser lädt die Webcam direkt (Weiterleitung, wie MK6) |

MK6 hat RTSP-Streams per FFmpeg in MJPEG umgewandelt – dafür reicht ein
Mikrocontroller nicht. Das Board leitet deshalb die H.264-Pakete
unverändert weiter, und der Browser baut daraus selbst ein Video. Daraus
folgen ein paar Grenzen:

* Die Kamera muss **H.264** senden. H.265/HEVC wird nicht angezeigt (bei
  vielen IP-Kameras lässt sich für einen zweiten Stream/„Substream“ H.264
  einstellen). Bambu-Drucker senden H.264.
* Der Browser braucht **Media Source Extensions** mit H.264: Chrome, Edge,
  Firefox, Safari auf dem Mac, Android – ja. Auf **iPhones** frühestens ab iOS 17.1
  (ManagedMediaSource, ungetestet), auf älteren gar nicht.
* Höchstens **zwei Kamera-Streams gleichzeitig** (Arbeitsspeicher des
  Boards). Wie bisher muss am Bambu-Drucker „LAN Only Liveview“ aktiv
  sein.

## 7. Grenzen & Hinweise

* Zertifikate werden – wie in MK6 – nicht geprüft (Drucker nutzen
  selbstsignierte Zertifikate).
* Keine Anmeldung/Passwort für die Weboberfläche (wie MK6) – nur im
  eigenen LAN betreiben.
* Max. 6 gleichzeitige Browser-Verbindungen. Mehrere offene
  Dashboard-Tabs sind kein Problem, die Seite fragt alle 2,5 s mit **einer**
  Anfrage (`/api/dashboard`) ab.
* Die Uhrzeit („Stand hh:mm:ss“) kommt per NTP. Ohne Internet steht dort
  die Laufzeit seit dem Start (`+hh:mm:ss`).
* Arbeitsspeicher: 520 KB. Jede TLS-Verbindung (Bambu-MQTT, Kamera, FTPS)
  braucht ~25 KB. Mit ca. 4–6 Bambu-Druckern plus Kamera ist noch Luft (geschätzt, am echten Board nicht gemessen);
  der freie Speicher steht unter *Einstellungen → System*.

## 8. Aus dem Quellcode bauen

Voraussetzung: [arduino-cli](https://arduino.github.io/arduino-cli/) mit
dem Core **arduino-pico ≥ 6.2** (Board-Manager-URL
`https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`).

```bash
sh tools/build.sh        # -> build/DruckerDashboardRP2350-MK1.ino.uf2 und .bin
```

Das Skript erzeugt zuerst `web_index.h` aus `web/index.html`
(gzip-komprimiert) und baut mit: Board *Raspberry Pi Pico 2*,
*FreeRTOS SMP*, *4MB (Sketch: 3MB, FS: 1MB)*, *IPv4 Only* mit erhöhtem
lwIP-Speicher (`__LWIP_MEMMULT=3`). Die Bibliotheken (mbedTLS 3.6.6 mit
eigener Konfiguration, ArduinoJson 7.4) liegen in `firmware/libraries/`.

**Arduino-IDE:** Inhalt von `firmware/libraries/` in den eigenen
`Arduino/libraries`-Ordner kopieren, `firmware/DruckerDashboardRP2350-MK1`
öffnen, obige Board-Optionen wählen, als IP-Stack *„IPv4 Only - 32K“*.
Vor jedem Build bei geänderter Weboberfläche `python3 tools/gen_web.py`.

**GitHub Actions:** `.github/workflows/build-uf2.yml` baut bei jedem Push
UF2 + BIN und veröffentlicht auf `main` ein Release (Version aus
`APP_VERSION` in `config.h`).

## 9. Aufbau (für die Weiterarbeit)

```
firmware/DruckerDashboardRP2350-MK1/
  DruckerDashboardRP2350-MK1.ino   Start (eigene Aufgabe, 24 KB Stack): LittleFS, W5500, DHCP, mDNS, NTP, LED, Watchdog
  plat.h / plat_arduino.cpp        Plattformschicht (FreeRTOS-Tasks, lwIP-TCP, LittleFS, OTA)
  tls.*                            mbedTLS-Client (TLS 1.2/1.3, Sitzungs-Wiederverwendung)
  httpd.* / api.*                  HTTP-Server + alle REST-Routen (Portierung der Flask-Routen)
  httpc.* / mqtt.*                 HTTP- und MQTT-Client
  bambu.*                          Bambu: MQTT-Status, FTPS-Upload, project_file
  pollers.*                        Formlabs, OctoPrint, Creality, Ultimaker (+ Kopplung/Upload)
  extras.*                         zweiter MQTT-Broker (Sensoren/Schalter)
  camera.*                         A1-MJPEG-Relay, RTSP(S)-Relay
  config.*                         config.json (gleiches Format wie MK6)
web/index.html                     Weboberfläche (aus MK6 abgeleitet) -> web_index.h
host/                              Linux-Testbuild derselben Logik (plat_posix.cpp)
test/mock_printers.py              simulierte Drucker/Kameras/Broker für den Host-Test
```

Die Logik ist plattformneutral; `host/` baut sie mit POSIX-Sockets für
Linux. So wurde diese Version gegen simulierte Drucker getestet
(`test/mock_printers.py`: Bambu MQTT/TLS, FTPS mit erzwungener
Sitzungs-Wiederverwendung, RTSPS mit Digest, A1-Kamera, OctoPrint,
Moonraker, Ultimaker mit Digest, PreFormServer, MQTT-Broker), inklusive
Browser-Test der Oberfläche und Prüfung mit Thread-/AddressSanitizer:

```bash
cd host && make
sudo python3 ../test/mock_printers.py &      # nutzt 127.0.0.2-127.0.0.10
./dashboard_host --port 18080 --data /tmp/dd
```

**Nicht** auf echter Hardware getestet werden konnten: das Board selbst
(W5500/lwIP unter FreeRTOS, Speicherverbrauch im Dauerbetrieb) und echte
Drucker. Die Netzwerk-/Protokoll-Logik ist dieselbe wie im Host-Test.
