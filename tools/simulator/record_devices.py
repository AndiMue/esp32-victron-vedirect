"""
Aufzeichnung der echten VE.Direct-Geräte als Grundlage für die Simulatoren.

Idee (Projektinhaber 12.11.2026): Text-Blöcke von SmartShunt und MPPT
aufzeichnen (zum späteren "Abspielen" im Simulator) und alle HEX-Antworten
mitschneiden, die der Simulator dann auf Anfrage des ESP32 senden kann.

Die Daten kommen über den seriellen Monitor des Steuergeräts (USB). Benutzt
werden nur LESENDE Befehle:
    bloecke ein/aus, hex async ein/aus, hex ping|version|pid, hex get <reg>

Aufruf (PlatformIO-Python, serieller Monitor muss geschlossen sein):
    python tools/simulator/record_devices.py COM17 180
        COM17 = Port des ESP32, 180 = Sekunden Text-Aufzeichnung

Ergebnis: tools/simulator/aufzeichnung_JJJJ-MM-TT_hhmm.json

DTR/RTS werden vor dem Öffnen abgeschaltet, damit der ESP32 nicht
zurückgesetzt wird (sonst fallen die Relais kurz ab).
"""

import datetime
import json
import os
import re
import sys
import time

import serial

# Register, die je Gerätetyp abgefragt werden (Einstellungen wie im Gerät,
# dazu Live- und Historienwerte, damit der Simulator mehr beantworten kann)
SHUNT_REGS = [
    0x0100, 0x0101, 0x010A, 0x010B, 0x0120,                     # Produkt
    0x1000, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006,     # Einstellungen
    0x1007, 0x1008, 0x1009,
    0xED8D, 0xED7D, 0xED8F, 0xED8C, 0xED8E, 0xEEFF, 0x0FFF,     # Live
    0x0FFE, 0xEDEC, 0xEEB6,
    0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306,     # Historie
    0x0307, 0x0308, 0x0309, 0x030A, 0x030B, 0x0310, 0x0311,
    0xEEF9, 0xEEB8,                                             # Version, DC-Monitor-Modus
]
MPPT_REGS = [
    0x0100, 0x0101, 0x010A, 0x010B, 0x0120, 0x0140,             # Produkt, Fähigkeiten
    0xEDEF, 0xEDEA, 0xEDF7, 0xEDF6, 0xEDF0, 0xEDF1, 0xEDF2,     # Einstellungen
    0xEDAB, 0xEDFB, 0xEDFC, 0xEDE8,
    0x0200, 0x0201, 0x0202, 0xEDD5, 0xEDD7, 0xED8D, 0xED8F,     # Live
    0xEDBC, 0xEDBB, 0xEDBD, 0xEDD3, 0xEDD2, 0xEDD1, 0xEDD0,
    0xEDDA, 0xEDD4, 0xEDDB, 0xEDDF, 0xEDDD, 0xEDDC,
    0x104F] + list(range(0x1050, 0x106F))                       # Historie gesamt + 31 Tage

LINE = re.compile(r"^\[(UART\d)\]\s+([\d.]+)s\s+(.*)$")
GET = re.compile(r"HEX <- (Get|Async \(ungefragt\)) 0x([0-9A-F]{4}) Flags ([0-9A-F]{2}) Wert((?: [0-9A-F]{2})*)")

# Seriennummern nicht in die (evtl. veröffentlichte) Aufzeichnung übernehmen
ANONYMOUS_SERIAL = "HQ00000TEST"
FIELD = re.compile(r"^\s{4}.*?\s(\S+) = (.*)$")


def hex_line(code, data):
    """Fertige HEX-Zeile wie vom Gerät gesendet (inkl. Prüfsumme, ohne \\n)."""
    s = (code + sum(data)) & 0xFF
    check = (0x55 - s) & 0xFF
    return ":" + "%X" % code + "".join("%02X" % b for b in data) + "%02X" % check


class Monitor:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.1
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.rest = ""

    def lines(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            chunk = self.s.read(8192).decode("utf-8", "replace")
            if not chunk:
                continue
            self.rest += chunk
            *done, self.rest = self.rest.split("\n")
            for line in done:
                yield line.rstrip("\r")

    def cmd(self, text, seconds):
        self.s.write((text + "\n").encode())
        return list(self.lines(seconds))


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM17"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 180
    mon = Monitor(port)
    list(mon.lines(1))
    mon.cmd("hex async aus", 0.5)

    # ---- 1. Text-Blöcke ----
    print(f"Text-Blöcke {seconds:.0f} s aufzeichnen ...")
    frames = {"UART1": [], "UART2": []}
    current = None
    mon.s.write(b"bloecke ein\n")
    for line in mon.lines(seconds):
        m = LINE.match(line)
        if m and "Block #" in m.group(3):
            current = {"t": float(m.group(2)), "header": m.group(3).split("  |")[0].strip(), "fields": []}
            frames[m.group(1)].append(current)
            continue
        f = FIELD.match(line)
        if f and current is not None:
            value = ANONYMOUS_SERIAL if f.group(1) == "SER#" else f.group(2)
            current["fields"].append([f.group(1), value])
            continue
        if not line.startswith("    "):
            current = None
    mon.cmd("bloecke aus", 1)

    # Gerätezuordnung über die PID im Textblock
    def pid_of(blocks):
        for b in blocks:
            for label, value in b["fields"]:
                if label == "PID":
                    return value
        return None

    channels = {}
    for uart in ("UART1", "UART2"):
        pid = pid_of(frames[uart])
        # SmartShunt: PID 0xA389..0xA38B bzw. 0xC0xx; alles andere hier = MPPT
        up = (pid or "").upper()
        kind = "shunt" if up.startswith("0XA38") or up.startswith("0XC0") else "mppt"
        channels[uart] = {"pid": pid, "kind": kind}
        print(f"  {uart}: PID {pid} -> {kind}, {len(frames[uart])} Blöcke")

    # ---- 2. HEX-Antworten ----
    result = {
        "format": "vedirect-recording",
        "version": 1,
        "created": datetime.datetime.now().isoformat(timespec="seconds"),
        "note": "Aufnahme über den seriellen Monitor des Steuergeräts. Textblöcke: Felder ohne "
                "Checksum (der Simulator berechnet sie neu). HEX: fertige Antwortzeilen inkl. "
                "Prüfsumme (ohne abschließendes \\n).",
        "devices": {},
    }
    for uart, info in channels.items():
        n = uart[-1]
        regs = SHUNT_REGS if info["kind"] == "shunt" else MPPT_REGS
        dev = {"uart": uart, "pid": info["pid"], "textFrames": frames[uart],
               "hex": {"ping": None, "appVersion": None, "productId": None, "registers": {}},
               "async": []}
        print(f"  {uart}: HEX-Antworten ({len(regs)} Register) ...")
        for name, cmd, code in (("ping", "ping", 5), ("appVersion", "version", 1), ("productId", "pid", 1)):
            for line in mon.cmd(f"hex {cmd} {n}", 0.6):
                m = re.search(r"HEX <- (?:Ping: Firmware ([0-9A-F.]+)|Antwort: 0x([0-9A-F]+))", line)
                if m and uart in line:
                    if m.group(1):   # Ping: Version wieder in Rohform 0x4xyy
                        major, minor = m.group(1).split(".")
                        raw = 0x4000 | (int(major, 16) << 8) | int(minor, 16)
                    else:
                        raw = int(m.group(2), 16)
                    dev["hex"][name] = {"raw": "0x%04X" % raw,
                                        "line": hex_line(code, [raw & 0xFF, raw >> 8])}
        mon.cmd(f"hex async ein", 0.3)
        for reg in regs:
            for line in mon.cmd(f"hex get {reg:04x} {n}", 0.35):
                if uart not in line:
                    continue
                m = GET.search(line)
                if not m:
                    continue
                kind, rid, flags = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
                value = [int(x, 16) for x in m.group(4).split()]
                if rid == 0x010A and value:
                    # Seriennummer (Text) anonymisieren, Länge beibehalten,
                    # Rest wie beim Gerät mit 0 auffüllen (nullterminiert)
                    anon = list(ANONYMOUS_SERIAL.encode())[:len(value)]
                    value = anon + [0] * (len(value) - len(anon))
                data = [rid & 0xFF, rid >> 8, flags] + value
                if kind == "Get":
                    dev["hex"]["registers"]["0x%04X" % rid] = {
                        "flags": "0x%02X" % flags,
                        "value": " ".join("%02X" % v for v in value),
                        "line": hex_line(7, data)}
                else:
                    dev["async"].append({"t": float(LINE.match(line).group(2)) if LINE.match(line) else None,
                                         "register": "0x%04X" % rid, "line": hex_line(0xA, data)})
        mon.cmd("hex async aus", 0.3)
        answered = sum(1 for r in dev["hex"]["registers"].values() if r["flags"] == "0x00")
        print(f"    {len(dev['hex']['registers'])} Antworten, davon {answered} mit Wert, "
              f"{len(dev['async'])} ungefragte Meldungen")
        result["devices"][info["kind"]] = dev

    out_dir = os.path.dirname(os.path.abspath(__file__))
    name = "aufzeichnung_" + datetime.datetime.now().strftime("%Y-%m-%d_%H%M") + ".json"
    path = os.path.join(out_dir, name)
    with open(path, "w", encoding="utf-8") as f:
        # Kompakt: eine Zeile je Textblock bzw. Register (gut lesbar, kleine Datei)
        text = json.dumps(result, ensure_ascii=False, indent=1)
        text = re.sub(r'\[\n\s+"([^"\n]*)",\n\s+"([^"\n]*)"\n\s+\]', r'["\1", "\2"]', text)
        f.write(text)
    print("Gespeichert:", path)


if __name__ == "__main__":
    main()
