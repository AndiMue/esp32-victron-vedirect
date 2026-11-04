# Devices

## Unterstützte Geräteklassen

| Kategorie       | Geräte                     |
| --------------- | -------------------------- |
| SmartSolar MPPT | SmartSolar MPPT Laderegler |
| BlueSolar MPPT  | BlueSolar MPPT Laderegler  |
| Battery Monitor | BMV-Serie, SmartShunt      |
| DC-DC Charger   | Orion XS                   |

---

## Bekannte Product IDs

### SmartSolar MPPT

| Product ID | Gerät                        |
| ---------- | ---------------------------- |
| 0xA050     | SmartSolar MPPT 250/100      |
| 0xA051     | SmartSolar MPPT 150/100      |
| 0xA052     | SmartSolar MPPT 150/85       |
| 0xA053     | SmartSolar MPPT 75/15        |
| 0xA054     | SmartSolar MPPT 75/10        |
| 0xA055     | SmartSolar MPPT 100/15       |
| 0xA056     | SmartSolar MPPT 100/30       |
| 0xA057     | SmartSolar MPPT 100/50       |
| 0xA058     | SmartSolar MPPT 150/35       |
| 0xA059     | SmartSolar MPPT 100/100 rev2 |

### BlueSolar MPPT

| Product ID | Gerät                 |
| ---------- | --------------------- |
| 0xA040     | BlueSolar MPPT 75/50  |
| 0xA041     | BlueSolar MPPT 150/35 |
| 0xA042     | BlueSolar MPPT 75/15  |
| 0xA043     | BlueSolar MPPT 100/15 |
| 0xA044     | BlueSolar MPPT 100/30 |
| 0xA045     | BlueSolar MPPT 100/50 |

### BMV-Serie

| Product ID | Gerät              |
| ---------- | ------------------ |
| 0x203      | BMV-700            |
| 0x204      | BMV-702            |
| 0x205      | BMV-700H           |
| 0x381      | BMV-712 Smart      |
| 0x382      | BMV-710H Smart     |
| 0x383      | BMV-712 Smart Rev2 |

### SmartShunt

| Product ID | Gerät                                                |
| ---------- | ---------------------------------------------------- |
| 0x389      | SmartShunt 500A/50mV                                 |
| 0x38A      | SmartShunt 1000A/50mV                                |
| 0x38B      | SmartShunt 2000A/50mV                                |
| 0xC035     | SmartShunt 500A/50mV (beobachtete VE.Direct-Ausgabe) |

### Orion XS

| Product ID | Gerät                |
| ---------- | -------------------- |
| 0x3F0      | Orion XS 12V/12V-50A |
| 0x3F1      | Orion XS 1400        |

---

## Aktuell getestet

| Gerät                  | Status     |
| ---------------------- | ---------- |
| SmartSolar MPPT 100/50 | ✔ getestet |
| SmartShunt 500A/50mV   | ✔ getestet |

---

## Hinweise

Die Product-ID-Erkennung erfolgt über das VE.Direct-Feld `PID`.

Nicht jede Product-ID wurde bislang physisch getestet. Nicht getestete Geräte basieren auf den offiziellen Victron-Dokumentationen.
