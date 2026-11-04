Victron Energy Manager

# Projektphilosophie

* Von innen nach außen entwickeln
* Jeder Meilenstein endet mit einem lauffähigen System
* Erst testen, dann erweitern
* Eine Klasse = eine Verantwortung
* Fachlogik kennt keine Hardware
* Hardware kennt keine Fachlogik
* Entscheidungen werden dokumentiert

---

# Roadmap

[M1] Hardware              ✔
[M2] Kommunikation         ✔
[M3] Parser                ✔
[M4] DeviceCatalog         ✔
[M5] SystemState           ▶
[M6] Diagnostics           ⏳
[M7] RuleEngine            ⏳
[M8] MQTT                  ⏳
[M9] WebUI                 ⏳

---

# Definition of Done (M3)

✔ Telegramm vollständig erkannt
✔ Telegramm in Label/Value zerlegt
✔ Parser kennt keine Geräte
✔ Parser kennt keine Semantik
✔ Parser kennt keinen UART
✔ Parser ist ohne ESP32 testbar

---

# Definition of Done (M4)

✔ Product-ID erkannt
✔ Gerät anhand der Product-ID identifiziert
✔ DeviceCatalog implementiert
✔ DeviceInterpreter implementiert
✔ SmartSolar MPPT unterstützt
✔ SmartShunt unterstützt
✔ Historienfelder interpretiert
✔ OffReason dekodiert
✔ Menschenlesbare Konsolenausgabe vorhanden
✔ Parser bleibt geräteunabhängig

---

# Nächster Meilenstein (M5)

Ziel:

Gemeinsamen Anlagenzustand schaffen, der unabhängig vom konkreten Gerätetyp genutzt werden kann.

Erste Aufgaben:

* SystemState definieren
* StateMerger implementieren
* Gemeinsame Batteriewerte bereitstellen
* Gemeinsame PV-Werte bereitstellen
* Geräteunabhängige Datenbasis schaffen
