# Architektur

## Verantwortlichkeiten

| Klasse               | Aufgabe                                                       | Darf NICHT               |
| -------------------- | ------------------------------------------------------------- | ------------------------ |
| `VeDirectReader`     | VE.Direct-Bytes vom UART empfangen                            | Parser kennen            |
| `VeDirectTextParser` | Telegramm in Felder zerlegen                                  | UART verwenden           |
| `VeDirectFrame`      | Ein vollständiges VE.Direct-Telegramm repräsentieren          | Entscheidungen treffen   |
| `FieldCatalog`       | VE.Direct-Felder beschreiben (Name, Typ, Einheit, Skalierung) | Daten speichern          |
| `ValueConverter`     | Rohdaten in physikalische Werte umwandeln                     | UART oder Parser kennen  |
| `FieldInterpreter`   | Daten menschenlesbar darstellen                               | Umrechnungen durchführen |
| `DeviceCatalog`      | Geräte anhand der Product-ID identifizieren                   | Zustände speichern       |
| `StateMerger`        | Datenquellen zum `SystemState` zusammenführen                 | UART lesen               |
| `SystemState`        | Aktuellen Anlagenzustand bereitstellen                        | Geräte kennen            |
| `Diagnostics`        | Status, Warnungen und Fehler verwalten                        | Relais schalten          |
| `RuleEngine`         | Entscheidungen treffen                                        | MQTT oder UART kennen    |
| `SerialPublisher`    | Daten seriell ausgeben                                        | Logik enthalten          |
| `MqttPublisher`      | Daten per MQTT veröffentlichen                                | Entscheidungen treffen   |

---

## Aktuelle Verarbeitungskette

```text
UART
  │
  ▼
VeDirectReader
  │
  ▼
VeDirectTextParser
  │
  ▼
VeDirectFrame
  │
  ▼
FieldCatalog
  │
  ▼
ValueConverter
  │
  ▼
FieldInterpreter
  │
  ▼
Serial Monitor
```

---

## Bereits umgesetzt

* UART-Kommunikation mit VE.Direct
* Stabiler Telegrammempfang
* Parser für Text-Frames
* FieldCatalog
* ValueConverter
* OffReason-Dekodierung
* Menschenlesbare Konsolenausgabe

---

## Geplante Ausbaustufen

### M5 – DeviceCatalog

* Product-ID erkennen
* Gerätetyp bestimmen
* MPPT, SmartShunt, Orion unterscheiden

### M6 – SystemState

* Gemeinsamer Anlagenzustand
* Entkopplung von Geräteprotokollen
* Zentrale Datenbasis

### M7 – Diagnostics

* Timeouts
* Kommunikationsfehler
* Checksum-Prüfung
* Ereignisspeicher

### M8 – RuleEngine

* SOC-basierte Entscheidungen
* Lastabwurf
* Nachteinspeisung sperren
* Camper-Verbrauch reduzieren

### M9 – MQTT / Visualisierung

* MQTT Publisher
* Home Assistant
* Dashboard

```
```
