# Architekturentscheidungen

## 2025-08

### Schichtenarchitektur

**Entscheidung**

Jede Klasse besitzt genau eine Verantwortung.

**Beispiele**

* Reader liest UART
* Parser zerlegt Telegramme
* Converter wandelt Werte um
* Interpreter stellt Daten dar
* RuleEngine trifft Entscheidungen

**Begründung**

Klare Verantwortlichkeiten vereinfachen Tests, Erweiterungen und Fehlersuche.

---

### Entwicklungsstrategie

**Entscheidung**

Von innen nach außen entwickeln.

**Reihenfolge**

1. Hardware
2. Reader
3. Parser
4. Interpretation
5. DeviceCatalog
6. SystemState
7. Diagnostics
8. RuleEngine
9. MQTT

**Begründung**

Jeder Meilenstein endet mit einem lauffähigen und testbaren System.

---

### Geräteerkennung über Product-ID

**Entscheidung**

Geräte werden über die VE.Direct Product-ID erkannt.

**Begründung**

Die Product-ID ist Bestandteil des VE.Direct-Protokolls und unabhängig von den aktuell übertragenen Messwerten.

**Umsetzung**

* DeviceCatalog
* DeviceInterpreter

---

### SmartShunt als führende Batteriequelle

**Entscheidung**

Der SmartShunt ist die führende Quelle für:

* Ladezustand (SOC)
* Batteriespannung
* Batteriestrom

**Begründung**

Der SmartShunt misst direkt am Akku und bleibt dauerhaft Bestandteil des Systems.

MPPT-Regler können ausgetauscht oder in anderen Anlagen verwendet werden und sind deshalb nicht die primäre Quelle für Batteriedaten.

---

### Geräteunabhängige Verarbeitung

**Entscheidung**

Reader, Parser und Frame kennen keine konkreten Gerätetypen.

**Begründung**

Neue VE.Direct-Geräte sollen ohne Änderungen an der Kommunikationsschicht integriert werden können.

Gerätespezifisches Wissen wird ausschließlich in:

* DeviceCatalog
* FieldCatalog
* Interpreter-Klassen

abgebildet.

---

### Menschenlesbare Interpretation

**Entscheidung**

VE.Direct-Kürzel bleiben in der Ausgabe sichtbar.

**Beispiel**

```text
[H2] Last Discharge : 12.4 Ah
[SOC] State Of Charge : 84.2 %
```

**Begründung**

Die Ausgabe bleibt mit der offiziellen Victron-Dokumentation vergleichbar und erleichtert Debugging sowie Erweiterungen.
