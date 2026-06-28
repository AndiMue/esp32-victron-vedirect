| Klasse            | Aufgabe                                                 | Darf NICHT             |
| ----------------- | ------------------------------------------------------- | ---------------------- |
| `VeDirectReader`  | VE.Direct-Bytes vom UART empfangen                      | Parser kennen          |
| `VeDirectParser`  | Telegramm in Felder zerlegen                            | UART verwenden         |
| `DeviceCatalog`   | Geräte und semantische Felder kennen                    | Daten speichern        |
| `DeviceFrame`     | Ein vollständiges Telegramm eines Geräts repräsentieren | Entscheidungen treffen |
| `StateMerger`     | Datenquellen zum `SystemState` zusammenführen           | UART lesen             |
| `SystemState`     | Aktuellen Anlagenzustand bereitstellen                  | Geräte kennen          |
| `Diagnostics`     | Status, Warnungen und Fehler verwalten                  | Relais schalten        |
| `RuleEngine`      | Entscheidungen treffen                                  | MQTT oder UART kennen  |
| `SerialPublisher` | Daten seriell ausgeben                                  | Logik enthalten        |
| `MqttPublisher`   | Daten per MQTT veröffentlichen                          | Entscheidungen treffen |
