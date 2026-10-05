# Host-Test

`mock_printers.py` simuliert alle Gegenstellen des Dashboards auf Loopback-
Adressen (Linux, als root wegen Ports < 1024):

| Adresse | Simuliert |
|---|---|
| 127.0.0.2 | Bambu X1C: MQTT/TLS 8883, FTPS 990 (verlangt TLS-Sitzungs-Wiederverwendung wie vsftpd), RTSPS 322 (Digest bblp/12345678) |
| 127.0.0.3 | Bambu A1: MQTT/TLS 8883 (trennt bei Abo des request-Topics wie die echte Firmware), FTPS 990 (TLS 1.3), Kamera 6000 |
| 127.0.0.4 | OctoPrint (API-Key `octokey`) |
| 127.0.0.5 | Moonraker |
| 127.0.0.6 | Ultimaker (Kopplung, Digest-Auth) |
| 127.0.0.7 | PreFormServer (Geraet 192.168.99.9) |
| 127.0.0.8 | RTSP-Kamera 8554 (Basic cam/geheim) |
| 127.0.0.9 | MQTT-Broker 1883 (werkstatt/temperatur, werkstatt/feuchte) |

Testdaten in `data/`: Zertifikat, H.264-Teststream, JPEG-Bilder, `Testteil.gcode.3mf`.
Access Code aller Bambu-Mocks: `12345678`.

## Kooperative Aufgaben (QEMU)

`test/coop_qemu/run.sh` prüft `coop.cpp` auf einem emulierten Cortex-M33
(qemu-system-arm, Maschine mps2-an505): Kontextwechsel inkl. FPU-Register,
Interrupts auf Aufgaben-Stacks, Mutex, Aufräumen und Stack-Überlauf-Erkennung.
