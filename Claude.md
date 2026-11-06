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
- Behoben 06.11.2026: **Null-Byte beim Einstecken** eines VE.Direct-Steckers. Ein 0x00 vor „PID“ machte das Label unsichtbar, und die Modulo-256-Prüfsumme erkennt 0x00 nicht. In einem Wert hätte es den Wert abgeschnitten („13400“ → „13“). Jetzt sind in Label und Wert nur noch druckbare ASCII-Zeichen (0x20–0x7E) erlaubt, andere Bytes verwerfen den Block. Gefunden im Praxistest (Log `Ausgabe_Klarnamen_v0.4.2_mitAusfallUART2.txt`); 4 neue Tests, 44/44 auf PC und ESP32.
- Behoben 06.11.2026: Statuszeile zeigte vereinzelt „letzter gültiger Block vor 4294967290 ms“. Die Zeit wurde vor dem Kopieren des Status gelesen, die vorzeichenlose Differenz lief über.
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
2. **(umgesetzt und getestet 05.11.2026, nach Stufe 3)** Geräteerkennung pro Kanal (`DeviceDetector`):
   - `Unbekannt` → `Erkannt` durch gültigen Block mit PID. Eine unbekannte PID gilt als „unbekanntes Gerät (0x…)“; ihre Felder werden trotzdem über den FieldCatalog umgerechnet.
   - Blöcke ohne PID (H-Block) werden **nur einem Batteriemonitor** zugeordnet, sonst sind sie „nicht zuordenbar“ (Entscheidung des Projektinhabers).
   - Timeout **5 s** ohne Block mit PID → „Gerät verloren“. Bei PID-Wechsel sofort „Gerätewechsel“.
   - Diagnose für Wackelkontakte/Kabelbruch: Zähler für Ausfälle und Dauer des letzten Ausfalls; in der Statuszeile sichtbar.
   - Timeout steht in `include/AppConfig.hpp` (`g_config.deviceTimeoutMs`, `std::atomic`) und ist später per WebUI/MQTT änderbar. NVS-Speicherung kommt mit der WebUI.
   - Neu: `TextUtil.hpp` (gemeinsamer Vergleich ohne Groß-/Kleinschreibung).
   - Testergebnis: 40/40 Unit-Tests auf PC und ESP32 (davon 14 für den Detektor). Hardware: Shunt (UART1) und Simulator (UART2) sofort erkannt, H-Block korrekt zugeordnet, 0 Fehler.
   - Praxistest (Projektinhaber, 05.11.2026) erfolgreich: UARTs getauscht → Geräte richtig erkannt; Stecker gezogen → Ausfall erkannt; wieder eingesteckt → Wiedererkennung.
   - Hinweis: In der Bash-Shell von Claude Code läuft der MSYS2-GCC nur mit `C:\msys64\ucrt64\bin` am Anfang des PATH (Git-eigene DLLs stören). In PowerShell und im PlatformIO-Terminal tritt das nicht auf.
3. **(umgesetzt und getestet 05.11.2026)** Klarnamen und physikalische Werte:
   - Neu: `FieldCatalog` (alle Felder von BMV-71x/SmartShunt und SmartSolar laut Doku 3.34; deutsche Klarnamen, Einheiten, Codetabellen CS/ERR/MPPT/MON, Bitmasken AR/OR/CAP_BLE), `ValueConverter` (Rohtext → `MeasuredValue` mit Status), `ValueFormatter` (Text mit Dezimalkomma, exakt aus dem Ganzzahlwert).
   - Überarbeitet: `SemanticField`, `FieldDefinition`, `MeasuredValue`.
   - `---` → „nicht verfügbar“ (nie 0), TTG `-1` → „unendlich“, ungültiger Rohtext → „ungültiger Wert“, `ON`/`OFF` ohne Beachtung der Groß-/Kleinschreibung.
   - Ausgabe je Feld: `Klarname   umgerechneter Wert   Label = Rohwert`; Blockkopf mit Gerätename und PID.
   - Testergebnis: 26/26 Unit-Tests auf PC und ESP32 bestanden (12 Parser, 14 Umrechnung/Formatierung/DeviceCatalog). Hardwaretest mit SmartShunt (UART1) und Simulator (UART2): Werte plausibel, 0 Prüfsummenfehler.
4. Gerätezustand und Steuerung, aufgeteilt in kleine Schritte (siehe „Anforderungen Steuerung“):
   - **4a:** Gerätezustand (Batterie vom Shunt, PV vom MPPT) mit Gültigkeit und Zeitstempel je Wert, dazu das Fehlerwort. Noch ohne Relais. Entscheidungen des Projektinhabers (06.11.2026):
     - „Verbindung instabil“: Fehler innerhalb von **5 Minuten** (Prüfsummenfehler, verworfene Blöcke, Ausfälle). Die Warnung erlischt nach **2 Minuten** ohne Fehler. Dafür **Fehlerzähler** anlegen. Die Schwellen sollen konfigurierbar sein.
     - **Zwei Batteriemonitore gleichzeitig** kommen eigentlich nicht vor. Falls doch: keine Steuerung und keine Anzeige möglich, Behandlung wie bei Ausfall des Shunts (kritisch).
     - **Nach Ausfall eines Geräts ungültige Werte anzeigen** (z. B. `--`), keine veralteten letzten Werte. Für die Steuerung sind sie ohnehin gesperrt.
     - Zeitdifferenzen immer erst NACH dem Kopieren des Zustands mit `millis()` bilden (sonst Überlauf, siehe Fehler 06.11.2026).
   - **4b:** Abschaltlogik: Stufen, Hysterese, Spannungs-Rückfallebene, Verhalten bei Fehlern. Auf dem PC testbar.
   - **4c:** Relais, Bedienung (Touch/Taster) und Display.
   - Offen: Sollen die lokal vorhandenen StateBuilder-Klassen des Projektinhabers einbezogen werden?

## Anforderungen Steuerung (Stand: 05.11.2026, vom Projektinhaber)

### Einsatzfälle
- Dieselbe Firmware für zwei Anlagen. Der Unterschied liegt nur in der **Konfiguration** (später NVS + WebUI), nicht in getrenntem Code.
- **Balkonkraftwerk:** 1 Relais schaltet einen DC-DC-Wandler. Bei zu geringem Ladezustand wird er ausgeschaltet.
- **Camper:** gestufte Reaktion:
  1. Warnung im Display
  2. nicht zwingend benötigte Verbraucher abschalten
  3. zuletzt alle Verbraucher abschalten
- **Wechselrichter (Camper):** per Knopfdruck ein-/ausschaltbar, über ein eigenes Relais. Muss noch genauer spezifiziert werden.

### Geräte
- **SmartShunt ist Pflicht** für die Steuerung. **MPPT ist optional** (Add-on): Fehlt er, gibt es keine PV-Werte, das System läuft aber normal weiter (nur Hinweis).
- Welche Geräte erforderlich sind, soll konfigurierbar sein (wie der Geräte-Timeout in `AppConfig`).

### Fehlererkennung
- Ein **Fehlerwort (Bitmaske)**, in dem mehrere Fehler gleichzeitig hinterlegt werden können. Die Steuerung reagiert abhängig davon.
- Vorgeschlagene Fehler (Entwurf):
  - Shunt nicht vorhanden/verloren (kritisch)
  - Shunt nicht synchronisiert, SOC = `---` (kritisch für die SOC-Logik)
  - Shunt-Alarm `AR ≠ 0` (Warnung)
  - MPPT nicht vorhanden/verloren (Info)
  - MPPT-Fehler `ERR ≠ 0` (Warnung)
  - Verbindung instabil: Prüfsummenfehler oder Ausfälle häufen sich (Warnung)
- **Shunt fehlt:** konfigurierbar entweder manueller Betrieb mit deutlichem Hinweis **oder** komplett ausschalten.

### Abschaltlogik (Entwurf)
- Abschaltstufen nach SOC. Beispielwerte: < 40 % Warnung, < 30 % Stufe 1, < 20 % alles aus.
- **Hysterese** und Mindestzeit beim Wiedereinschalten (z. B. +5 %), damit die Relais nicht klappern.
- **Spannungsschwellen** als zweites Kriterium und als Rückfallebene, wenn der SOC ungültig ist.
- **SOC- und Spannungsschwellen müssen über die WebUI konfigurierbar sein**, auch pro Relais bzw. Abschaltstufe.
- Pro Relais konfigurierbar: Name, Abschaltstufe/Priorität, Verhalten bei Fehler (aus/halten/manuell), ob manuelles Schalten erlaubt ist.

### Sicherheit
- **Stürzt der ESP32 ab, startet er neu oder fehlt die Versorgung, müssen alle Relais in einen sicheren Zustand gehen.** Vermutlich heißt das: Steuerleitung offen, Verbraucher aus. Umsetzung über die Verdrahtung (Öffner/Schließer), einen definierten Pegel beim Booten und einen Watchdog.

### Bedienung und Display
- Display-Typ noch offen (OLED, E-Paper, TFT). **Aktueller Wunsch: Touch-Display.**
- Mit Touch-Display wird über Touch bedient.
- Ohne Touch-Display: Taster nötig. Die Taster sitzen **weit entfernt vom ESP32**. Denkbar ist deshalb ein eigenes Bediengerät mit mehreren Tastern, das über einen Bus mit dem ESP32 kommuniziert (Bus noch offen).

### Offene Fragen
- **Wie viele Relais werden tatsächlich gebraucht?** (Balkonkraftwerk: 1; Camper: noch offen)
- **Pinbelegung überdenken:** Im Excel sind Relais auf GPIO5 und GPIO15 vorgesehen. Das sind Boot-Strapping-Pins, die beim Start Pegel bzw. Signale ausgeben; GPIO14 gibt beim Booten ein PWM-Signal aus. Ein Relais darf beim Booten nicht kurz anziehen. Mit einem Touch-Display (SPI/I²C) ändert sich der Pinbedarf zusätzlich.
- Darf manuelles Einschalten (z. B. Wechselrichter) die Schutzabschaltung überstimmen (Notbetrieb)? Vorschlag: nein, Schutz geht vor Bedienung.
- Konkrete Schwellenwerte für SOC und Spannung sowie die Hysterese.
- Welcher Bus für ein abgesetztes Bediengerät (z. B. RS485, CAN, I²C über längere Leitungen ist kritisch)?

### Code-Stil
- Aussagekräftige Kommentare im Code (Wunsch des Projektinhabers).
