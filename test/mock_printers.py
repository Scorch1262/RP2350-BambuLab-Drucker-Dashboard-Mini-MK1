#!/usr/bin/env python3
"""
Simulierte Drucker fuer den Host-Test des Drucker-Dashboards (RP2350-Port).

Startet auf Loopback-Adressen (Linux, als root wegen Ports < 1024):
  127.0.0.2  Bambu "X1C"  : MQTT/TLS 8883, FTPS 990 (verlangt TLS-Sitzungs-
                            wiederverwendung wie vsftpd), RTSPS 322 (Digest)
  127.0.0.3  Bambu "A1"   : MQTT/TLS 8883, FTPS 990 (ohne Wiederverwendung),
                            JPEG-Kamera 6000 (TLS)
  127.0.0.4  OctoPrint    : HTTP 80 (API-Key "octokey")
  127.0.0.5  Moonraker    : HTTP 7125
  127.0.0.6  Ultimaker    : HTTP 80 (Kopplung + Digest-Auth fuer /print_job)
  127.0.0.7  PreFormServer: HTTP 44388 (Geraet 192.168.99.9)
  127.0.0.8  RTSP-Kamera  : RTSP 8554 (Basic-Auth cam/geheim)
  127.0.0.9  MQTT-Broker  : 1883 ohne TLS (Sensoren/Schalter)
"""
import base64, glob, hashlib, json, os, random, socket, socketserver, ssl, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
ACCESS = "12345678"
LOG_LOCK = threading.Lock()
EVENTS = []          # fuer Testauswertung (GET http://127.0.0.10:8000/events)

def log(*a):
    msg = " ".join(str(x) for x in a)
    with LOG_LOCK:
        print("[MOCK]", msg, flush=True)
        EVENTS.append(msg)

def server_ctx(tls13=True):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(DATA, "cert.pem"), os.path.join(DATA, "key.pem"))
    if not tls13:
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    return ctx

class TServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

# ---------------------------------------------------------------------------
# Bambu: MQTT ueber TLS
# ---------------------------------------------------------------------------
class BambuState:
    def __init__(self, name, serial, ip, family):
        self.name, self.serial, self.ip, self.family = name, serial, ip, family
        self.state = "IDLE"; self.percent = 0; self.file = ""
        self.nozzle = 25.0; self.bed = 24.0
        self.prints = []
        self.lock = threading.Lock()

def mqtt_read_packet(sock):
    h = recvn(sock, 1)
    if not h: return None, None
    mult, ln = 1, 0
    while True:
        b = recvn(sock, 1)
        if not b: return None, None
        ln += (b[0] & 0x7F) * mult
        mult *= 128
        if not b[0] & 0x80: break
    body = recvn(sock, ln) if ln else b""
    return h[0], body

def recvn(sock, n):
    buf = b""
    while len(buf) < n:
        try:
            c = sock.recv(n - len(buf))
        except (ssl.SSLError, OSError):
            return None
        if not c: return None
        buf += c
    return buf

def mqtt_packet(t, body):
    out = bytes([t]); ln = len(body)
    while True:
        b = ln % 128; ln //= 128
        if ln: b |= 0x80
        out += bytes([b])
        if not ln: break
    return out + body

def mqtt_publish(topic, payload):
    t = topic.encode()
    return mqtt_packet(0x30, struct.pack(">H", len(t)) + t + payload)

def full_report(st):
    rep = {"print": {
        "command": "push_status", "gcode_state": st.state, "mc_percent": st.percent,
        "subtask_name": st.file, "gcode_file": "", "nozzle_temper": st.nozzle, "bed_temper": st.bed,
        "mc_remaining_time": max(0, 100 - st.percent),
        "ipcam": {"rtsp_url": "rtsps://%s/streaming/live/1" % st.ip if st.family != "a1" else "disable"},
        "ams": {"ams": [{"id": "0", "humidity": "4", "humidity_raw": "44", "temp": "24.5", "tray": [
            {"id": "0", "tray_type": "PLA", "tray_color": "FF0000FF", "remain": 80, "tag_uid": "0" * 16},
            {"id": "1", "tray_type": "PETG", "tray_color": "0000FFFF", "remain": 50},
            {"id": "2"},
            {"id": "3", "tray_type": "PLA-CF", "tray_color": "000000FF", "remain": 20}]}]},
        # Platzhalter, damit der Report so gross ist wie bei echten Druckern (>10 KB)
        "hms": [{"attr": 0, "code": i} for i in range(600)],
    }}
    if st.family != "a1":
        rep["print"]["chamber_temper"] = 31.0
    return json.dumps(rep).encode()

class MqttHandler(socketserver.BaseRequestHandler):
    state = None
    def handle(self):
        st = self.server.bstate
        try:
            sock = self.server.tlsctx.wrap_socket(self.request, server_side=True)
        except Exception as e:
            log(st.name, "MQTT TLS-Fehler", e); return
        t, body = mqtt_read_packet(sock)
        if t is None or t >> 4 != 1: return
        # CONNECT parsen
        p = 2 + 4 + 1 + 1 + 2
        flags = body[7]
        def rstr(i):
            l = struct.unpack(">H", body[i:i+2])[0]; return body[i+2:i+2+l].decode(), i + 2 + l
        cid, p = rstr(10)
        user = pw = ""
        if flags & 0x80: user, p = rstr(p)
        if flags & 0x40: pw, p = rstr(p)
        if user != "bblp" or pw != ACCESS:
            sock.sendall(bytes([0x20, 2, 0, 5])); log(st.name, "MQTT Anmeldung abgelehnt"); return
        sock.sendall(bytes([0x20, 2, 0, 0]))
        log(st.name, "MQTT verbunden", cid)
        wlock = threading.Lock()
        alive = [True]
        def ticker():
            while alive[0]:
                time.sleep(1.5)
                with st.lock:
                    st.nozzle = round(25 + random.random() * 2, 2)
                    if st.state == "RUNNING":
                        st.percent = min(100, st.percent + 5)
                        if st.percent >= 100: st.state = "FINISH"
                    inc = json.dumps({"print": {"nozzle_temper": st.nozzle, "mc_percent": st.percent,
                                                "gcode_state": st.state, "command": "push_status"}}).encode()
                try:
                    with wlock: sock.sendall(mqtt_publish("device/%s/report" % st.serial, inc))
                except Exception:
                    alive[0] = False
        threading.Thread(target=ticker, daemon=True).start()
        try:
            while True:
                t, body = mqtt_read_packet(sock)
                if t is None: break
                typ = t >> 4
                if typ == 8:  # SUBSCRIBE
                    pid = body[:2]
                    tl = struct.unpack(">H", body[2:4])[0]
                    topic = body[4:4+tl].decode()
                    log(st.name, "SUBSCRIBE", topic)
                    if st.family == "a1" and topic.endswith("/request"):
                        log(st.name, "A1 trennt bei request-Abo (wie echte Firmware)"); break
                    with wlock: sock.sendall(mqtt_packet(0x90, pid + b"\x00"))
                elif typ == 3:  # PUBLISH
                    tl = struct.unpack(">H", body[:2])[0]
                    topic = body[2:2+tl].decode()
                    payload = json.loads(body[2+tl:])
                    if "pushing" in payload:
                        with wlock: sock.sendall(mqtt_publish("device/%s/report" % st.serial, full_report(st)))
                    elif payload.get("print", {}).get("command") == "project_file":
                        log(st.name, "PROJECT_FILE", json.dumps(payload["print"]))
                        with st.lock:
                            st.prints.append(payload["print"]); st.state = "RUNNING"; st.percent = 0
                            st.file = payload["print"]["subtask_name"]
                elif typ == 12:  # PINGREQ
                    with wlock: sock.sendall(bytes([0xD0, 0]))
                elif typ == 14:
                    break
        finally:
            alive[0] = False
            log(st.name, "MQTT getrennt")

# ---------------------------------------------------------------------------
# FTPS (implizit, Port 990)
# ---------------------------------------------------------------------------
class FtpsHandler(socketserver.BaseRequestHandler):
    def handle(self):
        st = self.server.bstate
        ctx = self.server.tlsctx
        try:
            ctl = ctx.wrap_socket(self.request, server_side=True)
        except Exception as e:
            log(st.name, "FTPS TLS-Fehler", e); return
        f = ctl.makefile("rb")
        def send(s): ctl.sendall((s + "\r\n").encode())
        send("220 (vsFTPd 3.0.3)")
        pasv = None
        while True:
            line = f.readline()
            if not line: break
            cmd = line.decode().strip()
            verb = cmd.split(" ")[0].upper()
            arg = cmd[len(verb):].strip()
            if verb == "USER": send("331 Please specify the password.")
            elif verb == "PASS":
                if arg == ACCESS: send("230 Login successful.")
                else: send("530 Login incorrect."); break
            elif verb == "PBSZ": send("200 PBSZ set to 0.")
            elif verb == "PROT": send("200 PROT now Private.")
            elif verb == "TYPE": send("200 Switching to Binary mode.")
            elif verb == "PASV":
                pasv = socket.socket(); pasv.bind((st.ip, 0)); pasv.listen(1)
                port = pasv.getsockname()[1]
                send("227 Entering Passive Mode (192,168,77,1,%d,%d)." % (port // 256, port % 256))
            elif verb == "STOR":
                if "fail553" in arg:
                    send("553 Could not create file."); continue
                send("150 Ok to send data.")
                pasv.settimeout(10)
                d, _ = pasv.accept()
                try:
                    ds = ctx.wrap_socket(d, server_side=True)
                except Exception as e:
                    log(st.name, "FTPS Daten-TLS-Fehler", e); send("425 TLS fail"); continue
                if self.server.require_reuse and not ds.session_reused:
                    log(st.name, "FTPS: Datenverbindung OHNE Sitzungswiederverwendung abgelehnt")
                    ds.close(); send("522 SSL connection failed: session reuse required"); continue
                total = 0
                h = hashlib.sha256()
                while True:
                    try:
                        c = ds.recv(65536)
                    except (ssl.SSLError, OSError):
                        break
                    if not c: break
                    total += len(c); h.update(c)
                try: ds.close()
                except Exception: pass
                log(st.name, "FTPS STOR", arg, total, "Bytes", "sha256=" + h.hexdigest()[:16],
                    "reuse=" + str(ds.session_reused), ctl.version())
                send("226 Transfer complete.")
            elif verb == "QUIT":
                send("221 Goodbye."); break
            else:
                send("502 Command not implemented.")

# ---------------------------------------------------------------------------
# A1-Kamera (Port 6000, TLS, 80-Byte-Anmeldung, 16-Byte-Kopf + JPEG)
# ---------------------------------------------------------------------------
class A1CamHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            s = self.server.tlsctx.wrap_socket(self.request, server_side=True)
        except Exception as e:
            log("A1-Kamera TLS-Fehler", e); return
        auth = recvn(s, 80)
        if not auth or auth[16:20] != b"bblp" or auth[48:48+len(ACCESS)] != ACCESS.encode():
            log("A1-Kamera: falsche Anmeldung"); return
        frames = [open(p, "rb").read() for p in sorted(glob.glob(os.path.join(DATA, "frame_*.jpg")))]
        log("A1-Kamera: Stream gestartet")
        i = 0
        try:
            while True:
                jpg = frames[i % len(frames)]; i += 1
                s.sendall(struct.pack("<IIII", len(jpg), len(jpg), 1, 0) + jpg)
                time.sleep(0.2)
        except Exception:
            log("A1-Kamera: Stream beendet")

# ---------------------------------------------------------------------------
# RTSP(S)-Server mit H.264 aus data/test.h264
# ---------------------------------------------------------------------------
def load_h264():
    data = open(os.path.join(DATA, "test.h264"), "rb").read()
    nals, i = [], 0
    starts = []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0: break
        starts.append(j + 3); i = j + 3
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        nal = data[s:e]
        while nal.endswith(b"\x00"): nal = nal[:-1]
        nals.append(nal)
    aus, cur = [], []
    for n in nals:
        t = n[0] & 0x1f
        cur.append(n)
        if t in (1, 5):
            aus.append(cur); cur = []
    sps = next(n for n in nals if n[0] & 0x1f == 7)
    pps = next(n for n in nals if n[0] & 0x1f == 8)
    return aus, sps, pps

AUS, SPS, PPS = load_h264()

class RtspHandler(socketserver.BaseRequestHandler):
    def handle(self):
        srv = self.server
        sock = self.request
        if srv.tlsctx:
            try:
                sock = srv.tlsctx.wrap_socket(sock, server_side=True)
            except Exception as e:
                log(srv.label, "RTSPS TLS-Fehler", e); return
        f = sock.makefile("rb")
        wlock = threading.Lock()
        nonce = "%016x" % random.getrandbits(64)
        session = "%08X" % random.getrandbits(32)
        playing = [False]
        def reply(code, reason, cseq, extra="", body=b""):
            h = "RTSP/1.0 %d %s\r\nCSeq: %s\r\n%s" % (code, reason, cseq, extra)
            if body: h += "Content-Length: %d\r\n" % len(body)
            with wlock: sock.sendall(h.encode() + b"\r\n" + body)
        def authorized(hdrs, method):
            a = hdrs.get("authorization", "")
            if srv.auth == "basic":
                return a == "Basic " + base64.b64encode((srv.user + ":" + srv.pw).encode()).decode()
            if not a.startswith("Digest "): return False
            parts = dict((k.strip(), v.strip().strip('"')) for k, v in
                         (x.split("=", 1) for x in a[7:].split(",") if "=" in x))
            ha1 = hashlib.md5(("%s:%s:%s" % (srv.user, "bambu", srv.pw)).encode()).hexdigest()
            ha2 = hashlib.md5(("%s:%s" % (method, parts.get("uri", ""))).encode()).hexdigest()
            exp = hashlib.md5(("%s:%s:%s" % (ha1, nonce, ha2)).encode()).hexdigest()
            return parts.get("response") == exp and parts.get("username") == srv.user
        while True:
            line = f.readline()
            if not line: break
            req = line.decode().strip()
            if not req: continue
            hdrs = {}
            while True:
                l = f.readline().decode().strip()
                if not l: break
                k, v = l.split(":", 1); hdrs[k.strip().lower()] = v.strip()
            method, url = req.split(" ")[0], req.split(" ")[1]
            cseq = hdrs.get("cseq", "0")
            if method == "OPTIONS":
                reply(200, "OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"); continue
            if not authorized(hdrs, method):
                ch = ('WWW-Authenticate: Basic realm="cam"\r\n' if srv.auth == "basic"
                      else 'WWW-Authenticate: Digest realm="bambu", nonce="%s"\r\n' % nonce)
                reply(401, "Unauthorized", cseq, ch); log(srv.label, "RTSP 401 fuer", method); continue
            if method == "DESCRIBE":
                sdp = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Mock\r\nt=0 0\r\na=control:*\r\n"
                       "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                       "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=%s,%s\r\n"
                       "a=control:trackID=1\r\n" % (base64.b64encode(SPS).decode(), base64.b64encode(PPS).decode()))
                reply(200, "OK", cseq, "Content-Type: application/sdp\r\nContent-Base: %s/\r\n" % url.rstrip("/"), sdp.encode())
            elif method == "SETUP":
                reply(200, "OK", cseq, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nSession: %s;timeout=60\r\n" % session)
            elif method == "PLAY":
                reply(200, "OK", cseq, "Session: %s\r\n" % session)
                if not playing[0]:
                    playing[0] = True
                    log(srv.label, "RTSP PLAY")
                    threading.Thread(target=self.stream, args=(sock, wlock, playing), daemon=True).start()
            elif method == "TEARDOWN":
                reply(200, "OK", cseq); break
            else:
                reply(405, "Method Not Allowed", cseq)
        playing[0] = False

    def stream(self, sock, wlock, playing):
        seq = random.randint(0, 60000); ts = random.randint(0, 2**31); ssrc = 0x1234
        idx = 0
        try:
            while playing[0]:
                au = AUS[idx % len(AUS)]; idx += 1
                pkts = []
                for n in au:
                    if len(n) <= 1400:
                        pkts.append(n)
                    else:
                        hdr = n[0]; payload = n[1:]; first = True
                        while payload:
                            chunk = payload[:1400]; payload = payload[1400:]
                            fu_ind = (hdr & 0xe0) | 28
                            fu_hdr = (hdr & 0x1f) | (0x80 if first else 0) | (0x40 if not payload else 0)
                            pkts.append(bytes([fu_ind, fu_hdr]) + chunk); first = False
                for k, pl in enumerate(pkts):
                    marker = 0x80 if k == len(pkts) - 1 else 0
                    rtp = struct.pack(">BBHII", 0x80, marker | 96, seq & 0xffff, ts & 0xffffffff, ssrc) + pl
                    seq += 1
                    with wlock: sock.sendall(b"$" + struct.pack(">BH", 0, len(rtp)) + rtp)
                ts += 6000
                time.sleep(1 / 15)
        except Exception:
            pass
        log(self.server.label, "RTSP-Stream beendet")

# ---------------------------------------------------------------------------
# HTTP-Mocks
# ---------------------------------------------------------------------------
UM = {"auth_checks": 0, "busy": False, "nonce": "abc123nonce"}

class HttpMock(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send_json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""
    def do_GET(self):
        kind = self.server.kind; p = self.path
        if kind == "octo":
            if self.headers.get("X-Api-Key") != "octokey": return self.send_json({"error": "Invalid API key"}, 403)
            if p == "/api/printer":
                return self.send_json({"state": {"text": "Printing"}, "temperature": {"tool0": {"actual": 214.8}, "bed": {"actual": 60.1}}})
            if p == "/api/job":
                return self.send_json({"job": {"file": {"name": "benchy.gcode"}}, "progress": {"completion": 42.123, "printTimeLeft": 1830}})
        if kind == "moon":
            if p == "/printer/objects/list":
                return self.send_json({"result": {"objects": ["print_stats", "extruder", "heater_bed", "temperature_sensor chamber_temp"]}})
            if p.startswith("/printer/objects/query"):
                if "temperature_sensor%20chamber_temp" not in p: log("Moonraker: Kammer-Objekt fehlt in Query", p)
                return self.send_json({"result": {"status": {
                    "print_stats": {"state": "printing", "filename": "k1_teil.gcode"},
                    "display_status": {"progress": 0.5567}, "extruder": {"temperature": 220.04},
                    "heater_bed": {"temperature": 65.0}, "temperature_sensor chamber_temp": {"temperature": 38.26}}}})
        if kind == "um":
            if p == "/api/v1/printer/status": return self.send_json("printing" if UM["busy"] else "idle")
            if p == "/api/v1/print_job":
                if UM["busy"]:
                    return self.send_json({"name": "um_teil", "progress": 0.25, "time_elapsed": 600, "time_total": 3000})
                return self.send_json({"message": "Not found"}, 404)
            if p == "/api/v1/printer/bed/temperature": return self.send_json({"current": 59.94, "target": 60})
            if p.startswith("/api/v1/printer/heads/0/extruders/0/hotend/temperature"): return self.send_json({"current": 199.5, "target": 200})
            if p.startswith("/api/v1/auth/check/"):
                UM["auth_checks"] += 1
                return self.send_json({"message": "authorized" if UM["auth_checks"] >= 2 else "unknown"})
        if kind == "preform":
            if p == "/devices/":
                return self.send_json({"devices": [{"id": "Form4-ABC", "ip_address": "192.168.99.9", "is_connected": True}]})
            if p == "/devices/Form4-ABC/":
                return self.send_json({"id": "Form4-ABC", "ip_address": "192.168.99.9", "is_connected": True,
                                       "printer_status": {"status": "PRINTING", "current_print_run": {
                                           "job_name": "zahnrad", "progress_percentage": 0.37}},
                                       "cartridge_status": {"material_name": "Grey V5"}})
        self.send_json({"error": "not found", "path": p}, 404)
    def do_POST(self):
        kind = self.server.kind; p = self.path
        if kind == "um":
            if p == "/api/v1/auth/request":
                self.body(); return self.send_json({"id": "umid123", "key": "umkey456"})
            if p == "/api/v1/print_job":
                auth = self.headers.get("Authorization", "")
                if not auth.startswith("Digest "):
                    self.body()
                    self.send_response(401)
                    self.send_header("WWW-Authenticate", 'Digest realm="Jedi-API", nonce="%s", qop="auth", algorithm="MD5"' % UM["nonce"])
                    self.send_header("Content-Length", "0"); self.end_headers(); return
                parts = dict((k.strip(), v.strip().strip('"')) for k, v in (x.split("=", 1) for x in auth[7:].split(",") if "=" in x))
                ha1 = hashlib.md5(b"umid123:Jedi-API:umkey456").hexdigest()
                ha2 = hashlib.md5(("POST:" + parts["uri"]).encode()).hexdigest()
                exp = hashlib.md5(("%s:%s:%s:%s:%s:%s" % (ha1, UM["nonce"], parts["nc"], parts["cnonce"], parts["qop"], ha2)).encode()).hexdigest()
                body = self.body()
                if parts.get("response") != exp:
                    log("Ultimaker: Digest falsch"); return self.send_json({"message": "unauthorized"}, 401)
                ok = b'name="jobname"' in body and b'name="file"' in body
                log("Ultimaker: print_job empfangen", len(body), "Bytes, multipart ok =", ok)
                UM["busy"] = True
                return self.send_json({"message": "ok"}, 201)
        if kind == "preform" and p == "/discover-devices/":
            log("PreForm: discover", self.body()); return self.send_json({"count": 1})
        if kind == "events":
            EVENTS.clear(); return self.send_json({"ok": True})
        self.body(); self.send_json({"error": "not found"}, 404)

class EventsHandler(HttpMock):
    def do_GET(self):
        with LOG_LOCK: self.send_json(list(EVENTS))

# ---------------------------------------------------------------------------
# einfacher MQTT-Broker (ohne TLS) fuer Sensoren/Schalter
# ---------------------------------------------------------------------------
class BrokerHandler(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        t, body = mqtt_read_packet(s)
        if t is None: return
        s.sendall(bytes([0x20, 2, 0, 0]))
        alive = [True]
        def pub():
            v = 20.0
            while alive[0]:
                v += random.random() - 0.5
                try:
                    s.sendall(mqtt_publish("werkstatt/temperatur", ("%.1f" % v).encode()))
                    s.sendall(mqtt_publish("werkstatt/feuchte", b"48"))
                except Exception:
                    break
                time.sleep(1)
        threading.Thread(target=pub, daemon=True).start()
        while True:
            t, body = mqtt_read_packet(s)
            if t is None: break
            if t >> 4 == 8: s.sendall(mqtt_packet(0x90, body[:2] + b"\x00"))
            elif t >> 4 == 3:
                tl = struct.unpack(">H", body[:2])[0]
                log("BROKER PUBLISH", body[2:2+tl].decode(), body[2+tl:].decode())
            elif t >> 4 == 12: s.sendall(bytes([0xD0, 0]))
        alive[0] = False

def start_tcp(ip, port, handler, **attrs):
    srv = TServer((ip, port), handler)
    for k, v in attrs.items(): setattr(srv, k, v)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv

def start_http(ip, port, kind, handler=HttpMock):
    srv = ThreadingHTTPServer((ip, port), handler); srv.kind = kind
    threading.Thread(target=srv.serve_forever, daemon=True).start()

def main():
    x1 = BambuState("X1C-Mock", "00M00A000000001", "127.0.0.2", "x1")
    a1 = BambuState("A1-Mock", "03919A000000002", "127.0.0.3", "a1")
    ctx = server_ctx(True); ctx12 = server_ctx(False)
    start_tcp("127.0.0.2", 8883, MqttHandler, bstate=x1, tlsctx=ctx12)
    start_tcp("127.0.0.3", 8883, MqttHandler, bstate=a1, tlsctx=ctx12)
    start_tcp("127.0.0.2", 990, FtpsHandler, bstate=x1, tlsctx=ctx12, require_reuse=True)
    start_tcp("127.0.0.3", 990, FtpsHandler, bstate=a1, tlsctx=ctx, require_reuse=False)
    start_tcp("127.0.0.3", 6000, A1CamHandler, tlsctx=ctx12)
    start_tcp("127.0.0.2", 322, RtspHandler, tlsctx=ctx12, auth="digest", user="bblp", pw=ACCESS, label="X1-RTSPS")
    start_tcp("127.0.0.8", 8554, RtspHandler, tlsctx=None, auth="basic", user="cam", pw="geheim", label="Ext-RTSP")
    start_tcp("127.0.0.9", 1883, BrokerHandler)
    start_http("127.0.0.4", 80, "octo")
    start_http("127.0.0.5", 7125, "moon")
    start_http("127.0.0.6", 80, "um")
    start_http("127.0.0.7", 44388, "preform")
    start_http("127.0.0.10", 8000, "events", EventsHandler)
    log("Alle Mocks laufen.")
    while True: time.sleep(3600)

if __name__ == "__main__":
    main()
