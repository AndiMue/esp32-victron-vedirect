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

## Aktueller Stand und nächste Schritte (Stand: 05.11.2026)

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

### Bekannte Fehler / Hinweise
- Behoben 05.11.2026: PIDs im `DeviceCatalog.cpp` korrigiert (`0xA381`…, `0xA389`…, `0xA3F0`…), `0xC035` aufgenommen, `0xA059` = „150/100 rev2“, PID-Vergleich ohne Beachtung der Groß-/Kleinschreibung.
- Simulator: PID und IL-Kommentar laut Projektinhaber korrigiert (05.11.2026). Im Simulator sind `OR = 0x00000001` („Keine Eingangsleistung“) und gleichzeitig `PPV > 0`/`CS = 3` widersprüchlich – nur Testdaten, kein Codefehler.
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
   - PC-Tests: GCC 16.2.0 aus MSYS2 (`C:\msys64\ucrt64\bin`, im Benutzer-Path); `pio test -e native` läuft.
2. Geräteerkennung pro Kanal (**noch offen**, Reihenfolge mit Stufe 3 getauscht): `Unbekannt` → `Erkannt` durch gültigen Block mit PID; Blöcke ohne PID erben die Kanalerkennung; Rücksetzen bei Timeout (> ca. 3 s) oder PID-Wechsel. Bisher nur vorläufig in der Logger-Task gelöst (letzte PID je Kanal für den Gerätenamen im Blockkopf).
3. **(umgesetzt und getestet 05.11.2026)** Klarnamen und physikalische Werte:
   - Neu: `FieldCatalog` (alle Felder von BMV-71x/SmartShunt und SmartSolar laut Doku 3.34; deutsche Klarnamen, Einheiten, Codetabellen CS/ERR/MPPT/MON, Bitmasken AR/OR/CAP_BLE), `ValueConverter` (Rohtext → `MeasuredValue` mit Status), `ValueFormatter` (Text mit Dezimalkomma, exakt aus dem Ganzzahlwert).
   - Überarbeitet: `SemanticField`, `FieldDefinition`, `MeasuredValue`.
   - `---` → „nicht verfügbar“ (nie 0), TTG `-1` → „unendlich“, ungültiger Rohtext → „ungültiger Wert“, `ON`/`OFF` ohne Beachtung der Groß-/Kleinschreibung.
   - Ausgabe je Feld: `Klarname   umgerechneter Wert   Label = Rohwert`; Blockkopf mit Gerätename und PID.
   - Testergebnis: 26/26 Unit-Tests auf PC und ESP32 bestanden (12 Parser, 14 Umrechnung/Formatierung/DeviceCatalog). Hardwaretest mit SmartShunt (UART1) und Simulator (UART2): Werte plausibel, 0 Prüfsummenfehler.
4. Gerätezustand (Battery/MPPT) mit Zeitstempel und Gültigkeit; dann Display, Relais, SOC-Logik.

### Code-Stil
- Aussagekräftige Kommentare im Code (Wunsch des Projektinhabers).
