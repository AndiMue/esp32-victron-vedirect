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
| I²C Reserve | 21 SDA, 22 SCL |
| Reserve Eingänge | 35, 36, 39 (externer Pull-up nötig; 36/39 analog) |
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

### Code-Stil
- Aussagekräftige Kommentare im Code (Wunsch des Projektinhabers).
