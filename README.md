# Tibber-Strompreisdisplay (Waveshare ESP32-S3 PhotoPainter)

Standalone-Firmware für den Waveshare ESP32-S3 PhotoPainter mit
6-Farben-E-Paper-Display (E Ink Spectra 6, 800×480). Zeigt aktuelle
Tibber-Strompreise inkl. 24-48h-Diagramm, holt die Preise direkt über
die Tibber-API. Ohne gespeicherte WLAN-Zugangsdaten startet das Gerät
einen Setup-Access-Point mit Web-Portal.

## Details
Siehe [docs/HANDOFF.md](docs/HANDOFF.md) für Hardware-Spezifika,
bereits gelöste Probleme und offene Punkte.

## Build
- Framework: Arduino über PlatformIO
- Board: `esp32-s3-devkitc-1` (Waveshare ESP32-S3 PhotoPainter)
- `pio run` zum Bauen, `pio run -t upload` zum Flashen über USB

Alternativ: Browser-Compile + WebSerial-Flash über
[schematik.io](https://schematik.io).

## Konfiguration
WLAN und Tibber Personal Access Token werden **nicht** im Code
hinterlegt, sondern zur Laufzeit über das Setup-Web-Portal eingegeben
und in NVS (`Preferences`) gespeichert. Werksreset: KEY-Taste 15
Sekunden halten.
