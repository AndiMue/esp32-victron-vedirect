# Projektregeln – ESP32 Victron

## Projektziel
Entwicklung eines ESP32-basierten Systems zur Überwachung und Steuerung von Victron SmartShunt und SmartSolar MPPT 100/50.

Das System soll Messwerte erfassen, auf einem Display darstellen, Verbraucher abhängig vom Batteriezustand über Relais steuern und Fehler erkennen.

## Technische Grundlagen
- ESP32-WROOM-32
- PlatformIO
- Arduino Framework
- UART0 (`Serial`) ausschließlich für Debug-Ausgaben
- UART1 und UART2 für die beiden VE.Direct-Geräte
- VE.Direct: 19200 Baud, 8N1

## Dokumentation
Die technischen Referenzunterlagen liegen unter `docs/`:
- `docs/VeDirect-Doku/`: offizielle Victron-Dokumentation
- `docs/MPPT-Simulator/`: Simulator und Testunterlagen

Die vorhandenen Markdown-Dateien in `docs/` sind ungepflegt und möglicherweise veraltet. Sie sind keine verbindliche Projektspezifikation.

Maßgeblich sind das aktuelle Projektziel, der tatsächliche Quellcode und die offiziellen Victron-Unterlagen.

## Arbeitsregeln
- Vor Änderungen zunächst den vorhandenen Code untersuchen.
- Bestehende funktionierende Funktionen schützen.
- Keine Annahmen als Tatsachen darstellen.
- Unklarheiten ausdrücklich benennen und nachfragen.
- Änderungen klein und nachvollziehbar halten.
- Keine eigenmächtigen Architekturänderungen.
- Bei größeren Änderungen zuerst einen Vorschlag vorlegen und Zustimmung abwarten.
- Testergebnisse und technische Entscheidungen dokumentieren.
- Keine Änderungen an der Hardwarekonfiguration ohne Rücksprache.

## Aktueller Stand und nächste Schritte (Stand: 04.11.2026)

### Bestätigte Fakten (vom Projektinhaber)
- UART1: RX GPIO16 / TX GPIO17. UART2: RX GPIO26 / **TX GPIO33** (der Code ist maßgeblich; die Angabe GPIO27 in `ESP32_Pinbelegung.xlsx` ist veraltet).
- Pegel über Pegelwandler angepasst (MPPT sendet 5 V, SmartShunt 3,3 V). Derzeit **ohne galvanische Trennung**, gemeinsames Potential. Trennung evtl. später (dann neue Pegelwandler nötig).
- SmartShunt sendet PID **0xC035** (nicht in VE.Direct-Protokoll 3.34 gelistet), Firmware 419. SmartSolar MPPT 100/50 sendet PID 0xA057.
- Parser, FieldCatalog, Interpreter wurden in Commit `04a0b80` **bewusst entfernt** (Fehler in der seriellen Ausgabe, Ursache ungeklärt). Neuanfang ab Rohdatenausgabe. Der alte Code in Commit `0d5d648` dient nur als Referenz.
- StateBuilder-Klassen existieren lokal außerhalb des Repos; ob sie übernommen werden, ist offen.
- Ziel: Jedes Gerät soll an jedem UART funktionieren (Erkennung über PID, pro Kanal).

### Vermutete Ursachen der früheren Ausgabefehler
- Prüfsumme wurde nie geprüft; gestörte Blöcke wurden übernommen.
- Das binäre Prüfsummenbyte kann `\n`, `\r`, `\t` oder `:` sein und wurde als Steuerzeichen behandelt.
- Der H-Block des SmartShunts enthält keine PID und wurde einer alten PID zugeordnet.
- `---` (nicht synchronisiert) wurde per `atoi` zu 0 (z. B. SOC 0 %).
- Aktueller Code: zwei Tasks schreiben gleichzeitig Byte für Byte auf `Serial`.

### Bekannte Fehler (noch nicht behoben)
- `DeviceCatalog.cpp`: BMV-71x/SmartShunt/Orion-PIDs ohne „A“ (`0x389` statt `0xA389` usw.), `0xA059` ist „150/100 rev2“.
- Simulator sendet PID `0xA042` (= BlueSolar 75/15) statt `0xA057`. Laut Projektinhaber korrigiert (05.11.2026); die Datei im Repo unter `docs/MPPT-Simulator/` enthält aber noch `0xA042`.
- `test_Code/` ist ein abweichender, nicht kompilierbarer Entwurf (nicht Teil des Builds).

### Stufenplan
1. **(umgesetzt und getestet 05.11.2026)** Neuer `VeDirectTextParser` in `lib/Victron`:
   - Testergebnis: 12/12 Unit-Tests auf dem ESP32 bestanden (`pio test -e esp32dev`, COM17).
   - Hardwaretest: SmartShunt (PID 0xC035, FW 0419) an UART1, MPPT-Simulator (PID 0xA057) an UART2. Über ca. 25 s gab es 0 Prüfsummenfehler, 0 verworfene Blöcke und 0 Queue-Verluste. Die Ausgabe ist sauber nach Blöcken getrennt. Der Shunt liefert zwei Blöcke pro Sekunde (der H-Block ohne PID), der Simulator einen.
   - Prüfsumme (Modulo 256) über alle Bytes laufend mitrechnen.
   - Erstes Byte nach `Checksum\t` beendet den Block.
   - Block erst in temporären Frame, Übernahme nur bei gültiger Prüfsumme.
   - `:`-HEX-Zeilen bis `\n` überspringen, auch mitten im Frame.
   - Überlange Labels/Werte verwerfen den Block.
   - Je UART-Task eine Parser-Instanz; fertige Frames per FreeRTOS-Queue an **eine** Logger-Task, die allein auf `Serial` schreibt (Rohwerte je Block + Zähler: OK, Prüfsummenfehler, Alter des letzten Blocks).
   - Banner vor Taskstart, auskommentierte Altblöcke in `main.cpp` entfernen.
   - Reader, Pins und DeviceCatalog bleiben unverändert.
   - PC-Tests: `native`-Umgebung in `platformio.ini` (Unity), mit aussagekräftigen Kommentaren.
   - **Blocker:** Auf dem PC ist kein GCC installiert (geprüft 04.11.2026). Vor jeder Installation (z. B. MSYS2/MinGW-w64) Rücksprache halten.
2. Geräteerkennung pro Kanal: `Unbekannt` → `Erkannt` durch gültigen Block mit PID; Blöcke ohne PID erben die Kanalerkennung; Rücksetzen bei Timeout (> ca. 3 s) oder PID-Wechsel. DeviceCatalog-PIDs korrigieren, 0xC035 aufnehmen, Simulator-PID korrigieren.
3. Neuer FieldCatalog: Klarnamen, Einheiten, Skalierung; `---` = ungültig (nie 0); `ON`/`OFF` ohne Beachtung der Groß-/Kleinschreibung.
4. Gerätezustand (Battery/MPPT) mit Zeitstempel und Gültigkeit; dann Display, Relais, SOC-Logik.

### Code-Stil
- Aussagekräftige Kommentare im Code (Wunsch des Projektinhabers).
