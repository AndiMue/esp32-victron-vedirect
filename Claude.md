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

### Zusammenarbeit (festgehalten 09.11.2026)
- **Commits macht ausschließlich der Projektinhaber.** Claude committet nie, auch nicht `Claude.md`.
- Kommunikation und Code-Kommentare auf **Deutsch**. Ausgaben auf Serial/Display: Display ohne Umlaute (ae/oe/ue), Monitor/WebUI mit Umlauten.
- Vorgehen je Stufe: Vorschlag mit offenen Fragen → Freigabe → Umsetzung → Tests (PC + ESP32) → Hardwaretest durch den Projektinhaber → Findings beheben → Eintrag in `Claude.md` → Commit durch den Projektinhaber.
- Findings des Projektinhabers werden mit Ursache und Abhilfe in `Claude.md` notiert.

### Bauen, Testen, Flashen (Stand 09.11.2026)
- **PlatformIO immer aus VS Code nutzen:** `%USERPROFILE%\.platformio\penv\Scripts\pio.exe` (6.2.0). `pio` im PATH (6.1.18, Python 3.13) baut GxEPD2 nicht.
- PC-Tests: `pio test -e native` (in PowerShell; in der Bash von Claude Code nur mit `C:\msys64\ucrt64\bin` vorne im PATH). ESP32-Tests: `pio test -e esp32dev` (überschreibt die Firmware! danach `pio run -t upload`).
- ESP32 hängt an **COM17** (CH9102). Vor dem Flashen prüfen, ob der Port frei ist (serieller Monitor des Projektinhabers belegt ihn sonst: „Zugriff verweigert“/„port doesn't exist“).
- Nach Änderung an `platformio.ini` liest VS Code das Projekt neu ein; ein gleichzeitiger Build kann mit „WinError 2 … .cpp.d“ scheitern → einfach erneut bauen.
- Nach Änderung der **Partitionierung**: Firmware per USB + `pio run -t uploadfs` (WebUI-Dateien neu). NVS (WLAN, Passwort) bleibt erhalten.
- **Updates im Normalfall per Browser** (WebUI → Update), Dateien aus `.pio/build/esp32dev/` (`package_*.tar`, `firmware_*.bin`, `webui_*.tar`). USB nur noch für Debug-Ausgaben und als Rettungsweg.
- Serieller Monitor: `pio device monitor -p COM17` (mit `monitor_echo`). Befehle: `r1 ein|aus`, `override ein|aus`, `bloecke ein|aus`, `hilfe`, Testfunktion `absturz` (später entfernen).

## Aktueller Stand und nächste Schritte (Stand: 05.11.2026)

### Bestätigte Fakten (vom Projektinhaber)
- UART1: RX GPIO16 / TX GPIO17. UART2: RX GPIO26 / **TX GPIO33** (der Code ist maßgeblich; die Angabe GPIO27 in `ESP32_Pinbelegung.xlsx` ist veraltet).
- Pegel über Pegelwandler angepasst (MPPT sendet 5 V, SmartShunt 3,3 V). Derzeit **ohne galvanische Trennung**, gemeinsames Potential. Trennung evtl. später (dann neue Pegelwandler nötig).
- SmartShunt sendet PID **0xC035** (nicht in VE.Direct-Protokoll 3.34 gelistet), Firmware 419 (seit 09.11.2026: 4.20). SmartSolar MPPT 100/50 sendet PID 0xA057.
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
   - **4a (umgesetzt und getestet 06.11.2026):** Gerätezustand (Batterie vom Shunt, PV vom MPPT) mit Gültigkeit und Zeitstempel je Wert, dazu das Fehlerwort. Noch ohne Relais.
     - Neu in `lib/System`: `DeviceState` (Werte je Kanal nach `SemanticField`), `LinkMonitor` (Verbindungsqualität), `SystemState`/`SystemEvaluator` (Rollen nach Gerätekategorie, Messwerte, Fehlerwort `SystemFault::*`, Schwere Info/Warnung/kritisch).
     - `main.cpp`: Mutex je Kanal, neue `systemTask` (1×/s, Core 0), Systemzeile `[SYSTEM]` alle 5 s, Statuszeile mit `Verbindung=stabil/INSTABIL`.
     - `AppConfig`: `requireBatteryMonitor` (true), `requirePvCharger` (false), `linkErrorThreshold` (3), `linkOutageThreshold` (1), `linkRecoveryMs` (120000).
     - Testergebnis: 68/68 Unit-Tests auf PC und ESP32 (24 neu). Hardware: Shunt an UART2, MPPT an UART1 → Rollen richtig, Fehlerwort 0x00000000. RAM 16 %.
     - „Batteriemonitor“ = Gerätekategorie im DeviceCatalog; im Projekt ist das der **SmartShunt** (BMV-7xx gehören ebenfalls dazu). Das Fehlerwort hat 32 Bit, 10 davon belegt.
     Entscheidungen des Projektinhabers (06.11.2026):
     - „Verbindung instabil“: **kein festes Zeitfenster**, stattdessen Zähler. Instabil ab **1 Ausfall** oder **3 Prüfsummenfehlern/verworfenen Blöcken**. Erholung nach **2 Minuten** ohne Fehler, aber nur bei verbundenem Gerät (Kabel ab → Warnung bleibt). Gesamtzähler laufen immer weiter. Erholzeit 0 = Warnung bleibt bis zum Neustart. Alles konfigurierbar.
     - **Fehlt der Shunt**, wird die Batteriespannung **vom MPPT** übernommen und gekennzeichnet (`ValueSource::PvCharger`, Anzeige „Spannung vom MPPT“). SOC und Strom kommen nie vom MPPT.
     - **Zwei Batteriemonitore gleichzeitig** kommen eigentlich nicht vor. Falls doch: keine Steuerung und keine Anzeige möglich, Behandlung wie bei Ausfall des Shunts (kritisch).
     - **Nach Ausfall eines Geräts ungültige Werte anzeigen** (z. B. `--`), keine veralteten letzten Werte. Für die Steuerung sind sie ohnehin gesperrt.
     - Zeitdifferenzen immer erst NACH dem Kopieren des Zustands mit `millis()` bilden (sonst Überlauf, siehe Fehler 06.11.2026).
   - **4b (umgesetzt und getestet 06.11.2026):** Abschaltlogik. Auf dem PC testbar. Läuft in der `systemTask` auf Basis von `SystemState`. **Die Relais werden noch nicht geschaltet** (GPIO kommt in 4c); `[RELAIS]` zeigt die Sollzustände.
     - Neu: `lib/System/LoadController` mit `LoadControllerConfig::camper()` / `::balcony()`. Je Relais drei SOC-Schwellen (gesperrt unter / aus unter / frei ab), Betriebsart manuell (R1, nur per Befehl) oder automatisch (R2/R3, Mindest-Auszeit 60 s), erlaubte Befehlsquellen (Local/WebUI/MQTT), `rememberRequest` (BKW), `requiresSoc` + `forceOffWithoutSoc` (R1).
     - **Bestätigt vom Projektinhaber (06.11.2026):** Fehlt der SOC (Shunt fehlt), wird ein **laufender** Wechselrichter **abgeschaltet** (nicht nur gesperrt), weil ohne SOC der Ladezustand unbekannt ist. Einstellbar über `forceOffWithoutSoc`.
     - Automatische Relais (R2/R3) nehmen keine Befehle an; Override schaltet sie ein.
     - `AppConfig::installation` (Camper/Balkonkraftwerk) wählt die Voreinstellung beim Start.
     - Befehle per Queue an die `systemTask` (sofort bearbeitet). Zum Testen im seriellen Monitor: `r1 ein`, `r1 aus`, `override ein`, `override aus`, `hilfe` (Quelle „Local“).
     - Testergebnis: 90/90 Unit-Tests auf PC und ESP32 (22 neu). Hardware: Befehle und Override per seriellem Monitor geprüft. Sperre/Abschaltung bei niedrigem SOC nur in Unit-Tests geprüft (Akku steht bei 100 %).
     - Vorgabe Projektinhaber (06.11.2026): Liegt die Batteriespannung nur vom MPPT vor (Shunt fehlt), darf das **Wechselrichter-Relais NICHT schalten**. Kleinverbraucher dürfen weiterlaufen.
     - Entscheidungen Projektinhaber (06.11.2026):
       - **Alle Schwellwerte konfigurierbar** (später WebUI/NVS).
       - **3 Relais:** R1 Hauptverbraucher (Camper: Wechselrichter, BKW: DC-DC-Wandler), R2 Licht, R3 Kühlschrank. Im BKW werden R2/R3 nicht genutzt.
       - Akku ist **LiFePO4** → möglichst keine Spannungsschwellen (flache Kennlinie); Steuerung über SOC.
       - Startwerte SOC: R1 Einschalten gesperrt unter **25 %**, wieder frei ab **30 %**. R2 aus unter **15 %**, frei ab **20 %**. R3 aus unter **5 %**, frei ab **10 %**.
       - R1: Ein **laufender** Wechselrichter wird beim Sperren nicht abgeschaltet. Abgeschaltet wird erst bei einer **eigenen, konfigurierbaren Abschaltschwelle** (Vorschlag Standard: 15 %, wie R2).
       - BKW: Der DC-DC wird **per MQTT aus der Hausautomatisierung** geschaltet. Entschieden: dasselbe R1 mit Befehlsquelle MQTT (+ WebUI) und Schutz, kein separates Relais.
       - **Nach Neustart:** Wechselrichter bleibt **aus**, R2 und R3 **an**.
       - **Override-Modus:** umgeht **alle** Sperren (SOC-Schwellen, Shunt fehlt, Wechselrichter-Sperre), setzt sich **nicht selbst zurück**, deutliche Anzeige im Display (z. B. Blitz/Ausrufezeichen). Entschieden: **Ein Neustart beendet den Override**, danach Normalbetrieb.
       - **Keine Spannungs-Notabschaltung** vorerst (LiFePO4). Spannungsschwellen entfallen zunächst ganz.
       - **R1-Befehlsquellen** je Anlage konfigurierbar (entschieden): Camper = Taster/Touch + WebUI, BKW = MQTT + WebUI. Schutz immer aktiv. Gemeinsame Befehlsschnittstelle für alle Quellen.
       - BKW, **Abbruch der MQTT-Verbindung** (entschieden): Zustand halten, der SOC-Schutz bleibt aktiv. Umsetzung kommt mit MQTT.
   - **4c-1 (umgesetzt 06.11.2026):** Relais werden geschaltet. Neu `src/RelayDriver.hpp`: aktiv Low (R1 GPIO25, R2 GPIO27, R3 GPIO32). `g_relays.begin()` ist die erste Aktion in `setup()` (Pegel „aus“ vorbelegen, dann Ausgang). Ausgabe nach jeder Auswertung und sofort nach jedem Befehl. **Task-Watchdog 10 s** (Panic → Neustart → Relais aus) für `systemTask` und beide UART-Tasks; der Logger wird nicht überwacht.
     - Test 06.11.2026: Start ohne Neustart/Brownout, `r1 ein`/`r1 aus` per seriellem Monitor ausgeführt. Rückmeldung des Projektinhabers zum hörbaren Schalten steht noch aus.
     - Beim Aufbau gefundene Verdrahtungsfehler (alle behoben): 5 V am Modul-VCC statt an JD-VCC (ESP32 lief nicht mehr), E-Paper VCC/GND vertauscht (Brownout-Schleife), GND-Leitung auf TXD (Flashen schlug fehl: „serial TX path seems to be down“).
   - **4c-2 (umgesetzt 06.11.2026):** E-Paper. Neu `src/StatusDisplay.hpp/.cpp`, eigene `displayTask` (nicht vom Watchdog überwacht). Bibliothek `zinggjm/GxEPD2 @ ^1.5.0` (Treiber `GxEPD2_154_D67`), nur in `env:esp32dev`.
     - Inhalt: SOC groß; Spannung (`*` = vom MPPT) und Strom; Leistung und PV-Leistung; 3 Relaisfelder (schwarz = EIN, durchgestrichen = deaktiviert); Statuszeile (Override > wichtigster Fehler > „OK“, schwarzer Balken bei Warnung/kritisch/Override). Texte ohne Umlaute (ae/oe/ue). Neu `RelayConfig::shortName` (WR/Licht/Kuehl, BKW: DC-DC).
     - Aktualisierung: Teilaktualisierung bei Änderung; reine Wertänderung höchstens alle 30 s, Relais/Status sofort; alle 30 Teilaktualisierungen ein Vollbild. Danach `powerOff()`.
     - SPI nur mit SCK 18 / MOSI 23 gestartet, damit GPIO19 (Reserve R4) frei bleibt. `init(0)`: keine Diagnoseausgabe auf Serial.
     - Test 06.11.2026: Firmware läuft stabil (RAM 17,9 %, Flash 24,8 %). **Anzeige selbst noch vom Projektinhaber zu bestätigen.**
   - **Hinweis Build-Umgebung:** Auf dem PC sind zwei PlatformIO-Versionen installiert. `pio` im PATH ist **6.1.18** (Python 3.13, `...\Python313\Scripts\pio.exe`) und scheitert beim Bauen von GxEPD2 („opening dependency file … No such file or directory“). **Immer die VS-Code-Version 6.2.0 verwenden:** `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.
   - Nach einer Änderung an `platformio.ini` liest die PlatformIO-Erweiterung in VS Code das Projekt neu ein und arbeitet dabei im selben `.pio`-Ordner. Ein gleichzeitig gestarteter Build scheitert dann mit „WinError 2 … .cpp.d“. Abhilfe: kurz warten, erneut bauen.
   - **E-Paper, Fehlersuche Schwarz/Grau (06.11.2026):**
     - Treiber `GxEPD2_154_GDEY0154D67` (Panel „FPC-B001-exp 24.08.02“), statt `GxEPD2_154_D67`.
     - **Kurzer Reset-Impuls** `init(0, true, 2, false)` (GxEPD2-README: Waveshare-Platinen mit „clever reset circuit“). Damit ist das **Vollbild zuverlässig deckend schwarz** (bestätigt). Vorher: mal deckend, mal grau.
     - **Teilaktualisierung** (`0x22 0xFC`, Kurve aus dem Panel-OTP) hellt unveränderte schwarze Pixel grau auf; das gilt für beide Treiber. Entscheidung Projektinhaber: **immer Vollbild** (`UsePartialRefresh = false`), Intervall bleibt **30 s** für Werte, Relais/Fehler/Override sofort. Ein schnellerer Verschleiß des Displays wird bewusst in Kauf genommen.
     - Offen: Ist ein anderes Display besser geeignet? (siehe Antwort vom 06.11.2026: z. B. kleines IPS-TFT ST7789 an denselben SPI-Pins)
   - **Serieller Monitor (06.11.2026):** Blockausgabe standardmäßig **aus**; sichtbar sind nur `[STATUS]`, `[SYSTEM]`, `[RELAIS]` (alle 5 s), Ereignisse und Befehle. Befehle `bloecke ein` / `bloecke aus` schalten die vollständige Blockausgabe um. `monitor_echo = yes` in `platformio.ini`: Eingaben sind beim Tippen sichtbar.
   - **Danach:** Bedienung (Touch/Taster) über das Bediengerät, WLAN/MQTT, WebUI.
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

### Architektur Anzeige/Bedienung (Entscheidung 06.11.2026)
- Das **Touch-Display sitzt weit entfernt** vom Steuergerät → eigenes **Bediengerät** (2. ESP32 + 3,6" TFT-Shield ST7793, 240×400, 8-Bit parallel, resistiver Touch), angebunden per **WLAN** (ESP-NOW oder WLAN/WebSocket, noch offen). Kein Bluetooth (ein Funkmodul, WLAN wird für MQTT gebraucht). Im Camper evtl. ohne Router → Steuergerät als Access Point (AP+STA).
- Grundregel: Die Steuerung bleibt im Steuergerät. Fällt die Funkverbindung aus, schaltet nichts um.
- Am Steuergerät selbst: **Waveshare 1.54inch e-Paper Module Rev2.1 (V2)**, schwarz-weiß 200×200, Teilaktualisierung möglich, BS = 0 (4-Draht-SPI), VCC 3,3/5 V. Nur die wichtigsten Werte. Bibliothek voraussichtlich GxEPD2 (Treiber noch prüfen).

### Pinbelegung Steuergerät (ESP32-WROOM-32), bestätigt 06.11.2026
| Funktion | GPIO |
|---|---|
| UART0 Debug/USB | 1 TX, 3 RX |
| UART1 VE.Direct | 16 RX, 17 TX (TX für späteres HEX-Protokoll) |
| UART2 VE.Direct | 26 RX, 33 TX (TX für späteres HEX-Protokoll) |
| Relais R1 (Wechselrichter/DC-DC) | 25 |
| Relais R2 (Licht) | 27 |
| Relais R3 (Kühlschrank) | 32 |
| Reserve Relais R4 | 19 |
| E-Paper SCK / DIN | 18 / 23 (VSPI) |
| E-Paper CS / DC / RST | 5 / 13 / 4 |
| E-Paper BUSY | 34 (reiner Eingang) |
| I²C (RTC PCF8523, 0x68, seit W4e) | 21 SDA, 22 SCL |
| Reset Btn | 35 | Pull high -> Btn schaltet auf Gnd
| Reserve Eingänge | 36, 39 (externer Pull-up nötig; 36/39 analog) |
| Nicht verwenden | 0, 2, 12, 14, 15 (Strapping/Bootsignale) |

- **Board:** „MINI32 V1.0.0“ (ESP32-WROOM-32, D1-mini-Format, CH9102-USB). Beschriftung nur auf der **Unterseite** (von oben gesehen links/rechts vertauscht). JTAG-Namen: **TCK = GPIO13**, TMS = 14, TDI = 12, TDO = 15; SVP = 36, SVN = 39. **SD0 („SDD“)/SD1/SD2/SD3/CLK/CMD = Flash, nie verwenden.** „VCC“ = 5 V vom USB.
- **Relaismodule** (Stand 06.11.2026): „2 Relay Module“ mit PC817-Optokoppler und JD-VCC-Jumper, **aktiv Low** (IN = Low → Relais an). Verdrahtung für sicheren Zustand: Jumper JD-VCC↔VCC **ab**, Modul-**VCC an 3,3 V**, **JD-VCC an 5 V**, **10 kΩ Pull-up von 3,3 V an jedes IN** (ersetzt den ursprünglich geplanten Pull-down). ESP32-Pin hochohmig (Boot/Reset/Absturz) → Relais aus. Firmware: aktiv Low, beim Start erst High setzen, dann auf Ausgang schalten; Watchdog.
- VE.Direct-TX: Der Pegelwandler sollte die Leitung beim Booten (TX hochohmig) definiert auf High halten (MPPT-RX kann Fernein/aus sein).
- `ESP32_Pinbelegung.xlsx` ist dadurch teilweise veraltet (UART2 TX, Relais-Pins, Display).

### Offene Fragen
- ~~Wie viele Relais werden tatsächlich gebraucht?~~ → 3 (R1–R3), Reserve R4 auf GPIO19.
- **Pinbelegung überdenken:** Im Excel sind Relais auf GPIO5 und GPIO15 vorgesehen. Das sind Boot-Strapping-Pins, die beim Start Pegel bzw. Signale ausgeben; GPIO14 gibt beim Booten ein PWM-Signal aus. Ein Relais darf beim Booten nicht kurz anziehen. Mit einem Touch-Display (SPI/I²C) ändert sich der Pinbedarf zusätzlich.
- Darf manuelles Einschalten (z. B. Wechselrichter) die Schutzabschaltung überstimmen (Notbetrieb)? Vorschlag: nein, Schutz geht vor Bedienung.
- Konkrete Schwellenwerte für SOC und Spannung sowie die Hysterese.
- Welcher Bus für ein abgesetztes Bediengerät (z. B. RS485, CAN, I²C über längere Leitungen ist kritisch)?

## WebUI (W-Stufen, ab 08.11.2026)

### Grundsätze (Projektinhaber 08.11.2026)
- WebUI wird **neu umgesetzt**; aus `ESP32_WebUI_AP_BASIS` nur sinnvolle Ideen übernehmen (TAR-Update der WebUI, kombiniertes Update, Build-Skripte, Partitionierung). Dort gefundene Probleme: Passwörter im HTML (`%PASS%`, `%MQTTPASS%`), offener AP, kein Login, Debug-Routen, doppelte Routen.
- Die WebUI (inkl. OTA, AP, WLAN, MQTT) soll **Basis für weitere Projekte** sein (z. B. TempSens auf ESP8266). Code liegt in `lib/WebCore`, ESP32/ESP8266-Unterschiede gekapselt. **ESP8266-Variante folgt in einem eigenen Schritt** (`#error` als Platzhalter).
- Sicherheit: Schutz gegen Angriffe übers Netz vollständig (WPA2-AP, Login, Passwörter verlassen nie das Gerät, `WiFi.persistent(false)`). Schutz gegen Auslesen des Flash nur mit ESP32-Flash-Verschlüsselung (unumkehrbar) → höchstens am Ende fürs fertige Gerät. ESP8266 hat dafür keine Hardware.
- MQTT: Mosquitto → Node-RED und Homematic OpenCCU (nicht im Camper, dort kein WLAN).
- Camper: dauerhafter AP muss möglich sein. Später: Shunt-Einstellungen per WebUI (braucht VE.Direct-HEX-Protokoll), Bediengerät evtl. per ESP-NOW.
- Gewünscht: Live-Werte-Seite (Inhalt noch festlegen), alle 3 Relais schalten, Override, Schwellen konfigurieren, Logdatei (später), **Statusmeldungen auch in der WebUI**, Sicherung/Wiederherstellung der Einstellungen, Datum/Uhrzeit (evtl. RTC, Abgleich per WLAN/NTP), mDNS.

### W1 (umgesetzt und getestet 08.11.2026)
- `lib/WebCore`: `SettingsStore` (NVS), `WifiManager` (AP beim Erststart; sonst STA, nach 20 s AP-Rückfall, Wiederholung alle 60 s; Captive Portal; mDNS `ve-direct.local`; Fehlergrund der Verbindung), `WebUi` (eingebauter `WebServer`, Basic Auth `admin` + Gerätepasswort, Sperre 60 s nach 5 Fehlversuchen, API `/api/status`, `/api/wifi`, `/api/wifi/scan`, `/api/wifi/reset`, `/api/password`, `/api/restart`).
- `data/`: `index.html`, `app.js`, `style.css` (Reiter Status/WLAN/Gerät, „Passwort anzeigen“, Rückmeldung nach dem Speichern). Hochladen: `pio run -t uploadfs`.
- **Gerätepasswort** (AP + Login): beim Erststart zufällig, 8 Zeichen ohne verwechselbare Zeichen; steht auf dem E-Paper und im Monitor, solange es nicht geändert wurde.
- **Reset-Taste IO35** (externer 10-kΩ-Pull-up nach 3,3 V, Taste nach GND): beim Einschalten 10 s halten, **dann loslassen** → WLAN + Gerätepasswort zurückgesetzt. Fehlersicher: dauerhaft Low (Pull-up fehlt, Taste klemmt) löst keinen Reset aus.
- Partitionierung `partitions_ota_littlefs.csv`: 2 × 1,5 MB App, 896 KB LittleFS, 64 KB Core Dump. Firmware ca. 935 KB, RAM 26 %.
- Test Projektinhaber: Weboberfläche auf PC und Handy, WLAN-Suche, Fehlermeldung bei falschem Passwort, Verbindung mit dem Heim-WLAN, `http://ve-direct.local`.
- Beim Aufbau gefunden: IO35 ohne Pull-up löste bei jedem Start den Reset aus; MPPT-Simulator mit eigener USB-Versorgung ohne gemeinsame Masse → Prüfsummenfehler (LinkMonitor hat das korrekt als „instabil“ gemeldet). Abhilfe: GND direkt verbinden.

### Nächste Schritte / Wünsche
- **W2 (umgesetzt 08.11.2026, Browser-Test durch Projektinhaber offen):** Updates über den Browser (Reiter „Update“, nur mit Login):
  - `firmware_<V>.bin` (direkt in den freien App-Bereich, nur gültiges ESP32-Programm, Kopf 0xE9) → Neustart; `webui_<V>.tar` (web/…) → kein Neustart; `package_<V>.tar` (firmware.bin + web/…) → Neustart.
  - TAR wird während des Uploads nach `/_new` entpackt und erst bei Vollständigkeit übernommen.
  - **Probestart:** neue Firmware läuft auf Probe; 3 Starts ohne 60 s stabilen Lauf → Rückfall auf die vorherige Firmware (eigene Lösung, `UpdateManager`; der Hersteller-Rückfall ist im Arduino-Paket nicht aktiv). Auch 3 schnelle Resets von Hand lösen den Rückfall aus.
  - Relais laufen während des Uploads weiter; der Neustart schaltet sie sicher ab, danach Startzustand. E-Paper zeigt „UPD“ während des Uploads.
  - Version `JJ.MM.TT-HHMM` automatisch (`tools/build_version.py` → `src/BuildVersion.h`, `data/version.json`, beide in `.gitignore`). `tools/build_packages.py` legt die drei Update-Dateien in `.pio/build/esp32dev/`.
  - Browser-Test Projektinhaber 08.11.2026: WebUI- und Paket-Update funktionieren. Finding: Ein Paket unter „Nur WebUI“ wurde angenommen (WebUI übernommen, Firmware still ignoriert) → behoben, wird jetzt mit Meldung abgelehnt.
  - Rückfall-Test Projektinhaber 08.11.2026 erfolgreich (3 × `absturz` → Rückfall auf die vorherige Firmware). Finding danach: Nach dem erneuten Paket-Update war das WLAN eine Weile nicht erreichbar (nur AP), WLAN-Suche ging nicht, Zugangsdaten schienen zu fehlen. Ursache: Fritz!Box lehnt den ersten Anmeldeversuch nach einem Neustart ab (Code 202) → 20 s Geduld zu kurz → AP-Rückfall mit Wiederholung erst nach 60 s; automatisches Wiederverbinden störte die Suche; Browser hielt die alte `app.js` im Cache. Behoben: 30 s Geduld, erster Wiederholversuch nach 15 s, Wiederverbinden während der Suche pausiert, WebUI-Dateien mit `Cache-Control: no-cache`.
  - **TESTFUNKTION `absturz`** im seriellen Monitor (löst `abort()` aus) zum Testen des Rückfalls → **später wieder entfernen** (in `main.cpp`, `handleCommandLine`).
  - **Kein ArduinoOTA** (Entscheidung Projektinhaber 08.11.2026). USB bleibt in der Entwicklung für Debug-Ausgaben und als Rettungsweg. Spätere Unterscheidung Develop/Serie möglich.
- **W2 abgeschlossen** (Projektinhaber 08.11.2026: WebUI-Update, Paket-Update, Paket-Sperre unter „Nur WebUI“, Rückfall getestet; Trennstriche im Update-Reiter, Hinweis „Passwort gespeichert“ fett). Committet vom Projektinhaber.
- **W3: MQTT – abgeschlossen 09.11.2026.** Test Projektinhaber mit MQTT.fx: Befehle (`vedirect/relay/r2-set` = `ON`) schalten, Empfangszähler zählt hoch, Topics und Letzter Wille ok. Hinweis zur Bedienung MQTT.fx: Profil (Zahnrad) mit Broker 192.168.178.41:1883 anlegen → Connect → Reiter Publish: Topic oben, Payload im großen Feld, Knopf Publish rechts neben dem Topic. Entscheidungen Projektinhaber 09.11.2026:
  - Benutzer/Passwort möglich (derzeit leer), kein TLS. Basis-Topic `vedirect` (einstellbar). **Getrennte JSON je Bereich.** Intervall 10 s Standard, **alle MQTT-Parameter in der WebUI einstellbar**.
  - **Alle Relais und Override per MQTT schaltbar**; Befehls-Topics `…/relay/r1-set`, `r2-set`, `r3-set`, `…/override-set` (ON/OFF, auch 1/0/true/false). Rücklesen der Relais (Hilfskontakt) evtl. späteres Upgrade.
  - Schalten soll auch im Camper ohne Router gehen (über AP/WebUI, W4); MQTT nur, wenn das Heim-WLAN erreichbar ist.
  - Umsetzung: `lib/WebCore/MqttManager` (PubSubClient, eigene `mqttTask`, Letzter Wille `…/status` online/offline, Wiederholung 5–60 s, Konfiguration im NVS-Namensraum `mqtt`, Passwort nie an die WebUI). Topics `…/battery`, `…/pv`, `…/relays`, `…/system` (retained, ungültige Werte = `null`), `…/command-result` (Rückmeldung, nicht retained). Sofort senden bei Relais-/Override-/Fehleränderung. WebUI-Reiter „MQTT“, `/api/mqtt` (GET/POST), `/api/mqtt/test`, `/api/mqtt/reset`. E-Paper-Symbol MQTT. Monitor `[MQTT]`-Meldungen, neue Befehle `r2|r3 ein|aus`.
  - **LoadController umgebaut (Modell „Schaltwunsch“):** jedes Relais hat einen Wunsch; R2/R3 nach Start „ein“, per Befehl schaltbar, SOC-Schutz stellt den Wunsch wieder her; Camper-R1 nach Start „aus“, nach Schutzabschaltung kein Wiedereinschalten. Alle Befehlsquellen erlaubt (Einschränkung per Konfiguration möglich). Override schaltet nur gewünschte Relais. 92/92 PC-Tests.
  - Test Projektinhaber 09.11.2026 mit `26.11.09-1651`: Verbindung, Topics, Letzter Wille (offline nach Ausschalten) ok. Finding „`r2-set off` schaltet nicht“: Nachricht kam am ESP32 nicht an (Empfangszähler blieb 0). Gegenprobe von Claude mit eigenem Testclient gegen 192.168.178.41: `vedirect/relay/r2-set` = `OFF` → R2 aus, `command-result` „ausgeführt“ → Firmware korrekt, Ursache auf Senderseite (Topic/Payload/Broker im Testprogramm).
  - Build `26.11.09-1716`: Monitor-Zeile `[MQTT] empfangen: <Topic> = <Wert>`; ganze Zahlen im JSON ohne führendes Leerzeichen (`String(float,0)` lieferte `" 0"`); Fortschrittsbalken im Update-Reiter abgetrennt mit Überschrift „Fortschritt“.
  - Nach dem Build `26.11.09-1651` geändert (in `26.11.09-1716` enthalten): E-Paper zeigt das MQTT-Symbol, sobald MQTT eingerichtet ist, **auch ohne WLAN – dann durchgestrichen** (Entscheidung Projektinhaber 09.11.2026).
  - Hinweis: Ein **retained** Befehl aus Node-RED (z. B. `r1-set ON`) wird nach jedem Neustart erneut ausgeführt → in Node-RED Retain bei Befehlen nur bewusst setzen.
  Ursprünglicher Vorschlag (zur Nachvollziehbarkeit):
  - WebUI-Reiter „MQTT“: Host, Port, Benutzer, Passwort (nur „gesetzt“), Basis-Topic, Intervall, ein/aus, „Verbindung testen“; NVS, rücksetzbar.
  - Bibliothek PubSubClient (ESP32 + ESP8266).
  - Last Will: `…/status` = `online`/`offline`.
  - Einzelne Topics je Wert, z. B. `vedirect/battery/soc`, `…/voltage`, `…/current`, `…/power`, `vedirect/pv/power`, `vedirect/pv/state`, `vedirect/relay/r1..r3` (ON/OFF), `vedirect/override`, `vedirect/faults`.
  - Befehle: `vedirect/relay/r1/set` ON/OFF (BKW; SOC-Schutz gilt immer; bei MQTT-Abbruch Zustand halten).
  - E-Paper: MQTT-Symbol, sobald eingerichtet.
  - **Offene Fragen:** (1) Broker: wo, Benutzer/Passwort, TLS nötig? (2) Topic-Schema `vedirect/…` oder `vedirect/<Gerätename>/…`, vorhandenes Schema in Node-RED? (3) Einzelne Topics oder JSON je Bereich? (4) Befehle nur R1 oder auch R2/R3/Override? (5) Intervall 10 s + sofort bei Änderung?
- **W4:** Projektseiten (Live-Werte, Relais, Override, Schwellen, **Statusmeldungen**), Sicherung/Wiederherstellung.
  - **Festgelegt 09.11.2026 (Projektinhaber):**
    - Reiter: **Übersicht** (wie Display + Schalter R1–R3/Override), **Shunt**, **MPPT**, **Historie**, **Einstellungen** (mind. einer; zuerst eigene Relais-Schwellen), **Fehler** (eigener Reiter für Fehleranzeigen/Meldungen), dazu **ein Sammelreiter „System“** mit WLAN, MQTT, Gerät, Update.
    - **W4a** ohne HEX: Live-Werte, Historie aus Textprotokoll (Shunt H1–H18, MPPT H19–H23), Relais/Override schalten, eigene Schwellen einstellen/speichern, Fehler/Meldungen.
    - **W4b** HEX nur **lesen**: MPPT-30-Tage-Historie (`0x1050`–`0x106E`, Summe `0x104F`), aktuelle Shunt- und MPPT-Einstellungen anzeigen. Vorher am Gerät prüfen, ob Text-Frames bei HEX-Verkehr pausieren (FAQ Q7); DeviceDetector/LinkMonitor müssen HEX-Pausen kennen.
    - **W4a Details (Projektinhaber 09.11.2026):**
      - Änderbar je Relais: Sperre unter, Aus unter, Frei ab (%), Mindest-Auszeit, Zustand nach Neustart, nach Schutzabschaltung automatisch wieder ein. Nicht änderbar: Sicherheitslogik ohne SOC, Relais aktiv/deaktiviert, Namen.
      - Override: Relais immer schaltbar (außer in der Anlage deaktivierte). **Verantwortung beim Benutzer → deutlich in die Bedienungsanleitung.**
      - Ereignisliste im RAM (50 Einträge), Zeit als Laufzeit, bis die RTC kommt.
      - Aktualisierung Standard 5 s, im Browser wählbar (2/5/10/30 s, je Browser gespeichert); nach Schaltbefehlen sofort.
      - Sicherung/Wiederherstellung: später.
      - Anlage Camper/Balkonkraftwerk in der WebUI: Auswahl unter Einstellungen, lädt Voreinstellung, Neustart (bestätigt 09.11.2026, umgesetzt in Schritt 5).
      - RTC (W4e/W5): interne Uhr + Stellen per NTP/Browserzeit, oder DS3231 mit Knopfzelle (Hardware, 2 Pins I2C) – noch zu entscheiden.
    - **W4a Schritt 1 (09.11.2026, umgesetzt, Test durch Projektinhaber offen):**
      - `WebUi::addApi(path, Method, handler)` + `ApiRequest`: Projekt-Endpunkte ohne WebServer-Abhängigkeit, Login/Sperre prüft WebUi. Anmelden vor `begin()` (main.cpp: `registerProjectApi()`).
      - `GET /api/live` = `{"battery","pv","relays","system"}`, dieselben JSON-Bausteine wie MQTT.
      - Oberfläche: Hauptreiter Übersicht, Shunt, MPPT, Historie, Fehler (mit Zähler-Markierung, noch ohne Funktion), Einstellungen, System (Unterreiter Info, WLAN, MQTT, Gerät, Update). Projekt-Reiter vorerst Platzhalter. Gewählter Reiter wird im Browser gemerkt (localStorage, fehlertolerant).
      - Test Schritt 1 durch Projektinhaber: ✓ (09.11.2026)
    - **W4a Schritt 2 (09.11.2026, umgesetzt, Test offen):** Übersicht (Batterie, Solar, Verbraucher R1–R3 mit Ein/Aus, Override mit Sicherheitsabfrage, Verbindung).
      - `POST /api/relay` (relay=1..3, on), `POST /api/override` (on): Befehl über die Queue (Quelle WebUi) mit `ticket`; Handler wartet max. 500 ms auf genau diesen Befehl und liefert Ergebnistext + aktuelles Live-JSON.
      - `/api/live` zusätzlich `limits` (Schwellen je Relais, aus `ControlSnapshot::configs`).
      - `/api/status` enthält über `setStatusExtension` `faults:{count,severity}` → Zähler am Reiter „Fehler“ und rotes Banner bei kritischem Fehler auf allen Reitern.
      - Ein gemeinsamer Aktualisierungstakt (Status + aktiver Reiter), Standard 5 s, Auswahl 2/5/10/30 s in der Fußzeile (je Browser), pausiert im Hintergrund.
      - Test Schritt 2 durch Projektinhaber: ✓ (09.11.2026). Wunsch: Override-Zustand in Großbuchstaben („AUS“) → umgesetzt.
    - **W4a Schritt 3 (09.11.2026, umgesetzt, Test offen):** Reiter Shunt, MPPT, Historie.
      - `GET /api/device?role=shunt|mppt`: Gerät, PID, UART, Alter letzter Block, Verbindungsstatistik (seit Start), alle empfangenen Felder mit Klarname, formatiertem Wert (ValueFormatter, Dezimalkomma), Alter und `history` (Label H + Ziffer).
      - DeviceState/LinkMonitor werden unter `stateMutex` in statische Kopien der webTask kopiert (~5 KB, nicht auf den Stack).
      - Werte älter als 10 s werden grau mit „vor x s“ angezeigt.
      - Test Schritt 3 durch Projektinhaber (09.11.2026): funktioniert, intensiver Test später. Findings → umgesetzt:
        - Zähler am Reiter „Fehler“ war bei Info-Fehlern (MPPT fehlt) grau, nach Wiederanstecken rot (Warnung „instabil“, 2 min). Für den Benutzer unverständlich → **jeder aktive Fehler rot**, unabhängig von der Schwere.
        - Interne Victron-Kürzel (PID, FW, H19 …) neben den Klarnamen entfallen; Produkt-ID im Kopf entfällt (steht als Messwert in der Liste).
    - **W4a Schritt 4 (09.11.2026, umgesetzt, Test offen):** Reiter „Fehler“.
      - Neu `lib/System/EventLog` (Ringpuffer 50 × 96 Byte, neueste zuerst, fortlaufende Nummer, UTF-8-sicheres Kürzen), Tests `test/test_event_log` (7 Tests; gesamt 99/99).
      - main.cpp: `addEvent()` (Spinlock `g_eventLock`). Ereignisse: Start mit Neustart-Grund (`esp_reset_reason`, Absturz/Watchdog/Brownout = kritisch), Rückfall auf alte Firmware, Gerät erkannt/gewechselt/ausgefallen (in `publish()`), Relais EIN/AUS mit Quelle bzw. Grund (automatisch AUS = Warnung), abgelehnte/gespeicherte Befehle, Override, Fehler aufgetreten/behoben (erst nach 10 s Laufzeit, Startrauschen).
      - `GET /api/faults`: aktive Fehler mit Schwere, MPPT-Fehlertext (ERR), Shunt-Alarmgrund (AR, nur bei Batteriealarm), Neustart-Grund, Laufzeit, Ereignisse.
      - Stack UART- und System-Task 4 → 6 KB (vsnprintf in addEvent).
      - Test Schritt 4 durch Projektinhaber (09.11.2026, Teiltest):
        - Shunt ab → kritischer Fehler, Relais gesperrt ✓. Shunt an → Spannung wieder da, Relais frei ✓.
        - `absturz` → Neustart-Grund „Absturz“ rot/kritisch: **so gewollt** (ungeplanter Neustart, Relais waren kurz aus).
        - Vermutetes Finding „Shunt erneut abgezogen während Entprellung → kein Fehler“: vom Projektinhaber am 10.11.2026 **nicht reproduzierbar**, alles wie beschrieben → kein Fehler. Wahrscheinlich zu kurz gewartet.
        - Reaktionszeit nach Abziehen (aus dem Code): Timeout 5 s ab letztem Block mit PID (`deviceTimeoutMs`), Auswertung 1-s-Takt, E-Paper sofort (~1 s Zeichnen), WebUI nach Abfragetakt → Display ca. 5–7 s, WebUI ca. 5–8 s (bei 2 s Takt).
    - **W4a Schritt 5 (10.11.2026, umgesetzt, Test offen):** Reiter „Einstellungen“.
      - `RelayLimits` in LoadController (Sperre/Aus/Frei, Mindest-Auszeit, Zustand nach Neustart, automatisch wieder ein) mit `validate()` (0 ≤ Aus ≤ Sperre ≤ Frei ≤ 100, Auszeit ≤ 3600 s) und `LoadController::setLimits()`; neues Ergebnis `RejectedInvalid`. 6 neue Tests (gesamt 105/105).
      - NVS: `app/install` (0 Camper, 1 Balkon, gelesen in `setup()`), `relays/r<n>.{lock,off,rel,minoff,start,auto,set}` (SOC in Zehntel-%). Fehlt `set` → Voreinstellung. Ungültige gespeicherte Werte → Voreinstellung + Warnereignis.
      - Änderung läuft als `CommandKind::Limits`/`ResetLimits` über die Befehlsqueue (nur die systemTask ändert die Konfiguration); danach sofort `update()` → evtl. Abschaltung erscheint als automatische Relaisänderung. Gespeichert wird erst nach Annahme durch die systemTask.
      - API: `GET /api/settings`, `POST /api/settings/relay`, `POST /api/settings/defaults` (ohne Neustart), `POST /api/settings/installation` (löscht `relays`, Neustart über neues `WebUi::requestRestart()`).
      - Ereignisse: Einstellungen geändert (mit Werten), zurückgesetzt, Anlage gewechselt.
      - Nicht änderbar (fest angezeigt): Verhalten ohne SOC, aktiv/deaktiviert, Namen.
      - Hinweis Umgebung: `pio test -e native` nur aus PowerShell; unter Git Bash bricht g++ (msys64) ohne Meldung ab.
      - Test Projektinhaber 10.11.2026 (`26.11.10-1746`): Anzeige der Werte ✓, ungültige Werte abgelehnt ✓, Anlagenwechsel mit Neustart ✓.
      - **OFFENE TESTS (brauchen dynamischen SOC → Stichwort „Shunt-Simulator“, gegen Projektende):** sofortige Wirkung geänderter Schwellen (Abschaltung bei SOC unter neuer Aus-Schwelle), Erhalt der Werte nach Neustart mit Wirkung, „Alle Relais auf Voreinstellung“, Einzel-Rücksetzen, Hysterese/Mindest-Auszeit/automatisch wieder ein.
      - Erweiterung (Wunsch Projektinhaber 10.11.2026, umgesetzt in `26.11.10-1804`): **einzelnes Relais auf Voreinstellung** (Knopf je Relais, nur sichtbar wenn „geändert“). `POST /api/settings/defaults` mit optionalem `relay=1..3`; löscht nur die NVS-Schlüssel dieses Relais. Damit ist Schritt 5 aus Sicht des Projektinhabers fertig.
      - **Finding 10.11.2026 (`26.11.10-1804`): WebUI lädt nicht** (keine Reiterinhalte, auch nach F5). Ursache: Claude hat app.js per Python-Heredoc über die Bash geändert; dabei wurde `\\n` zu einem echten Zeilenumbruch in einem JS-String → Syntaxfehler, das ganze Skript lief nicht. Abhilfe: korrigiert in `26.11.10-1819`. Künftig Dateiänderungen nur mit dem Edit-Werkzeug, keine Escape-Sequenzen über Bash-Heredocs.
      - Test Projektinhaber 10.11.2026 (FW `26.11.10-1804`, WebUI `26.11.10-1844` per USB/uploadfs): Einzel-Rücksetzen ✓ (Knopf nur bei „geändert“ sichtbar, so gewollt). **Schritt 5 abgeschlossen** (offene SOC-Tests siehe oben, Shunt-Simulator).
    - **W4a Schritt 6 (10.11.2026): Abschluss.** Bedienungsanleitung auf Stand FW `26.11.10-1804` / WebUI `26.11.10-1819`: Kapitel 3 (Reiterübersicht, Aktualisierung, Markierungen, Reiter „Fehler“ mit Ereignisliste und Reaktionszeit), Kapitel 7 neu gegliedert (7.1 Schalten, 7.2 Schwellen, 7.3 Override mit **Verantwortung beim Benutzer**, 7.4 Relais-Einstellungen, 7.5 Anlage, 7.6 Sicherheit), neue Pfade *System → …*, Kapitel 10 ergänzt um **10.1 Notweg WebUI per USB (uploadfs)** inkl. COM-Port-belegt-Hinweis. MQTT unverändert (keine neuen Topics; Einstellungen nur per WebUI).
    - **✅ W4a ABGESCHLOSSEN (10.11.2026).** Offen nur die SOC-abhängigen Tests aus Schritt 5 (Shunt-Simulator, gegen Projektende) und der intensive Test von Schritt 3. Nächste Stufe laut Plan: **W4b** (HEX nur lesen).
    - Offen (Projektinhaber 10.11.2026): Notweg Update per `curl` über WLAN hat nicht funktioniert, Ursache unbekannt (Ausgabe von curl fehlt noch).
    - **W4b Festlegungen (Projektinhaber 10.11.2026):**
      - **Nur lesen.** Die HEX-Bibliothek kann strukturell nur Ping, App-Version, Produkt-ID und Get senden (kein Set/Restart/Boot im Code).
      - TX-Leitungen (GPIO 17 / 33) sind „nach bestem Wissen“ angeschlossen → beim Erstkontakt prüfen.
      - **MPPT-Simulator sendet nur Text**, kein HEX → implementieren, ausführlicher Test später. Der Simulator muss ohnehin erweitert werden (HEX-Antworten, dynamischer SOC; „wir haben da Sachen noch nicht bedacht“) → Stichwort **Shunt-/MPPT-Simulator**.
      - Lesen: **nach dem Erkennen** des Geräts, **beim Öffnen des Reiters**, evtl. **per Knopf**. **Nicht regelmäßig.**
      - Einstellungen anzeigen: Shunt (Kapazität, Ladespannung, Schweifstrom, Ladeerkennungszeit, Peukert, Ladeeffizienz, Entladeboden), MPPT (Batteriespannung, Absorption, Erhaltung, max. Ladestrom, Batterietyp, Temperaturkompensation, Lastausgang) – endgültige Liste vor Schritt 5 vorlegen.
      - MQTT: erst nur WebUI (außer Integration jetzt deutlich einfacher als später).
      - Doku-Erkenntnisse: Rahmen `:` + Befehl (1 Ziffer) + Bytes (Hex, LE) + Prüfsumme, Summe = 0x55. MPPT sendet ab FW 1.17 Historie (0x104F/0x1050) **asynchron** (`:A…`), ab 1.26 bei HEX-Verkehr fast alle Register asynchron (2 min nach letzter Anfrage Ende); ab 1.53 pausieren Textblöcke bei HEX-Verkehr nicht mehr. Tag ohne Daten → Flag 0x04.
    - **W4b Schritt 1 (10.11.2026, umgesetzt):** `lib/Victron/VeDirectHex` (Namespace `VeHex`): Anfragen `ping()`, `appVersion()`, `productId()`, `get(reg)`; byteweiser `VeDirectHexParser` (ignoriert Textprotokoll, ':' als Text-Prüfsummenbyte wird still verworfen, toleriert `\r\n` und Kleinbuchstaben, zählt Prüfsummen-/Formatfehler); `readUnsigned/readSigned` (LE), `decodePing`, `decodeHistoryDay` (34 Byte), `decodeHistoryTotal` (19/34 Byte). Tests `test/test_vedirect_hex` (21 Tests, Beispiele aus der Doku; gesamt 126/126). ESP32-Build ok, noch nicht in main.cpp eingebunden.
    - **W4b Schritt 2 (10.11.2026, umgesetzt, Test offen): Erstkontakt/Diagnose** (`26.11.10-1947`).
      - `VeDirectReader::write()`; je Kanal `hexParser` (bekommt dieselben Bytes wie der Text-Parser) und eine hinterlegbare Anfrage (`requestHex()`, geschützt durch `g_statusLock`); die UART-Task sendet sie (`sendPendingHex()`), nur sie greift auf den UART zu.
      - Neue `LogKind::HexSent/HexMessage/HexError`; Logger zeigt Anfrage, Antwort (Ping mit Firmware, Get mit Register/Flags/Wert), Antwortzeit und **Abstand zum letzten Textblock** (zeigt, ob Textblöcke pausieren). `ChannelStatus.hex` (Zähler), `hexRequests`.
      - Monitor-Befehle: `hex ping|version|pid [1|2]`, `hex get <Register> [1|2]`, `hex async ein|aus` (ungefragte `:A`-Meldungen, Standard: angezeigt).
      - Steuerung, WebUI, MQTT unverändert. Noch keine automatischen Anfragen.
      - **Test Projektinhaber 10.11.2026, Shunt an UART1:** TX-Leitung ✓, Ping → FW 4.20 (Anwendung), PID `0xC035` (SmartShunt 500A/50mV), `get 1000` → 300 Ah. Antwort nach **8–14 ms**. **Textblöcke pausieren NICHT** (letzter Block 200–280 ms, kein Ausfall). UART2 = MPPT-Simulator antwortet nicht (nur Text, erwartet). **Echter MPPT: Test offen** (später, Simulator-Ausbau).
    - **W4b Schritt 3 (11.11.2026, umgesetzt, Test offen): automatische Abfragen** (`26.11.11-1616`).
      - `lib/Victron/VeDirectHexReader`: `HexRegisterCache` (48 Register × max. 34 Byte, Zustände ok/keine Daten/unbekannt/keine Antwort; ungefragte `:A`-Meldungen aktualisieren nur bereits bekannte Register), `HexJobRunner` (eine Anfrage nach der anderen, Zeitlimit 500 ms, 1 Wiederholung, Abbruch nach 2 Registern ohne Antwort in Folge → Simulator/fehlende TX-Leitung blockiert die Leitung nur ca. 2 s), `HexJobs::shunt()` (7 Einstellungen) und `HexJobs::mppt()` (7 Einstellungen + Historie gesamt + 31 Tage = 39). Tests `test/test_vedirect_hex_reader` (11, simuliertes Gerät; gesamt 137/137).
      - main.cpp: je Kanal `hexCache` (unter `stateMutex`), `hexJob`; Start nach **Erkennung** (auch Gerätewechsel, Cache wird geleert) und über `requestHexRefresh()` (für WebUI-Reiter/Knopf, Monitor `hex lesen`). **Nie regelmäßig.** Bei Ausfall: Abfrage abbrechen, Cache leeren.
      - Schutz für ältere MPPT-FW: Während einer Abfrage und 3 s danach wird ein Ausfall nicht gemeldet, solange das Gerät in den letzten 2 s auf HEX geantwortet hat.
      - Ereignisliste: Info „Gerät antwortet nicht auf HEX-Anfragen“ bzw. „x von y Werten ohne Antwort“. Monitor: Abschlusszeile, `hex cache [1|2]` zeigt den Zwischenspeicher. Antworten der Abfrage werden nicht einzeln ausgegeben.
      - RAM 33,5 % (+6,7 KB: 2 Caches + Ausgabekopie).
      - **Test Projektinhaber 11.11.2026 ✓:** Abfrage nach Erkennung (Shunt 7/7), `hex lesen 1` 7/7 in 159 ms, Simulator an UART2 → Abbruch + Info-Ereignis wie erwartet, Shunt ab → Cache leer, wieder an → neu gefüllt.
      - Shunt-Werte (umgerechnet laut BMV-Doku): 0x1000 = 300 Ah, **0x1001 Ladespannung = 0 (0,0 V) ❓**, 0x1002 = 4,0 % Schweifstrom, 0x1003 = 3 min, 0x1004 = 95 %, 0x1005 = Peukert 1,25, 0x1008 = Entladeboden 50,0 %. **Offen:** Vergleich mit VictronConnect durch den Projektinhaber (v. a. Ladespannung 0 V: echt eingestellt oder anderes Register beim SmartShunt?).
      - **Vergleich mit VictronConnect (11.11.2026):** 6 von 7 Werten stimmen exakt (Umrechnung bestätigt). VictronConnect zeigt Ladespannung **14,0 V**; Einzelabfrage `hex get 1001` liefert **140 = 14,0 V** ✓ → Register richtig. **Finding OFFEN:** In der automatischen Abfrage stand für 0x1001 zweimal **0** (nach Start und nach Wiederanstecken). Vermutete Ursachen: (a) Anfragen zu schnell hintereinander (nächste Anfrage wenige ms nach der Antwort), (b) Wert kurz nach Erkennung noch nicht bereit. Diagnose: `hex lesen 1` + `hex cache 1` mit laufender FW → 0 = (a) → Pause zwischen Anfragen; 140 = (b) → verzögert lesen.
        - Ergebnis Diagnose: `hex lesen 1` später → 0x1001 = **140** ✓ → Ursache (b): Wert direkt nach dem Erkennen (Kabel neu verbunden bzw. ESP-Start) noch 0, Grund am Gerät unklar (Shunt war nicht stromlos). **Abhilfe (`26.11.11-1646`):** automatische Abfrage erst **5 s nach dem Erkennen** (`HexAutoDelayMs`). **Test Projektinhaber 11.11.2026 ✓:** nach Start und nach Shunt ab/an jeweils 9/9 Register, 0x1001 = 140. **Finding behoben. W4b Schritt 3 abgeschlossen.**
      - **W4b Schritt 4 (11.11.2026, umgesetzt, Test offen): MPPT-Historie in der WebUI** (`26.11.11-1654`).
        - Lesen (Projektinhaber 11.11.2026): nach Erkennung, **beim Öffnen des Reiters** und **per Knopf „Neu einlesen“**. Kein Takt, keine Mindestwartezeit.
        - API: `GET /api/hex?role=shunt|mppt` (present, uart, device, pending, busy, job{finished, aborted, requested, answered, failed, ageS}; bei mppt `history{total, days[31]}` mit Ertrag, Verbrauch, Batterie min/max, Bulk/Abs./Erh. in min, P max, I max, Panel max, Fehlertexte aus der ERR-Tabelle), `POST /api/hex/refresh` (role). Beim Start der Abfrage wird `status.hexBusy` sofort gesetzt, damit die WebUI zwischen „angefordert“ und „läuft“ keine Lücke sieht.
        - WebUI Reiter *Historie*: Abschnitt „Solar (MPPT) – letzte 30 Tage“: Status, Knopf, Gesamtwerte, Balkendiagramm Tagesertrag (CSS, ohne Bibliothek), Tabelle je Tag (waagerecht wischbar). Hinweis bei „antwortet nicht“ (Simulator).
        - JS-Falle vermieden: `readHex()` läuft schon beim Laden (showTab) → Zustand als Eigenschaft der Funktion, keine `var`/`let` unten in der Datei.
        - Test mit echten Daten erst mit echtem MPPT (Simulator kann kein HEX).
        - Test Projektinhaber 11.11.2026: „passt soweit“ (Seite lädt, Hinweis „antwortet nicht“ beim Simulator).
      - **W4b Schritt 5 (11.11.2026, umgesetzt, Test offen): Geräteeinstellungen nur lesen** (`26.11.11-1705`).
        - Wunsch Projektinhaber: Reiter **Einstellungen** in Unterreiter **Steuerung** (Relais-Schwellen + Anlage, bisheriger Inhalt) | **Shunt** | **MPPT** (Hauptreiter Shunt/MPPT sind schon voll). „Aufhübschen später.“
        - app.js: `showSubIn(tab, name)` statt globalem `showSub` (Unterreiter je Hauptreiter, gemerkt als `sub.<tab>`; alter Schlüssel `sub` für System wird übernommen). Lesen beim Öffnen des Unterreiters + Knopf „Neu einlesen“; ein Lesen füllt beim MPPT Historie und Einstellungen.
        - main.cpp: Tabellen `ShuntSettings` (9, Namen wie VictronConnect) / `MpptSettings` (7) mit Umrechnung; `/api/hex` liefert `settings[{name, value, state}]` (Dezimalkomma). Register mit „unbekannt“ (z. B. Lastausgang beim 100/50) werden ausgeblendet. Batterietyp: Doku Rev 18 nummeriert die Drehschalter-Typen nicht eindeutig (Wert 2 doppelt) → Text **und** Code anzeigen, mit echtem MPPT prüfen.
        - Bedienungsanleitung (neue Pfade *Einstellungen → Steuerung*, Historie, Geräteeinstellungen) folgt in W4b Schritt 6.
        - Test Projektinhaber 11.11.2026: „scheint alles soweit zu funktionieren“ ✓.
      - **W4b Schritt 6 (11.11.2026): Abschluss.** Bedienungsanleitung auf `26.11.11-1705`: Reitertabelle (Historie mit 30 Tagen, Einstellungen mit Unterreitern Steuerung/Shunt/MPPT), Kapitel 3.2 (HEX-Daten nicht im Takt), **3.4 MPPT-Historie**, **3.5 Geräteeinstellungen (nur Anzeige, Ändern nur VictronConnect)**, Pfade *Einstellungen → Steuerung* in Kapitel 2 und 7, Fehlerbehebung „antwortet nicht auf HEX“ (TX-Leitung), 11.3 Monitor-Befehle (siehe unten). MQTT unverändert.
      - **✅ W4b ABGESCHLOSSEN (11.11.2026).** Offen (Test mit echtem MPPT bzw. erweitertem Simulator): MPPT-Historie mit echten Daten, MPPT-Einstellungen (v. a. Batterietyp-Nummerierung), Verhalten älterer MPPT-Firmware (Text-Pause, ungefragte `:A`-Meldungen). Nächste Stufen laut Plan: W4c leer, **W4d** (Shunt: wenige Befehle wie Historie löschen / SOC synchronisieren – separat entscheiden), **W4e/W5** (Echtzeituhr).
      - **Wunsch Projektinhaber 11.11.2026: alle Monitor-Befehle in die Anleitung**, als „nur für Entwickler“ gekennzeichnet → Bedienungsanleitung **Kapitel 11.3** (Relais/Override, Anzeige, HEX-Befehle mit nützlichen Registern, Testfunktion `absturz`). **Pflicht ab jetzt:** neue oder geänderte Monitor-Befehle dort mitpflegen.
      - Weitere Register am SmartShunt: 0x1006 = 10 (0,10 A Strom-Schwellwert ✓), 0x1007 = 3 (3 min Ø Restlaufzeit ✓), 0x1009 = 900 (Relais Low-SOC-Clear 90,0 %), 0x100A/0x100B unbekannt (Flag 01). „Richtung der Strommessung“ nicht dokumentiert → entfällt.
      - **0x1006 und 0x1007 in die Shunt-Liste aufgenommen** (Projektinhaber 11.11.2026) → 9 Register.
      - Nebenbefund: Gerät hat jetzt IP 192.168.178.112. Der curl-Notweg (Update über WLAN) nutzte die alte IP .41 → wahrscheinliche Ursache des Fehlschlags vom 10.11.2026. Besser `http://ve-direct.local/...` oder aktuelle IP aus *System → Info*.
    - **W4c bleibt leer** (Projektinhaber 09.11.2026). Reserviert für: **HEX-Schreiben am MPPT**, falls später doch gewünscht. Derzeit MPPT nur lesen, Einstellungen über VictronConnect.
    - **MPPT-Einstellungen nur lesen**, schreiben nur über VictronConnect (NVM-Verschleiß, Ladespannung kann LiFePO4 schädigen).
    - **W4d** Shunt-Einstellungen: separat entscheiden; evtl. nur wenige Befehle (z. B. Historie löschen `0x1030`, SOC synchronisieren `0x102C`) mit Bestätigung.
      - **⏭ ÜBERSPRUNGEN (Projektinhaber 11.11.2026), als späteres Ausbaupotenzial vorgemerkt.** Begründung: Die vorhandene BMV-700-Doku (Rev 7) nennt für die Befehle `0x102C` (SOC synchronisieren), `0x1030` (Historie löschen), `0x1029` (Nullstromkalibrierung), `0x0004` (Werkseinstellungen) nur die Register („write only“), **ohne Datentyp, Wert und Antwort**; kein Befehls-Beispiel; SmartShunt-Abweichungen von der BMV-Doku möglich (siehe 0x1001). Risiken: Historie löschen nicht umkehrbar; falsches SOC-Synchronisieren täuscht der SOC-Relaissteuerung einen vollen Akku vor (Tiefentladung, nur noch BMS als Schutz); Nullstromkalibrierung unter Last speichert dauerhaft falschen Nullpunkt; Werkseinstellungen nie einbauen. Nutzen gering, da alles in VictronConnect vorhanden. Gerät bleibt bis dahin **nur lesend**.
      - **TODO vor einer späteren Umsetzung:** (1) **neuere Victron-Doku** speziell für den SmartShunt suchen (HEX-Protokoll, aktuelle Revision); (2) **Umsetzungen im Netz** suchen (erprobte Open-Source-Projekte, die diese Befehle an SmartShunt/BMV senden) und Format/Antwort daraus ableiten; (3) erst dann Vorschlag mit Sicherheitsabfragen; höchstens „Historie löschen“ und ggf. „SOC synchronisieren“, nie „Werkseinstellungen“.
    - **W4e oder W5:** Echtzeituhr (RTC/NTP), danach Meldungen mit Uhrzeit.
    - **W4e Festlegungen (Projektinhaber 11.11.2026):**
      - Hardware: **Adafruit PCF8523** (ADA3295), I²C 0x68, 3,3 V, an GPIO 21 (SDA) / 22 (SCL). Knopfzelle **CR1220** noch nicht vorhanden (ohne: Zeit nach Stromausfall weg). Genauigkeit ±2 s/Tag.
      - **Eigener Minitreiber** statt Adafruit RTClib (zu groß). Als **eigene, projektneutrale Bibliothek** `lib/RtcPcf8523` (nur `Wire`), später separat vermarktbar. Datums-/BCD-Logik ohne Arduino → PC-Tests.
      - **Umstieg auf bessere RTC vorhalten** (z. B. DS3231): gemeinsame Schnittstelle `HardwareRtc`; Chip per Einstellung wählen (beide Adresse 0x68, keine Automatik).
      - **RTC läuft in UTC**, Umrechnung erst bei der Anzeige. **Zeitzone einstellbar** (Standard Deutschland MEZ/MESZ, Auswahl u. a. UTC).
      - **NTP:** nach WLAN-Verbindung, dann **1× pro Tag**; danach RTC nachstellen. Server Standard **`pool.ntp.org`, andere auswählbar**.
      - **Browserzeit:** automatisch nur, wenn keine gültige Zeit vorhanden; **kein Knopf**.
      - **Nicht** aufs E-Paper (voll), **kein** MQTT-Zeitfeld.
      - Anzeige: Ereignisliste mit Datum/Uhrzeit, System → Info (Zeit + Quelle), MPPT-Historie mit Datum.
    - **W4e Schritt 1 (11.11.2026, umgesetzt):** `lib/RtcPcf8523`: `RtcTime` (DateTime UTC, Schaltjahr, Wochentag, Unix ↔ Datum 2000–2099, BCD), `HardwareRtc` (Schnittstelle: begin/read/write/batteryLow/name), `Pcf8523` (Codec ohne Arduino + I²C-Klasse; begin(): STOP aus, 24 h, **Control_3 PM=000 → Umschaltung auf Knopfzelle an (ab Werk aus!)**; read() ungültig bei OS-Flag). Tests `test/test_rtc` (12).
    - **W4e Schritt 2 (11.11.2026, umgesetzt, Test offen):** `26.11.11-1745`. `Wire.begin(21, 22)` und `g_rtc.begin()` in `setup()` mit Ausgabe im Monitor; Monitor-Befehle `uhr`, `uhr stellen JJJJ-MM-TT hh:mm:ss` (UTC) → Anleitung 11.3. Eingabepuffer Monitor 32 → 48 Zeichen. I²C vorerst nur aus setup()/Logger-Task.
      - Test Projektinhaber 11.11.2026 ✓: RTC gefunden, stellen, läuft weiter, Neustart (ohne Stromlos) behält Zeit.
    - **W4e Schritt 3 (11.11.2026, umgesetzt, Test offen): NTP, Zeitzone, Browserzeit** (`26.11.11-1758`).
      - `lib/RtcPcf8523/RtcZones`: Zeitzonen (Deutschland Standard, UTC, GB, Portugal, Osteuropa; POSIX-TZ inkl. EU-Sommerzeit), NTP-Vorschläge (pool.ntp.org Standard, de.pool.ntp.org, ptbtime1.ptb.de, fritz.box, time.google.com), `isValidHostName`. Tests: test_rtc 14.
      - main.cpp Uhrdienst: Systemuhr in UTC; beim Start aus gültiger RTC; `startNtp()` in der webTask (`configTzTime`, Intervall 24 h, Callback setzt nur Merker; SNTP hält nur den Zeiger auf den Server → fester Puffer `g_ntpServer`, vor Änderung `sntp_stop()`); `clockLoop()` übernimmt den Abgleich und stellt die RTC nach (Ereignis nur beim Quellenwechsel, nicht täglich); Browserzeit `POST /api/time` nur, wenn keine gültige Zeit (< 2024-01-01). `g_i2cMutex` (+ `I2cLock`) für die RTC aus webTask und Logger-Task.
      - NVS `clock/tz`, `clock/ntp`. API: `GET/POST /api/clock` (tz, ntp), `POST /api/time` (unix). `/api/status` enthält `clock.valid` → WebUI schickt einmal pro Seitenaufruf die Browserzeit, wenn ungültig.
      - WebUI *System → Gerät → Uhr*: Uhrzeit, Quelle (mit Alter), RTC/Knopfzelle, Zeitzone (Auswahl), NTP-Server (Eingabe mit Vorschlagsliste).
      - Monitor `uhr` zeigt zusätzlich Systemzeit, Quelle, Zeitzone, NTP-Server.
      - Test Projektinhaber 11.11.2026 ✓ (NTP, Zeitzone, Server). **Browserzeit ungetestet.**
    - **W4e Schritt 4 (11.11.2026, umgesetzt, Test offen): Datum/Uhrzeit in der Anzeige** (`26.11.11-1803`).
      - Ereignisliste: Zeitpunkt = jetzt (Uhr) − Alter (millis) → **auch Ereignisse vor dem Stellen der Uhr** (z. B. Start) erhalten die richtige Zeit; EventLog unverändert. `/api/faults`: `events[].time`, `bootTime` (null ohne gültige Uhr → weiter „vor x min“).
      - `/api/status` → `clock{valid, local, source}`; *System → Info*: Zeile „Uhrzeit“ mit Quelle. Reiter *Fehler*: Laufzeit „(seit …)“.
      - MPPT-Historie: `/api/hex` → `today{y,m,d}` (Ortszeit des Geräts); Tage als „heute, 11.11.“, „gestern, 10.11.“, „Mo 09.11.“ (Hinweis: MPPT-„Tag“ = Solartag des Geräts, ungefähr Kalendertag).
      - Test Projektinhaber 11.11.2026: „perfekt“ ✓.
    - **W4e Schritt 5 (11.11.2026): Abschluss.** Bedienungsanleitung auf `26.11.11-1803`: neues **Kapitel 3.6 Uhr** (Quellen, Einstellungen, Knopfzelle CR1220, UTC intern), Ereignisliste mit Datum/Uhrzeit (3.3, dabei verrutschten Punkt „Reaktionszeit“ zurück nach 3.3), *System*-Unterreiter (Info mit Uhrzeit, Gerät mit Uhr), Fehlerbehebung (Uhr nicht gestellt / RTC nicht gefunden), Anhang 11.1 RTC-Anschluss, 11.3 Monitor `uhr`. Pinbelegung: I²C jetzt RTC.
    - **✅ W4e ABGESCHLOSSEN (11.11.2026).** Offen: **Browserzeit ungetestet** (Test: ohne Knopfzelle stromlos, dann nur AP ohne Internet, WebUI öffnen). Später: Knopfzelle CR1220 besorgen; ggf. DS3231 als zweite `HardwareRtc`-Klasse; Bibliothek `lib/RtcPcf8523` ggf. separat veröffentlichen (Lizenz klären).
  - Quellen HEX: `docs/VeDirect-Doku/Victron_BlueSolar-SmartSolar-MPPT-HEX-protocol_Rev18.pdf`, `docs/VeDirect-Doku/Victron_BMV-7xx-SmartShunt-HEX-protocol_Rev7.pdf` (BMV-700-Doku; SmartShunt-Abweichungen am Gerät prüfen).
- **Test 09.11.2026 (Projektinhaber): SmartShunt auf FW 4.20 geflasht** während des Betriebs. Verhalten wie festgelegt: Wechselrichter (R1, war EIN) fiel ab, SOC und Strom „--“, Spannung ca. 13,45 V mit `*` (Ersatzwert vom MPPT), Warndreieck kritisch. ✓
- **E-Paper mit Symbolen (umgesetzt 08.11.2026):** Symbolleiste: WLAN-Balken (4 Stufen nach RSSI, durchgestrichen ohne WLAN), AP (schwarz = an, durchgestrichen = aus), MQTT (nur bei WLAN + eingerichtetem MQTT, ab W3), OVR (nur bei Override), Warndreieck (gefüllt = kritisch, leer = Warnung). Lade-/Entladepfeil am SOC (▲ lädt, ▼ entlädt, o Ruhe bei |I| < 0,05 A). Untere Zeile: Shunt/MPPT ● ok, ◐ instabil, ⊗ defekt. Textliche Statuszeile entfällt (Details → WebUI). Sofort aktualisiert: Symbole/Zustände/Relais; WLAN-Balken und Pfeil im 30-s-Takt mit den Werten.
- MPPT-Simulator: GND war laut Projektinhaber immer verbunden; Ursache der Prüfsummenfehler nach der Umstellung auf USB-Versorgung noch offen → weiter beobachten.
- Später: ESP8266-Variante von `lib/WebCore`, Uhrzeit (RTC/NTP), ESP-NOW-Bediengerät, Shunt-Konfiguration (HEX-Protokoll).
- **Am Ende: kurze Anleitung** für Bedienung (Ersteinrichtung, WLAN, Passwort, Reset-Taste, Updates, Relais/Override) – Wunsch Projektinhaber 08.11.2026.
  - **PFLICHT (Projektinhaber 09.11.2026): alle MQTT-Messwerte und alle Schaltbefehle** müssen in die Anleitung. Stand 09.11.2026 (Basis-Topic einstellbar, Standard `vedirect`):
    - Senden (retained, JSON, ungültig = `null`):
      - `vedirect/status` = `online` / `offline` (Letzter Wille, ca. 45 s nach Ausfall)
      - `vedirect/battery` = `soc` (%), `voltage` (V), `voltageSource` (`shunt`/`mppt`/`none`), `current` (A, + laden / − entladen), `power` (W), `consumedAh` (Ah), `timeToGoMin` (min), `timeToGoInfinite` (true = keine Entladung)
      - `vedirect/pv` = `power` (W), `voltage` (V), `yieldTodayKWh` (kWh), `state` (Text), `stateCode` (CS-Code), `errorCode` (ERR)
      - `vedirect/relays` = `r1`/`r2`/`r3` (`ON`/`OFF`/`DISABLED`), je Relais `…Name`, `…Locked` (gesperrt), `…Wanted` (Schaltwunsch), `…Reason` (Grund), `override` (`ON`/`OFF`)
      - `vedirect/system` = `faults` (Bitmaske hex), `severity` (`none`/`info`/`warning`/`critical`), `faultTexts` (Liste), `shunt`/`mppt` (`ok`/`instabil`/`defekt`), `firmware`, `uptimeS`, `rssi` (dBm)
      - `vedirect/command-result` (nicht retained) = `command`, `value`, `result` (z. B. „ausgeführt“, „abgelehnt: SOC zu niedrig …“, „gespeichert, wird bei ausreichendem SOC ausgeführt“)
    - Befehle (Payload `ON`/`OFF`, auch `1`/`0`, `true`/`false`, `EIN`/`AUS`; Groß/klein egal):
      - `vedirect/relay/r1-set` (Wechselrichter bzw. DC-DC), `vedirect/relay/r2-set` (Licht), `vedirect/relay/r3-set` (Kühlschrank), `vedirect/override-set`
      - Hinweise: SOC-Schutz gilt immer (außer Override); Befehle in Node-RED **ohne Retain** senden (sonst nach Neustart erneut ausgeführt); Sendeintervall Standard 10 s, bei Relais-/Override-/Fehleränderung sofort.
    - Serieller Monitor (USB, Entwicklung): `r1|r2|r3 ein|aus`, `override ein|aus`, `bloecke ein|aus`, `hilfe` (Testbefehl `absturz` wird vor der Anleitung entfernt).
    - Bei jeder späteren Änderung an Topics/Befehlen diese Liste mitpflegen.
  - **Begonnen 09.11.2026: `docs/Bedienungsanleitung.md`** (gepflegt, im Gegensatz zu den alten `docs/*.md`). Gliederung: Überblick, Ersteinrichtung, WebUI, WLAN, Passwort/Reset-Taste, E-Paper, Relais/Override, Updates, **MQTT (vollständig)**, Fehlerbehebung, Anhang (Fehlerwort, Monitor). Platzhalter „folgt“ für W4 u. a. **Bei jeder Stufe mitpflegen.**

### Code-Stil
- Aussagekräftige Kommentare im Code (Wunsch des Projektinhabers).
