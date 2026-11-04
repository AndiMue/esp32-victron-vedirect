# Architektur

## Verantwortlichkeiten

| Klasse               | Aufgabe                                                       | Darf NICHT               |
| -------------------- | ------------------------------------------------------------- | ------------------------ |
| `VeDirectReader`     | VE.Direct-Bytes vom UART empfangen                            | Parser kennen            |
| `VeDirectTextParser` | Telegramm in Felder zerlegen                                  | UART verwenden           |
| `VeDirectFrame`      | Ein vollständiges VE.Direct-Telegramm repräsentieren          | Entscheidungen treffen   |
| `FieldCatalog`       | VE.Direct-Felder beschreiben (Name, Typ, Einheit, Skalierung) | Daten speichern          |
| `ValueConverter`     | Rohdaten in physikalische Werte umwandeln                     | UART oder Parser kennen  |
| `FieldInterpreter`   | Felder menschenlesbar darstellen                              | Umrechnungen durchführen |
| `DeviceCatalog`      | Geräte anhand der Product-ID identifizieren                   | Zustände speichern       |
| `DeviceInterpreter`  | Geräteinformationen menschenlesbar darstellen                 | Gerätelogik enthalten    |
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
  ├─────────────► DeviceCatalog
  │                    │
  │                    ▼
  │              DeviceInterpreter
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
* DeviceCatalog
* DeviceInterpreter
* Product-ID-Erkennung
* SmartSolar-Unterstützung
* SmartShunt-Unterstützung
* Menschenlesbare Konsolenausgabe

---

## Geplante Ausbaustufen

### M5 – SystemState

* Gemeinsamer Anlagenzustand
* Entkopplung von Geräteprotokollen
* Zentrale Datenbasis

### M6 – Diagnostics

* Timeouts
* Kommunikationsfehler
* Checksum-Prüfung
* Ereignisspeicher

### M7 – RuleEngine

* SOC-basierte Entscheidungen
* Lastabwurf
* Nachteinspeisung sperren
* Camper-Verbrauch reduzieren

### M8 – MQTT / Visualisierung

* MQTT Publisher
* Home Assistant
* Dashboard

---

## Spätere Erweiterungen

### VE.Direct Completion

* Vollständige Product-ID-Tabelle
* Vollständige Charger-State-Dekodierung
* Vollständige MPPT-State-Dekodierung
* Vollständige Error-Code-Dekodierung
* Vollständige Off-Reason-Dekodierung
* Hex-Protokoll-Unterstützung
* Erweiterte SmartShunt-Felder
