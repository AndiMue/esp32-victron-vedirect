# Bedienungsanleitung – VE.Direct Steuergerät

> **Stand:** 12.11.2026, Firmware und WebUI ab `26.11.12-1817`
> **Status:** Entwurf, wächst mit jeder Entwicklungsstufe mit.
> Abschnitte mit *„folgt“* sind noch nicht umgesetzt.

---

## Inhalt

1. Überblick
2. Ersteinrichtung
3. Weboberfläche (WebUI)
4. WLAN
5. Gerätepasswort und Reset-Taste
6. Statusanzeige (E-Paper)
7. Relais, Schutzabschaltung und Override
8. Updates
9. MQTT
10. Fehlerbehebung
11. Anhang

---

## 1. Überblick

Das Steuergerät liest einen **Victron SmartShunt** und einen **Victron SmartSolar MPPT** über VE.Direct aus, zeigt die wichtigsten Werte an und schaltet bis zu drei Verbraucher über Relais abhängig vom Ladezustand (SOC) der Batterie.

| Relais | Camper | Balkonkraftwerk |
|---|---|---|
| R1 | Wechselrichter | DC-DC-Wandler |
| R2 | Licht | – (deaktiviert) |
| R3 | Kühlschrank | – (deaktiviert) |

- Jedes Victron-Gerät kann an **jedem** der beiden VE.Direct-Anschlüsse hängen. Das Gerät erkennt selbst, welches wo angeschlossen ist.
- Der **SmartShunt ist Pflicht** für die Steuerung, der **MPPT ist optional**.
- Bedienung über die **Weboberfläche** (Handy oder PC), per **MQTT** (Hausautomatisierung) und später über ein abgesetztes Bediengerät *(folgt)*.

---

## 2. Ersteinrichtung

1. Gerät einschalten. Ohne eingerichtetes WLAN startet es einen eigenen **Access Point (AP)**.
2. Auf dem Display stehen unten der **WLAN-Name** (`VE-Direct-XXXX`) und das **Startpasswort** (8 Zeichen).
3. Mit dem Handy oder PC mit diesem WLAN verbinden.
4. Meist öffnet sich die Einrichtungsseite automatisch. Sonst im Browser **http://192.168.4.1** aufrufen.
5. Anmelden mit Benutzer **`admin`** und dem **Startpasswort**.
6. Unter **System → WLAN** das Heimnetz einrichten (siehe Kapitel 4).
7. Unter **System → Gerät** das Passwort ändern (siehe Kapitel 5). Danach verschwindet das Passwort vom Display. Solange das Startpasswort gilt, zeigt die WebUI oben einen gelben Hinweis.
8. Unter **Einstellungen → Steuerung** die **Anlage** prüfen (Camper oder Balkonkraftwerk, siehe Kapitel 7.5).

---

## 3. Weboberfläche (WebUI)

- Im Heimnetz erreichbar unter **http://ve-direct.local** (oder über die IP-Adresse, siehe *System → Info*), im AP unter **http://192.168.4.1**.
- Anmeldung: Benutzer **`admin`**, Passwort = **Gerätepasswort**. Nach 5 Fehlversuchen ist die Anmeldung 1 Minute gesperrt.
- Die Oberfläche funktioniert ohne Internet und auf dem Handy (die Reiterleiste lässt sich waagerecht wischen).
- Der zuletzt gewählte Reiter wird im Browser gemerkt.

### 3.1 Reiter

| Reiter | Inhalt |
|---|---|
| **Übersicht** | SOC, Spannung, Strom, Leistung, Restlaufzeit, PV-Werte, Verbindungen; Relais schalten; Override (Kapitel 7) |
| **Shunt** | alle Werte des SmartShunt im Klartext, Verbindungsstatistik |
| **MPPT** | alle Werte des Solarladereglers im Klartext, Verbindungsstatistik |
| **Historie** | Historienwerte von Shunt und MPPT (z. B. tiefste Entladung, Ertrag gesamt) und die **MPPT-Historie der letzten 30 Tage** (Kapitel 3.4) |
| **Fehler** | aktive Fehler, Gerätemeldungen, letzter Neustart, Ereignisliste (Kapitel 3.3) |
| **Einstellungen** | Unterreiter **Steuerung** (Relais-Schwellen und Anlage, Kapitel 7.4 und 7.5), **Shunt** und **MPPT** (Geräteeinstellungen, nur Anzeige, Kapitel 3.5) |
| **System** | Unterreiter **Info** (Netzwerk, MQTT, Versionen, Uhrzeit, Laufzeit, freier Speicher sowie kleinster freier Speicher und knappste Stack-Reserve seit Start – rot unter der Warngrenze), **WLAN** (Kapitel 4), **MQTT** (Kapitel 9), **Gerät** (Passwort Kapitel 5, Uhr Kapitel 3.6, Sicherung Kapitel 3.7, Neustart), **Update** (Kapitel 8) |

### 3.2 Anzeigen und Aktualisierung

- **Aktualisierung:** Unten auf der Seite wählbar (2, 5, 10 oder 30 s, Standard 5 s). Die Einstellung gilt nur für diesen Browser. Ist die Seite im Hintergrund, wird nicht abgefragt.
- Der Reiter *Einstellungen* wird **nicht** automatisch aktualisiert, damit Eingaben nicht überschrieben werden.
- Die MPPT-Historie (30 Tage) und die Geräteeinstellungen werden ebenfalls **nicht** im Takt aktualisiert, sondern nur beim Öffnen des (Unter-)Reiters und mit **„Neu einlesen“** aus dem Gerät gelesen (Kapitel 3.4, 3.5).
- Der zuletzt gewählte Reiter und Unterreiter werden im Browser gemerkt.
- **Zahl am Reiter „Fehler“** (rot): Anzahl der aktiven Fehler, unabhängig von der Schwere.
- **Roter Balken oben** (auf allen Reitern): Es liegt ein kritischer Fehler vor. Ein Klick darauf öffnet den Reiter *Fehler*.
- In den Reitern *Shunt* und *MPPT* werden Werte, die älter als 10 s sind, grau mit „vor x s“ angezeigt.
- Spannung mit **`*`**: Ersatzwert vom MPPT, weil der SmartShunt fehlt (siehe Kapitel 6). `–` = kein gültiger Wert.

### 3.3 Reiter „Fehler“

- **Aktive Fehler** mit Schwere: *kritisch* (rot), *Warnung* (orange), *Info* (grau). Ohne Fehler: „✓ Keine aktiven Fehler“. Liste der möglichen Fehler: Anhang 11.2.
- **Meldungen der Geräte:** Fehlertext des MPPT und Alarmgrund des Shunts (nur bei Batteriealarm).
- **Letzter Neustart:** Grund und Laufzeit seither, bei gestellter Uhr mit Startzeitpunkt („seit …“). *Absturz*, *Watchdog* und *Unterspannung* sind rot markiert: Das Gerät ist ungeplant neu gestartet, die Relais waren dabei kurz aus.
- **Ereignisliste:** die letzten 50 Ereignisse, neueste oben, mit **Datum und Uhrzeit** (beim Darüberfahren: „vor …“). Ist die Uhr nicht gestellt, steht dort „vor x min“. Auch Ereignisse von vor dem Stellen der Uhr (z. B. der Start) erhalten nachträglich die richtige Zeit. Erfasst werden:
  - Gerät gestartet (mit Grund und Firmware), Rückfall auf die vorherige Firmware
  - Victron-Gerät erkannt, gewechselt oder ausgefallen
  - Relais EIN/AUS mit Quelle (WebUI, MQTT, lokal) bzw. Grund; eine automatische Abschaltung ist eine Warnung
  - abgelehnte oder gespeicherte Schaltbefehle, Override ein/aus
  - Fehler aufgetreten / behoben (erst ab 10 s nach dem Start, vorher werden die Geräte noch erkannt)
  - geänderte Relais-Einstellungen, Zurücksetzen auf Voreinstellung, Anlagenwechsel
  - Uhr per NTP gestellt bzw. vom Browser übernommen, geänderte Uhr-Einstellungen (Kapitel 3.6)
  - Warnung „Arbeitsspeicher knapp“ (kleinster freier Wert seit Start unter 30 KB) bzw. „Stack der Task … knapp“ (unter 1 KB Reserve); wird 1× pro Minute geprüft und je Ursache nur einmal gemeldet
- Die Ereignisliste liegt im Arbeitsspeicher und ist nach einem Neustart leer (bis auf den Start selbst).
- Antwortet ein Victron-Gerät nicht auf Leseanfragen (Kapitel 3.4), steht dort die Info „Gerät antwortet nicht auf HEX-Anfragen“.
- **Reaktionszeit:** Wird ein Gerät abgezogen, erscheint der Fehler nach ca. 5–7 s auf dem Display und nach ca. 5–8 s in der WebUI (je nach Aktualisierungstakt). Die Wartezeit von 5 s verhindert Fehlalarme bei einzelnen verlorenen Datenblöcken.

### 3.4 MPPT-Historie (Reiter „Historie“, letzte 30 Tage)

Der MPPT speichert für jeden der letzten 30 Tage eine Tageszusammenfassung. Das Steuergerät liest sie aus dem MPPT:
- einmal kurz nach dem Erkennen des MPPT,
- beim Öffnen des Reiters *Historie*,
- mit dem Knopf **„Neu einlesen“**.

Das Lesen dauert etwa 1 s. Danach zeigt der Reiter:
- **Statuszeile:** Gerät, Anschluss und wann gelesen wurde.
- **Gesamtwerte:** Gesamtertrag, höchste Panelspannung, höchste und niedrigste Batteriespannung, Anzahl Tage mit Daten, letzte Fehler.
- **Balkendiagramm** des Tagesertrags (links vor 30 Tagen, rechts heute; Wert beim Darüberfahren bzw. Antippen).
- **Tabelle je Tag:** Ertrag, Maximalleistung, Batteriespannung min–max, Zeit in Bulk / Absorption / Erhaltung, Fehler (rot). Auf dem Handy waagerecht wischbar.

Tage ohne Daten (z. B. nach dem Löschen der Historie) erscheinen grau mit „keine Daten“. Antwortet der MPPT nicht, steht dort „antwortet nicht auf HEX-Anfragen“.

### 3.5 Geräteeinstellungen (Einstellungen → Shunt / MPPT)

Zeigt die aktuellen Einstellungen der Victron-Geräte an – **nur zur Ansicht**. Ändern ist nur mit der App **VictronConnect** möglich (Schutz vor versehentlichem Verstellen; falsche Ladespannungen können den Akku schädigen).

Gelesen wird beim Öffnen des Unterreiters und mit **„Neu einlesen“**.

| SmartShunt (Namen wie in VictronConnect) | MPPT |
|---|---|
| Batteriekapazität | Batteriespannung (12/24 V oder automatisch) |
| Spannung wenn aufgeladen | Absorptionsspannung |
| Schweifstrom | Erhaltungsspannung |
| Zeit für Ladezustandserkennung | Max. Ladestrom |
| Ladewirkungsgrad | Batterietyp (mit Typnummer des Drehschalters) |
| Peukert-Exponent | Temperaturkompensation |
| Strom-Schwellwert | Lastausgang (nur bei Modellen mit Lastausgang) |
| Durchschnittliche Restlaufzeit | |
| Entladungsboden | |

### 3.6 Uhr (System → Gerät → Uhr)

Das Gerät hat eine **Echtzeituhr (RTC)**. Sie sorgt für Datum und Uhrzeit in der Ereignisliste, unter *System → Info* und in der MPPT-Historie.

**Die Uhr stellt sich selbst:**
1. Beim Einschalten aus der Echtzeituhr (wenn diese eine gültige Zeit hat).
2. Per **Internet (NTP)**, sobald das Gerät mit einem WLAN mit Internet verbunden ist, danach **einmal täglich**. Die Echtzeituhr wird dabei nachgestellt.
3. Hat das Gerät gar keine gültige Zeit (z. B. Camper ohne Internet und Echtzeituhr ohne Knopfzelle nach einem Stromausfall), übernimmt es beim Öffnen der Weboberfläche automatisch die **Uhrzeit des Handys bzw. PCs**.

**Einstellungen:**

| Einstellung | Bedeutung | Standard |
|---|---|---|
| Zeitzone | Anzeige in Ortszeit; Sommer-/Winterzeit wird automatisch umgestellt. Auswahl u. a. Deutschland, UTC, Großbritannien, Portugal, Osteuropa | Deutschland (MEZ/MESZ) |
| NTP-Server | Zeitserver im Internet; Vorschlag aus der Liste oder eigener Name/IP (z. B. `fritz.box`) | `pool.ntp.org` |

Angezeigt werden außerdem die aktuelle Uhrzeit, wie sie zuletzt gestellt wurde (Echtzeituhr, Internet, Browser) und der Zustand der **Knopfzelle**.

**Knopfzelle:** Die Echtzeituhr braucht eine Knopfzelle **CR1220**. Ohne sie läuft die Uhr nur, solange das Gerät Strom hat. Nach einem Stromausfall ist die Zeit dann weg, bis sie neu gestellt wird (Internet bzw. Browser). Hinweis „Knopfzelle fehlt/schwach“ → Knopfzelle einsetzen bzw. tauschen.

Intern läuft die Uhr in UTC; Zeitzone und Sommerzeit werden nur bei der Anzeige berücksichtigt. Genauigkeit der Echtzeituhr: etwa ±2 s pro Tag (wird per NTP täglich korrigiert).

### 3.7 Sicherung und Wiederherstellung (System → Gerät)

**Sichern:** **„Einstellungen herunterladen“** speichert eine Datei `vedirect-einstellungen-JJJJ-MM-TT.json` mit:
- Anlage (Camper/Balkonkraftwerk) und den **wirksamen Werten aller Relais** (auch wenn sie der Voreinstellung entsprechen),
- Uhr (Zeitzone, NTP-Server),
- MQTT (Broker, Port, Benutzer, Basis-Topic, Intervall, Client-ID, ein/aus),
- WLAN (Netzname, „Access Point immer an“, Gerätename im Netz).

**Passwörter (WLAN, MQTT, Gerät) sind nie in der Datei.** Die Datei ist lesbarer Text und kann mit einem Editor angesehen und geändert werden, z. B. `"relay2.minOffS": 120`. Beim Wiederherstellen wird jeder Wert geprüft. Relais-Werte, die der Voreinstellung der Anlage entsprechen, gelten danach als „Voreinstellung“, abweichende als „geändert“.

Der aktuelle **Schaltzustand** der Relais (ein/aus) ist keine Einstellung und wird nicht gesichert; nach dem Neustart gilt „Zustand nach Neustart“ (`relayN.startup`).

**Wiederherstellen:**
1. Datei auswählen, **„Datei prüfen“**. Das Gerät prüft alle Werte und zeigt eine Vorschau. Eine fehlerhafte oder fremde Datei wird mit Grund abgelehnt – es wird nichts übernommen.
2. **„Wiederherstellen und neu starten“**: Es werden **immer alle** Einstellungen ersetzt. Das Gerät startet neu, die Relais fallen dabei kurz ab.
3. Die **Passwörter bleiben**, wie sie auf dem Gerät gespeichert sind. Auf einem neuen oder zurückgesetzten Gerät müssen WLAN- und MQTT-Passwort danach neu eingegeben werden (bis dahin ist das Gerät über seinen Access Point erreichbar).

Eine Sicherung aus einer neueren Firmware wird abgelehnt (erst die Firmware aktualisieren).

---

## 4. WLAN

- **Netzwerke suchen**, gewünschtes Netz antippen, Passwort eingeben, **Speichern und verbinden**. Die Seite meldet nach einigen Sekunden Erfolg (mit IP-Adresse) oder den Grund des Fehlers.
- Das Gerät kann nur **2,4-GHz-WLAN**.
- Ist das Heimnetz nicht erreichbar, schaltet das Gerät nach ca. 30 s zusätzlich seinen **Access Point** ein und versucht es weiter (erster neuer Versuch nach 15 s, dann jede Minute).
- **„Access Point immer eingeschaltet lassen“**: z. B. im Camper ohne Router.
- **Passwortfeld leer lassen** = gespeichertes Passwort behalten. „Passwort anzeigen“ zeigt die Eingabe im Klartext.
- **WLAN zurücksetzen** löscht die Zugangsdaten. Danach ist das Gerät nur noch über seinen AP erreichbar.

---

## 5. Gerätepasswort und Reset-Taste

- Das **Gerätepasswort** gilt für den **Access Point** und die **Anmeldung an der Weboberfläche**.
- Ändern unter **System → Gerät → Gerätepasswort ändern** (8–63 Zeichen).
- Passwörter (WLAN, MQTT, Gerät) werden nie an den Browser zurückgegeben. Angezeigt wird nur „gespeichert“.

**Passwort vergessen? Reset-Taste:**
1. Gerät ausschalten.
2. Reset-Taste drücken und **gedrückt halten**, Gerät einschalten.
3. **10 Sekunden halten, dann loslassen.**
4. WLAN-Zugangsdaten und Gerätepasswort sind zurückgesetzt. Ein neues Startpasswort steht auf dem Display. Weiter wie bei der Ersteinrichtung.

Wird die Taste zu früh oder gar nicht losgelassen, passiert nichts. Relais-Einstellungen und MQTT bleiben erhalten.

---

## 6. Statusanzeige (E-Paper)

```
┌──────────────────────────────┐
│ ▂▄▆█  [AP] [MQTT]   [OVR]  ⚠ │
│  ▲        100,0%             │
│ 12,33V               -1,5A   │
│ -18W               PV 96W    │
│──────────────────────────────│
│ [ WR ] [Licht] [Kuehl]       │
│ Shunt ●          MPPT ◐      │
└──────────────────────────────┘
```

| Symbol | Bedeutung |
|---|---|
| WLAN-Balken | Signalstärke (4 Stufen); durchgestrichen = kein WLAN |
| **AP** | schwarz = Access Point an, durchgestrichen = aus |
| **MQTT** | nur sichtbar, wenn MQTT eingerichtet ist; schwarz = verbunden, durchgestrichen = nicht verbunden |
| **UPD** | Update läuft – Gerät nicht ausschalten |
| **OVR** | Override aktiv (alle Sperren aufgehoben) |
| ⚠ | gefüllt = kritischer Fehler, leer = Warnung (Details in der WebUI) |
| ▲ / ▼ / o | Batterie wird geladen / entladen / Ruhe |
| `12,33V*` | Stern: Spannung stammt vom MPPT, weil der SmartShunt fehlt (siehe unten) |
| `--` | kein gültiger Wert (z. B. SOC und Strom ohne SmartShunt) |
| Relaisfelder | schwarz = EIN, weiß = AUS, durchgestrichen = deaktiviert |
| ● / ◐ / ⊗ | Verbindung zum Gerät ok / instabil / defekt (fehlt oder ausgefallen) |

**Was passiert, wenn der SmartShunt ausfällt?** (z. B. Kabel ab, Firmware-Update am Shunt)
- **SOC** und **Strom** zeigen `--`. Ohne Shunt gibt es keine gültigen Werte, das Gerät zeigt bewusst keine geschätzten an.
- Die **Spannung** kommt dann vom MPPT, erkennbar am **Stern**, z. B. `13,45V*`. Der MPPT misst die Batteriespannung an seinem eigenen Anschluss. Der Wert kann leicht vom Shunt abweichen und dient nur zur Orientierung.
- Das **Warndreieck ist gefüllt** (kritischer Fehler), unten steht `Shunt ⊗`.
- Der **Wechselrichter wird ausgeschaltet** und gesperrt. Licht und Kühlschrank laufen weiter.
- Ist der Shunt wieder da, verschwinden Stern und Warndreieck. Der Wechselrichter bleibt aus und muss neu eingeschaltet werden.

- Werte werden höchstens alle 30 s aktualisiert, Relais, Symbole und Fehler sofort. Bei jeder Aktualisierung flackert das Display kurz (normal bei E-Paper).
- Während der Ersteinrichtung stehen unten WLAN-Name und Startpasswort.

---

## 7. Relais, Schutzabschaltung und Override

### 7.1 Schalten

**Schaltwunsch:** Jedes Relais hat einen Schaltwunsch (ein/aus), den man per WebUI (Reiter *Übersicht*, Knöpfe **Ein**/**Aus**), MQTT (Kapitel 9.3) oder Bediengerät *(folgt)* setzt. Das Relais ist EIN, wenn es gewünscht ist **und** der Akku-Schutz es erlaubt.

- Nach jedem Schaltbefehl in der WebUI erscheint unten eine Rückmeldung, z. B. „ausgeführt“ oder „abgelehnt: SOC zu niedrig oder kein gültiger SOC“ (rot).
- Unter jedem Relais stehen Zustand (**EIN**/**AUS**/deaktiviert), Sperre, ein wartender Schaltwunsch, die Schwellen und der Grund der letzten Änderung.

**Nach dem Einschalten des Geräts (Voreinstellung Camper):** Wechselrichter **aus**, Licht und Kühlschrank **ein**. Änderbar unter *Einstellungen → Steuerung* („Zustand nach Neustart“).

### 7.2 Schwellen und Schutzabschaltung

**Schwellen (Voreinstellung, Camper):**

| Relais | Einschalten gesperrt unter | Zwangsweise aus unter | Wieder frei ab |
|---|---|---|---|
| R1 Wechselrichter | 25 % | 15 % | 30 % |
| R2 Licht | 15 % | 15 % | 20 % |
| R3 Kühlschrank | 5 % | 5 % | 10 % |

- Ein **laufender Wechselrichter** wird bei 25 % nicht abgeschaltet, nur das Neu-Einschalten ist gesperrt. Abgeschaltet wird er erst unter 15 %.
- Nach einer Schutzabschaltung schalten **Licht und Kühlschrank automatisch wieder ein**, sobald der SOC die Freigabeschwelle erreicht (frühestens nach 60 s). Der **Wechselrichter** muss neu eingeschaltet werden. Im Balkonkraftwerk schaltet der **DC-DC-Wandler** automatisch wieder ein.
- **Ohne SmartShunt** (kein gültiger SOC) wird der Wechselrichter ausgeschaltet und gesperrt. Licht und Kühlschrank laufen weiter.
- Wird ein Relais unterhalb der Sperrschwelle eingeschaltet, wird der Wunsch gespeichert und bei ausreichendem SOC ausgeführt (Licht, Kühlschrank, DC-DC) bzw. abgelehnt (Wechselrichter).
- Schwellen einstellen: *Einstellungen → Steuerung* (Kapitel 7.4).

### 7.3 Override

- Einschalten im Reiter *Übersicht* (mit Sicherheitsabfrage) oder per MQTT.
- Hebt **alle Sperren** auf (SOC-Schwellen, fehlender Shunt). Alle Relais lassen sich dann **immer** schalten. Ausgenommen sind nur Relais, die in der gewählten Anlage deaktiviert sind.
- Anzeige: **OVR** auf dem Display, in der WebUI „Override **AKTIV**“ mit rotem Hinweis, Eintrag in der Ereignisliste.
- Bleibt aktiv, bis er ausgeschaltet wird. **Ein Neustart beendet den Override.**

> ⚠ **Verantwortung beim Benutzer:** Im Override schützt nur noch das BMS des Akkus vor Tiefentladung. Wer den Override einschaltet, übernimmt die Verantwortung für Akku und Verbraucher. Override nur kurzzeitig und bewusst verwenden, z. B. in einer Notsituation, und danach wieder ausschalten.

### 7.4 Relais-Einstellungen (Einstellungen → Steuerung)

Je Relais lassen sich ändern:

| Einstellung | Bedeutung |
|---|---|
| **Sperre unter** (%) | unter diesem SOC lässt sich das Relais nicht einschalten |
| **Aus unter** (%) | unter diesem SOC wird das Relais zwangsweise ausgeschaltet |
| **Frei ab** (%) | ab diesem SOC ist die Sperre wieder aufgehoben |
| **Mindest-Auszeit** (s, 0–3600) | automatisches Wiedereinschalten frühestens nach dieser Zeit |
| **Zustand nach Neustart** | aus, oder ein (sobald der SOC es erlaubt) |
| **Nach Schutzabschaltung automatisch wieder ein** | ja: Relais schaltet wieder ein, sobald „Frei ab“ erreicht ist; nein: muss neu eingeschaltet werden |

- **Regel:** 0 ≤ *Aus unter* ≤ *Sperre unter* ≤ *Frei ab* ≤ 100. Andere Werte werden mit einer Meldung abgelehnt. Dezimalstellen (0,1 %) sind erlaubt.
- Unter jedem Feld steht die **Voreinstellung**. Oben rechts zeigt jedes Relais **„geändert“** oder **„Voreinstellung“**.
- **Speichern** übernimmt die Werte **sofort**. Liegt der SOC unter der neuen Aus-Schwelle, schaltet das Relais gleich ab. Die Werte bleiben nach einem Neustart erhalten.
- **Auf Voreinstellung** (nur bei geänderten Relais sichtbar) setzt **dieses** Relais zurück. **Alle Relais auf Voreinstellung zurücksetzen** setzt alle zurück. Beides wirkt sofort, ohne Neustart.
- **Nicht änderbar** (fest, nur angezeigt): das Verhalten ohne gültigen SOC (Wechselrichter: gesperrt und aus; Licht/Kühlschrank: Zustand bleibt), welche Relais aktiv sind, die Namen.
- Jede Änderung erscheint in der Ereignisliste (Reiter *Fehler*).

### 7.5 Anlage (Camper / Balkonkraftwerk)

- Auswahl unter *Einstellungen → Steuerung* bei **Anlage**, dann **Übernehmen und neu starten**.
- Die Anlage bestimmt Namen, aktive Relais und die Voreinstellung (Tabelle in Kapitel 1 und 7.2).
- Beim Wechsel werden **alle geänderten Relais-Einstellungen gelöscht**, es gilt die Voreinstellung der neuen Anlage. Das Gerät startet neu, **alle Relais fallen dabei kurz ab**.

### 7.6 Sicherheit

Bei Absturz, Neustart oder Stromausfall des Steuergeräts fallen **alle Relais ab** (Verbraucher aus). Nach dem Neustart gilt der „Zustand nach Neustart“ je Relais.

---

## 8. Updates

Updates werden unter **System → Update** als Datei hochgeladen (nur mit Anmeldung).

| Datei | Wirkung |
|---|---|
| `package_<Version>.tar` | **empfohlen:** Firmware + Weboberfläche, danach Neustart |
| `firmware_<Version>.bin` | nur Firmware, danach Neustart |
| `webui_<Version>.tar` | nur Weboberfläche, kein Neustart |

- Während des Updates zeigt das Display **UPD**. Gerät dabei **nicht ausschalten**.
- Nach einem Firmware-Update läuft die neue Firmware **auf Probe**. Startet sie dreimal hintereinander nicht stabil (60 s), schaltet das Gerät **automatisch auf die vorherige Firmware zurück**, zeigt unter *System → Update* einen Hinweis und trägt es in die Ereignisliste ein.
- War es ein **Paket-Update**, wird dabei auch die **vorherige Weboberfläche** wiederhergestellt, sodass Firmware und Weboberfläche wieder zusammenpassen. Deshalb ist das Paket der empfohlene Weg. „Nur Firmware“ bzw. „Nur WebUI“ nur verwenden, wenn beide aus derselben Version stammen (gleiche Versionsnummer).
- Ist die Weboberfläche während eines Updates geöffnet (z. B. Update von einem anderen Gerät aus), lädt sie sich nach dem Update selbst neu.
- Falsche Dateien werden abgelehnt (z. B. ein Paket unter „Nur WebUI“).
- Nach dem Neustart gilt der Startzustand der Relais (Kapitel 7).
- Nach einem WebUI-Update die Seite mit **Strg+F5** neu laden, damit der Browser keine alten Dateien verwendet.
- Lädt die WebUI nach einem Update nicht mehr, siehe Kapitel 10.1.

---

## 9. MQTT

Für die Hausautomatisierung (z. B. Mosquitto, Node-RED, Homematic OpenCCU). Funktioniert nur, wenn das Gerät mit dem Heim-WLAN verbunden ist.

### 9.1 Einstellungen (System → MQTT)

| Einstellung | Bedeutung | Standard |
|---|---|---|
| MQTT einschalten | Verbindung zum Broker aufbauen | aus |
| Broker | Hostname oder IP-Adresse des Brokers | – |
| Port | Port des Brokers (ohne Verschlüsselung) | 1883 |
| Benutzer | leer = ohne Anmeldung | leer |
| Passwort | leer lassen = gespeichertes behalten | leer |
| Basis-Topic | Anfang aller Topics | `vedirect` |
| Sendeintervall | Sekunden zwischen zwei Sendungen (1–3600) | 10 |
| Client-ID | leer = automatisch `ve-direct-XXXXXX` | leer |

- **Speichern und verbinden** übernimmt die Einstellungen sofort.
- **Verbindung testen** baut die Verbindung neu auf.
- **MQTT-Einstellungen löschen** setzt alles zurück.
- Der Unterreiter zeigt Zustand, letzten Fehler sowie die Anzahl gesendeter Nachrichten und empfangener Befehle.
- Relais-Einstellungen (Schwellen, Anlage) sind **nur über die WebUI** änderbar, nicht per MQTT.

### 9.2 Messwerte (das Gerät sendet)

Alle Nachrichten sind **retained** (der Broker hält den letzten Wert vor). Ungültige Werte werden als **`null`** gesendet, nie als 0. Gesendet wird im Sendeintervall, bei Änderungen an Relais, Override oder Fehlern **sofort**. In den Beispielen steht `vedirect` für das Basis-Topic.

**`vedirect/status`** – `online` / `offline`
`offline` setzt der Broker selbst („Letzter Wille“), wenn das Gerät ca. 45 s nicht erreichbar ist. Node-RED sollte diesen Wert immer mit auswerten.

**`vedirect/battery`** – Batterie
| Feld | Einheit | Bedeutung |
|---|---|---|
| `soc` | % | Ladezustand (nur vom SmartShunt) |
| `voltage` | V | Batteriespannung |
| `voltageSource` | – | `shunt`, `mppt` (Ersatzwert, Shunt fehlt) oder `none` |
| `current` | A | Batteriestrom, + laden / − entladen |
| `power` | W | Leistung |
| `consumedAh` | Ah | verbrauchte Amperestunden |
| `timeToGoMin` | min | Restlaufzeit |
| `timeToGoInfinite` | true/false | `true` = Batterie wird nicht entladen |

**`vedirect/pv`** – Solarladeregler
| Feld | Einheit | Bedeutung |
|---|---|---|
| `power` | W | PV-Leistung |
| `voltage` | V | PV-Spannung |
| `yieldTodayKWh` | kWh | Ertrag heute |
| `state` | – | Betriebszustand als Text (z. B. „Bulk (Konstantstrom)“) |
| `stateCode` | – | Victron-Code (CS) |
| `errorCode` | – | Victron-Fehlercode (ERR), 0 = kein Fehler |

**`vedirect/relays`** – Relais und Override
| Feld | Bedeutung |
|---|---|
| `r1`, `r2`, `r3` | `ON`, `OFF` oder `DISABLED` |
| `r1Name` … | Name des Relais (z. B. „Wechselrichter“) |
| `r1Locked` … | `true` = Einschalten durch SOC-Schutz gesperrt |
| `r1Wanted` … | Schaltwunsch (`true` = ein gewünscht) |
| `r1Reason` … | Grund der letzten Änderung (z. B. „SOC unter Abschaltschwelle“) |
| `override` | `ON` / `OFF` |

**`vedirect/system`** – Zustand und Fehler
| Feld | Bedeutung |
|---|---|
| `faults` | Fehlerwort als Hex-Zahl (`0x00000000` = keine Fehler) |
| `severity` | `none`, `info`, `warning`, `critical` |
| `faultTexts` | Liste der aktiven Fehler im Klartext |
| `shunt`, `mppt` | Verbindung: `ok`, `instabil`, `defekt` |
| `firmware` | Firmware-Version |
| `uptimeS` | Laufzeit seit Start in Sekunden |
| `rssi` | WLAN-Signalstärke in dBm |

**`vedirect/command-result`** – Rückmeldung zu jedem Befehl (nicht retained)
`{"command":"relay/r2-set","value":"OFF","result":"ausgeführt"}`
Mögliche Ergebnisse: „ausgeführt“, „gespeichert, wird bei ausreichendem SOC ausgeführt“, „abgelehnt: SOC zu niedrig oder kein gültiger SOC“, „abgelehnt: Relais deaktiviert“, „abgelehnt: Befehlsquelle nicht erlaubt“ (derzeit sind alle Quellen erlaubt), „abgelehnt: Wert muss ON oder OFF sein“.

### 9.3 Schaltbefehle (an das Gerät senden)

| Topic | Payload | Wirkung |
|---|---|---|
| `vedirect/relay/r1-set` | `ON` / `OFF` | R1 Wechselrichter (Camper) bzw. DC-DC-Wandler (Balkonkraftwerk) |
| `vedirect/relay/r2-set` | `ON` / `OFF` | R2 Licht |
| `vedirect/relay/r3-set` | `ON` / `OFF` | R3 Kühlschrank |
| `vedirect/override-set` | `ON` / `OFF` | Override ein/aus |

- Erlaubt sind auch `1`/`0`, `true`/`false`, `EIN`/`AUS`; Groß- und Kleinschreibung egal.
- Der **SOC-Schutz gilt immer** (außer im Override). Ein abgelehnter Befehl wird in `command-result` gemeldet.
- **Befehle ohne „Retain“ senden.** Ein retained Befehl wird nach jedem Neustart des Geräts erneut ausgeführt (z. B. würde der Wechselrichter dann wieder einschalten).
- Bricht die MQTT-Verbindung ab, behalten die Relais ihren Zustand. Der SOC-Schutz arbeitet lokal weiter.

### 9.4 Beispiel: Befehl mit MQTT.fx senden

1. Zahnrad → Verbindungsprofil anlegen: Broker-Adresse und Port 1883 → OK.
2. Profil wählen → **Connect** (grüner Punkt).
3. Reiter **Publish**: oben Topic `vedirect/relay/r2-set`, im großen Feld darunter `ON`, QoS 0, Retained aus → **Publish** (rechts neben dem Topic).
4. Zum Mitlesen im Reiter **Subscribe** `vedirect/#` abonnieren.

---

## 10. Fehlerbehebung

| Problem | Ursache / Abhilfe |
|---|---|
| WebUI nicht erreichbar unter `ve-direct.local` | IP-Adresse aus *System → Info* bzw. vom Router verwenden; manche Handys unterstützen `.local` nicht |
| WebUI lädt, aber Reiter bleiben leer (auch nach Strg+F5) | WebUI-Dateien defekt oder unvollständig → Kapitel 10.1 |
| WLAN verbindet nicht | Meldung in der WebUI beachten; nur 2,4 GHz; Passwort prüfen |
| Display zeigt Shunt ⊗ | SmartShunt fehlt/ausgefallen: Kabel und Stecker prüfen. Wechselrichter wird dann abgeschaltet |
| Display zeigt ◐ (instabil) | Übertragungsfehler oder kurzer Ausfall: Stecker/Crimpverbindung prüfen; Anzeige erlischt nach 2 min ohne Fehler |
| MQTT verbindet nicht | *System → MQTT*: letzter Fehler (Adresse/Port, Benutzer/Passwort) |
| MQTT-Befehl wirkt nicht | Topic exakt prüfen (`relay` Einzahl, `-set`), Payload im Nachrichtenfeld, gleicher Broker; Zähler „Befehle empfangen“ unter *System → MQTT* |
| Hinweis „vorherige Firmware“ unter *System → Update* | Die neue Firmware lief nicht stabil, das Gerät hat automatisch zurückgeschaltet |
| Relais lässt sich nicht einschalten | Rückmeldung bzw. Hinweis unter dem Relais beachten: SOC unter „Sperre unter“/„Frei ab“ noch nicht erreicht, oder kein gültiger SOC (Shunt fehlt) |
| Relais schaltet unerwartet ab | Reiter *Fehler* → Ereignisliste zeigt den Grund (z. B. „SOC unter Abschaltschwelle“) |
| Historie/Geräteeinstellungen: „antwortet nicht auf HEX-Anfragen“ | Die Leitung vom Steuergerät **zum** Victron-Gerät (TX) fehlt oder ist vertauscht; Live-Werte kommen trotzdem, weil sie nur in die andere Richtung laufen. Kabel prüfen. Simulatoren, die nur Text senden, antworten nie. |
| Passwort vergessen | Reset-Taste (Kapitel 5) |
| Uhrzeit „nicht gestellt“ oder falsch | *System → Gerät → Uhr*: Zeitzone prüfen; ohne Internet: Weboberfläche einmal öffnen (Browserzeit wird übernommen, wenn keine gültige Zeit vorhanden ist); Knopfzelle CR1220 einsetzen, damit die Zeit Stromausfälle übersteht |
| Uhr: „Echtzeituhr nicht gefunden“ | Verkabelung der RTC prüfen (SDA GPIO 21, SCL GPIO 22, 3,3 V, GND); Zeit kommt dann nur per Internet bzw. Browser |

### 10.1 Notweg: WebUI per USB neu aufspielen

Bleibt die Weboberfläche leer, kann man auch kein Update mehr hochladen. Die Steuerung (Relais, Schutz, MQTT) läuft davon unabhängig weiter. Abhilfe (PC mit VS Code und PlatformIO, Projektordner geöffnet):

1. ESP32 per USB anschließen.
2. **Alle seriellen Monitore schließen** (VS Code: Monitor-Terminal mit dem Mülleimer-Symbol beenden; andere Programme wie Arduino IDE, PuTTY, HTerm schließen). Sonst meldet der Upload „Could not open COMx, the port doesn't exist“, obwohl der Port vorhanden ist.
3. Links das **PlatformIO-Symbol** → **PROJECT TASKS** → **esp32dev** → **Platform** → **Upload Filesystem Image**.
4. Warten bis **SUCCESS**, dann im Browser **Strg+F5**.

Einstellungen (WLAN, Passwort, MQTT, Relais, Anlage) bleiben dabei erhalten; sie liegen in einem eigenen Speicherbereich. Hilft das nicht oder bricht der Upload mit „Failed to connect“ ab: beim Start des Uploads die **BOOT-Taste** am ESP32 gedrückt halten.

---

## 11. Anhang

### 11.1 Technische Daten
*folgt* (Versorgung, Anschlüsse, Pinbelegung siehe `Claude.md`)

**Echtzeituhr:** Adafruit PCF8523 (ADA3295), I²C-Adresse 0x68, Versorgung 3,3 V. Anschluss: VIN → 3,3 V, GND → GND, SDA → GPIO 21, SCL → GPIO 22 (SQW frei). Knopfzelle CR1220. Ein Umstieg auf eine genauere Uhr (z. B. DS3231) ist in der Software vorbereitet.

### 11.2 Fehlerwort (`faults`)
| Bit | Wert | Fehler | Schwere |
|---|---|---|---|
| 0 | `0x001` | Batteriemonitor (Shunt) fehlt | kritisch |
| 1 | `0x002` | Mehrere Batteriemonitore | kritisch |
| 2 | `0x004` | SOC nicht verfügbar | kritisch |
| 3 | `0x008` | Batteriealarm | Warnung |
| 4 | `0x010` | Verbindung Shunt instabil | Warnung |
| 5 | `0x020` | MPPT fehlt | Info |
| 6 | `0x040` | Mehrere MPPT | Info |
| 7 | `0x080` | MPPT-Fehler | Warnung |
| 8 | `0x100` | Verbindung MPPT instabil | Warnung |
| 9 | `0x200` | Unbekanntes Gerät | Info |

### 11.3 Serieller Monitor – nur für Entwickler

> ⚠ **Nur für Entwicklung und Fehlersuche.** Diese Befehle sind nicht Teil der normalen Bedienung. Sie wirken direkt auf das Gerät, auch auf die Relais. Für den Betrieb die Weboberfläche oder MQTT verwenden.

**Verbindung:** ESP32 per USB an den PC, serieller Monitor mit **115200 Baud** (VS Code: PlatformIO → *Monitor*). Befehl eintippen und mit Enter abschicken. Groß-/Kleinschreibung ist egal. Unbekannte Eingaben zeigen die Hilfe.

**Langzeitlauf ohne Zuschauer:** Ausgabe mit Uhrzeit in eine Datei schreiben (PlatformIO-Terminal):
```
pio device monitor -f time -f log2file
```
Die Logdatei landet im Projektordner und enthält auch die letzten Zeilen vor einem Absturz.

Alle 5 s erscheinen Statuszeilen: `[STATUS]` je Anschluss (Gerät, Zähler, Verbindung), `[SYSTEM]` (Batterie, PV, Fehlerwort), `[RELAIS]`, `[WLAN]`. Ereignisse (Gerät erkannt/verloren, Befehle, Fehler) erscheinen sofort.

#### Relais und Override

| Befehl | Wirkung |
|---|---|
| `r1 ein` / `r1 aus` | Relais R1 ein/aus (ebenso `r2`, `r3`; statt `ein` auch `an`/`on`, statt `aus` auch `off`) |
| `override ein` / `override aus` | Override ein/aus (hebt alle Schutzsperren auf, siehe Kapitel 7.3) |

Befehle aus dem Monitor zählen als Quelle **„lokal“** (wie später Taster/Touch). Es gelten dieselben Regeln wie bei WebUI und MQTT (SOC-Schutz, Rückmeldung `[BEFEHL] …`).

#### Anzeige

| Befehl | Wirkung |
|---|---|
| `bloecke ein` / `bloecke aus` | jeden empfangenen VE.Direct-Textblock vollständig ausgeben (Klarname + Rohwert). Standard: aus |
| `speicher` | Arbeitsspeicher (frei, kleinster Wert seit Start, größter Block) und Stack-Reserve je Task; unter ca. 1 KB Reserve wird „KNAPP“ markiert |
| `hilfe` | Befehlsübersicht |

#### HEX-Protokoll (W4b, nur lesend)

Das Gerät kann damit **nur lesen**. Befehle zum Verstellen der Victron-Geräte gibt es nicht. `1` = UART1, `2` = UART2, ohne Angabe = beide Anschlüsse.

| Befehl | Wirkung |
|---|---|
| `hex` | Übersicht der HEX-Befehle |
| `hex ping [1\|2]` | Lebenszeichen; Antwort mit Firmware-Version des Geräts. Prüft auch, ob die TX-Leitung funktioniert |
| `hex version [1\|2]` | Firmware-Version |
| `hex pid [1\|2]` | Produkt-ID (z. B. `0xC035` = SmartShunt 500A/50mV) |
| `hex get <Register> [1\|2]` | ein Register lesen, Register als Hex-Zahl, z. B. `hex get 1000 1` |
| `hex lesen [1\|2]` | automatische Abfrage neu starten (Shunt: Einstellungen; MPPT: Einstellungen + Historie) |
| `hex cache [1\|2]` | zuletzt gelesene Werte anzeigen (Register, Zustand, Alter, Rohwert) |
| `hex async ein` / `hex async aus` | ungefragte Meldungen des MPPT (`:A…`) anzeigen bzw. ausblenden. **Standard: aus.** Der MPPT (ab FW 1.53) sendet nach jeder HEX-Anfrage 2 min lang solche Meldungen, z. B. sekündlich die Ladespannung (0xEDD5) – normal und harmlos |

Ausgabe einer Anfrage, z. B.:
```
[UART1] HEX -> :154; letzter Textblock vor 201 ms
[UART1] HEX <- Ping: Firmware 4.20 (Anwendung); 8 ms nach der Anfrage; letzter Textblock vor 209 ms
```
„letzter Textblock vor … ms“ zeigt, ob die Textblöcke während HEX-Verkehr weiterlaufen (unter ca. 1,5 s = normal).

Nützliche Register (Werte als Rohzahl, Umrechnung in Klammern):

| Gerät | Register | Bedeutung |
|---|---|---|
| SmartShunt | `1000` | Batteriekapazität (Ah) |
| SmartShunt | `1001` | Ladespannung (0,1 V, z. B. 140 = 14,0 V) |
| SmartShunt | `1002` | Schweifstrom (0,1 %) |
| SmartShunt | `1003` | Ladeerkennungszeit (min) |
| SmartShunt | `1004` | Ladewirkungsgrad (%) |
| SmartShunt | `1005` | Peukert-Exponent (0,01, z. B. 125 = 1,25) |
| SmartShunt | `1006` | Strom-Schwellwert (0,01 A) |
| SmartShunt | `1007` | Durchschnittliche Restlaufzeit (min) |
| SmartShunt | `1008` | Entladeboden (0,1 %) |
| MPPT | `edf0` | max. Ladestrom (0,1 A) |
| MPPT | `edf7` / `edf6` | Absorptions- / Erhaltungsspannung (0,01 V) |
| MPPT | `104f` | Historie gesamt |
| MPPT | `1050` … `106e` | Historie heute … vor 30 Tagen |

Antwort `Flags 01 [Register unbekannt]` = das Gerät kennt das Register nicht.

#### Echtzeituhr (W4e)

| Befehl | Wirkung |
|---|---|
| `uhr` | Systemzeit (Ortszeit), Quelle (RTC/NTP/Browser), Zeitzone, NTP-Server; dazu RTC-Zeit (UTC), Unix-Zeit, Zustand der Knopfzelle. Sucht die RTC erneut, falls sie beim Start fehlte |
| `uhr stellen JJJJ-MM-TT hh:mm:ss` | RTC stellen, **in UTC** (MEZ = UTC + 1 h, MESZ = UTC + 2 h), z. B. `uhr stellen 2026-11-11 16:45:00` |

Normalerweise stellt sich die Uhr selbst (NTP bzw. Browser, folgt). `uhr stellen` ist nur zum Testen gedacht.

#### Testfunktion (wird vor der Fertigstellung entfernt)

| Befehl | Wirkung |
|---|---|
| `absturz` | löst **absichtlich einen Absturz** aus. Das Gerät startet neu, **alle Relais fallen ab**. Dient zum Testen des Neustart-Grunds (Reiter *Fehler*) und des automatischen Rückfalls nach einem Update (dreimal innerhalb von 60 s nach dem Start). |
