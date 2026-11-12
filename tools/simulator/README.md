# Aufzeichnungen für die Simulatoren (SmartShunt / MPPT)

Idee (Projektinhaber 12.11.2026): echte Geräte aufzeichnen und im Simulator
**abspielen** (Textprotokoll) bzw. auf Anfrage des ESP32 die **echten
HEX-Antworten** senden.

## Aufzeichnen

Serieller Monitor muss geschlossen sein. Es werden nur lesende Befehle benutzt.

```
python tools/simulator/record_devices.py COM17 180
```

`COM17` = Port des Steuergeräts, `180` = Sekunden Textaufzeichnung.
Ergebnis: `aufzeichnung_JJJJ-MM-TT_hhmm.json` in diesem Ordner.

Tipp: zu verschiedenen Zeiten aufzeichnen (Tag mit PV-Leistung, Nacht,
Entladung, Laden), damit der Simulator unterschiedliche Lagen abspielen kann.

## Dateiformat (`vedirect-recording`, Version 1)

```
{
  "format": "vedirect-recording", "version": 1, "created": "...",
  "devices": {
    "shunt": { ... }, "mppt": { ... }
  }
}
```

Je Gerät:

| Feld | Inhalt |
|---|---|
| `uart`, `pid` | Anschluss am Steuergerät, Produkt-ID |
| `textFrames` | Textblöcke in Empfangsreihenfolge: `t` (Laufzeit des Steuergeräts in s, für den Takt), `header`, `fields` = Liste `[Label, Wert]`. **Ohne `Checksum`** – der Simulator berechnet sie neu (Summe aller Bytes des Blocks inkl. `\r\n`, Tabs und Prüfbyte = 0 mod 256). Der SmartShunt sendet 2 Blöcke pro Sekunde (der zweite ohne PID mit H1…H18). |
| `hex.ping`, `hex.appVersion`, `hex.productId` | Antwort auf `:154`, `:352`, `:451`: `raw` und fertige Zeile `line` |
| `hex.registers` | je Register `"0x1000"`: `flags`, `value` (Hex-Bytes) und fertige Antwortzeile `line` (`:7…` inkl. Prüfsumme, ohne `\n`). Flags `0x01` = Register unbekannt – genau so antworten. |
| `async` | mitgeschnittene ungefragte Meldungen (`:A…`) mit Zeit; der echte MPPT (FW ≥ 1.53) sendet sie nach jeder HEX-Anfrage 2 min lang |

Seriennummern sind anonymisiert (`HQ00000TEST`).

Alle Zeilen enden beim Senden mit `\n`. Prüfsumme der HEX-Zeilen:
Befehl + alle Datenbytes + Prüfbyte = 0x55 (mod 256).
