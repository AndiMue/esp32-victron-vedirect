# Architekturentscheidungen

## 2025-08

### SmartShunt als führende Quelle

Der SmartShunt ist die führende Quelle für:

- SOC
- Batteriespannung
- Batteriestrom

Begründung:

Der SmartShunt verbleibt dauerhaft am Akku.
MPPT-Regler können zwischen verschiedenen Anlagen wechseln.

---

### Schichtenarchitektur

Jede Klasse besitzt genau eine Verantwortung.

Beispiele:

- Reader liest UART
- Parser zerlegt Telegramme
- Converter wandelt Werte um
- RuleEngine trifft Entscheidungen

---

### Entwicklungsstrategie

Von innen nach außen entwickeln.

Reihenfolge:

1. Hardware
2. Reader
3. Parser
4. Interpretation
5. SystemState
6. Diagnostics
7. RuleEngine
8. MQTT 