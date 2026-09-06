# Projekt-Übergabe: Tibber-Strompreisdisplay (Waveshare ESP32-S3 PhotoPainter)

Dies ist die aktualisierte Übergabe (Stand: Nachmittag). Ein früherer
Handoff-Versuch von heute Vormittag ist überholt - dieses Dokument ersetzt
ihn vollständig.

## Ziel
Standalone-Firmware (ohne Home Assistant) für den Waveshare ESP32-S3
PhotoPainter mit 6-Farben-E-Paper-Display (E Ink Spectra 6 "E6", 800×480,
GxEPD2, angesteuert über die `GxEPD2_7C`-Templateklasse - der Name "7C"
ist generisch, das Panel selbst hat 6 Farben). Zeigt aktuelle
Tibber-Strompreise, holt Preise direkt über die Tibber-API. Bietet bei
fehlender WLAN-Konfiguration einen Setup-Access-Point mit Web-Portal.

Entwicklung lief bisher über Schematik.io (Browser-Compile +
WebSerial-Flash). Die beiliegende `main.cpp` ist der vollständige,
aktuelle Stand nach einem sehr langen Debugging- und Redesign-Tag.

## Setup
- Framework: Arduino über PlatformIO
- Board: `esp32-s3-devkitc-1`
- Bibliotheken: ArduinoJson ^7, GxEPD2, QRCode, DNSServer, Adafruit GFX
  Library, HTTPClient, Preferences, SPI, WebServer, WiFi,
  NetworkClientSecure
- `main.cpp` liegt bei - das ist der vollständige aktuelle Stand.

## Hardware-Spezifika (bitte nicht "wegoptimieren")

- **PMIC AXP2101** an I²C, Adresse `0x34`, SDA=`GPIO47`, SCL=`GPIO48`.
  Muss vor Display-Nutzung ALDO1-4 auf 3,3 V setzen und aktivieren
  (Register `0x92`-`0x95` Spannung, `0x90` Enable). Register `0x00`
  dient nur der "ist der Chip da"-Diagnose, `0x01` liefert den
  Ladestatus, `0x34`/`0x35` die Batteriespannung (14-Bit-Rohwert, 1 LSB
  = 1 mV).
- **GPIO5 = `SYS_OUT_LATCH`**, interne Leitung, nicht extern verdrahtet.
  Muss als *allererste* GPIO-Aktion in `setup()` auf `HIGH` gesetzt
  werden, sonst kappt der AXP2101 die Stromversorgung wieder.
- **E-Paper-Panel:** `GxEPD2_730c_GDEP073E01`-Treiber ist korrekt und
  vom Hersteller (GooDisplay) bestätigt für dieses Spectra-6-Panel -
  kein Treiber-Mismatch trotz früherem Verdacht.
- **Framebuffer-Größe:** `GxEPD2_7C<..., HEIGHT / 2>` - bewusst auf die
  Hälfte reduziert (zwei Redraw-Durchgänge statt einem), weil der volle
  Puffer (~192 KB) zeitweise für einen Heap-Engpass beim WLAN-AP-Start
  verantwortlich war (siehe "Gelöste Probleme"). Aktuell 22 % RAM-
  Auslastung, viel Spielraum.

## Bereits gelöste Probleme (bitte nicht erneut vorschlagen)

1. Namenskollision `NetworkClient` → `TibberConnection` umbenannt.
2. `JsonArray::empty()` (ArduinoJson 7) → `size() == 0`.
3. Fehlende Panelstromversorgung → GPIO5-Latch + AXP2101-Init ergänzt.
4. **Stack-Overflow:** `Storage`/`DisplayDriver` waren lokale Objekte
   in `setup()` → beide sind jetzt globale Objekte (siehe Kommentar im
   Code direkt über ihrer Deklaration).
5. **WLAN-Setup-AP nahm keine Client-Verbindungen an:** Ursache war zu
   wenig freier interner RAM (nur ~10 KB) durch den vollen 192-KB-
   Framebuffer, der schon beim Programmstart reserviert wird (globales
   Objekt). Gelöst durch Reduktion auf `HEIGHT / 2`.
6. **Preis-Cache `NOT_ENOUGH_SPACE` beim Speichern in NVS:** Ursache
   war die schiere Datengröße (`today` + `tomorrow` zusammen), nicht
   Fragmentierung. Gelöst durch: nur `today` wird dauerhaft gecacht,
   `tomorrow` nur für die aktuelle Anzeige verwendet, nicht gespeichert.
7. **Wartungsmodus-Taste hatte keine Wirkung:** `heldForFactoryReset()`
   und eine zweite Funktion für den Toggle konkurrierten um denselben
   Tastendruck. Gelöst durch eine einzige kombinierte Prüfung
   (`readKeyHold()` mit `KeyHoldResult`-Enum).
8. **Wartungsmodus verlor sich beim Reconnect:** Ursache war ein
   gemeinsamer NVS-Namespace (`"tibber"`) zwischen Wartungsmodus-Flag
   und Preis-Cache - ein `p.clear()`-Rettungsversuch beim Cache-Fehler
   löschte versehentlich auch den Wartungsmodus-Status mit. Gelöst
   durch separaten Namespace `"settings"` für den Wartungsmodus.
9. **QR-Code im Setup-Bildschirm teilweise unlesbar:** Ein sichtbarer
   Bildfehler (braune statt schwarze Pixel) in einer Ecke des QR-Codes
   führte zu Scan-Fehlern. Vermutlich Folge des unten beschriebenen,
   weiterhin ungelösten Busy-Timeout-Problems. Als Sofortmaßnahme wurde
   die QR-Fehlerkorrektur von `ECC_LOW` auf `ECC_MEDIUM` erhöht -
   seitdem funktioniert der QR-Scan.
10. **Setup-Formular hatte keine Wirkung:** Beim iOS-Redesign der
    Setup-Seite wurde versehentlich das komplette `<form>`-Element
    weggelassen. Behoben, `<form method='post' action='/save'>` umgibt
    jetzt die Eingabefelder.
11. **Wiederkehrender Vollrefresh im unkonfigurierten Zustand:** Das
    Gerät wachte im Setup-Modus automatisch alle ~30 Minuten per Timer
    auf und zeichnete den kompletten Setup-Bildschirm neu, obwohl
    niemand konfigurierte - unnötiger Batterieverbrauch. Gelöst: im
    Setup-Zweig wird `sleepFor(..., enableTimerWakeup=false)`
    aufgerufen, das Gerät wacht dort nur noch per Tastendruck auf.

## Redesign heute (Nachmittag)

- **`assess()`-Kategorien umbenannt:** `GUENSTIG`→`NIEDRIG`,
  `TEUER`→`HOCH` (umlautfreie Alternative, `NORMAL` unverändert) -
  Grund: GxEPD2/Adafruit-GFX-Fonts unterstützen auf dem Display nur
  ASCII (0x20-0x7E), keine Umlaute. Auf der **Browser-Setup-Seite**
  hingegen sind Umlaute kein Problem (läuft im Browser-Font, nicht
  über das GFX-Bitmap-Font) und wurden dort bewusst eingesetzt.
- **Kategorisierung von Perzentil- auf Durchschnitts-Prozent
  umgestellt:** `<90%` Durchschnitt = niedrig, `90-110%` = normal,
  `>110%` = hoch (statt der ursprünglichen 25/75-Perzentile). `p25Ct`/
  `p75Ct`-Feldnamen sind aus Kompatibilitätsgründen erhalten geblieben,
  enthalten aber jetzt `average * 0.9` bzw. `* 1.1`.
- **`DisplayDriver::render()` komplett neu aufgebaut:** Statusbalken
  oben rechts (füllfarbig, Text weiß auf Grün/Rot, schwarz auf Gelb),
  Hauptzahl mit schwarzer Kontur, Min/Avg/Max-Zeile, Diagramm mit
  Y-Achsen-Rasterlinien (ganzzahlige Beschriftung) und X-Achsen-
  Stundenticks, horizontales "JETZT"-Label (Statusfarbe, bündig mit
  Diagrammoberkante), Tageswechsel-Marker bei vorhandenem
  `tomorrow`-Datensatz, Batterieanzeige unten rechts (Gehäuse + Blitz-
  Symbol bei Ladevorgang), Phasen-Text ("NIEDRIG NOCH.../NIEDRIG AB...")
  unten links, "LETZTES UPDATE"-Zeile mittig in der Fußzeile.
- **Batterieanzeige:** `readBatteryStatus()` liest Spannung (Register
  `0x34`/`0x35`, Plausibilitätsbereich 2500-4500 mV) und Laderichtung
  (Register `0x01`, Bits 6-5, `01` = lädt). Lineare Prozent-Näherung
  `(mV - 3300) / 9`. Erscheint nur bei plausiblem Wert, sonst keine
  Anzeige (keine Platzhalter).
- **Setup-Webseite (`SetupPortal::page()`) komplett neu im iOS-Stil:**
  Gruppierte abgerundete Karten, hellgraues Seiten-Grundlayout, Teal-
  farbener Kopfbereich mit Titel + "Setup"-Badge (Punkt in zweiter
  Akzentfarbe Koralle), gefüllter Teal-Button statt Schwarz. System-
  Sans-Font-Stack (`-apple-system, 'SF Pro Text', 'Segoe UI', ...`) -
  kein Web-Font-Laden, da das Gerät im AP-Only-Modus ohne Internet
  läuft.
  **Wichtige Falle bei künftigen Änderungen an diesem HTML-String:**
  Der komplette HTML/CSS/JS-Inhalt steckt in einem C++ `F("...")`-
  String-Literal mit doppelten Anführungszeichen als äußerer
  Begrenzer. **Jedes doppelte Anführungszeichen innerhalb des Strings
  bricht den Compile** - das ist heute mehrfach passiert (u. a. bei
  `onchange="..."`-Attributen). Im gesamten String dürfen nur einfache
  Anführungszeichen (`'`) vorkommen.
- **WLAN-Netzwerk-Dropdown:** `SetupPortal::run()` scannt vor dem
  AP-Start kurz im `WIFI_AP_STA`-Modus (`WiFi.scanNetworks()`), baut
  daraus `scannedNetworksJson`, liefert es über einen neuen
  `/networks`-Endpoint. Das Frontend befüllt das `<select>` per
  `fetch('/networks')`, ein manuelles Textfeld bleibt als Fallback für
  versteckte Netze.
- **Tibber-Token-Anleitung** verweist jetzt auf die korrekte, direkte
  URL `developer.tibber.com/settings/access-token` (vorher war die
  Anleitung ungenau/unvollständig).
- **Boot-Diagnose:** `esp_reset_reason()` + `esp_sleep_get_wakeup_cause()`
  werden bei jedem Boot als erste Zeile geloggt (`Boot: reset=%d
  wakeup=%d`) - dauerhaft im Code belassen, kein Wegwerf-Checkpoint,
  siehe "Offener Punkt" unten.

## Offene Punkte

### 1. Gelegentlicher Zwischen-Refresh außerhalb des stündlichen Zyklus
Nutzer berichtet, dass das Display gelegentlich auch abseits der
geplanten stündlichen Updates (`HH:01:30`) neu zeichnet. Noch nicht
reproduziert/eingegrenzt. Die Boot-Diagnosezeile (`Boot: reset=%d
wakeup=%d`, siehe oben) wurde genau dafür ergänzt, aber es fehlt noch
ein tatsächlich eingefangener Log-Auszug von einem solchen Ereignis.

**Interpretationshilfe für den nächsten eingefangenen Fall:**
- `wakeup=4` (`ESP_SLEEP_WAKEUP_TIMER`) → planmäßig, kein Problem
- `wakeup=2` (`ESP_SLEEP_WAKEUP_EXT0`) → Taste ausgelöst (mechanisch
  oder absichtlich)
- `reset` ungleich dem normalen Deep-Sleep-Wert (`ESP_RST_DEEPSLEEP`,
  Wert 5) → echter ungeplanter Neustart, dann in Richtung
  Stromversorgung/Brownout weiterschauen

### 2. `Busy Timeout!` beim E-Paper-Refresh
Seit dem allerersten funktionierenden Refresh im GxEPD2-Log sichtbar,
nie behoben. Vermutlich (nicht bestätigt) die Ursache für den einmal
beobachteten QR-Code-Bildfehler (siehe gelöstes Problem 9). Display
funktioniert trotz des Timeouts sichtbar korrekt, daher bisher nicht
weiterverfolgt. Falls erneut ein Bildfehler auftritt, hier zuerst
nachschauen.

### 3. 48h-Diagrammverlauf noch nicht am Nachmittag verifiziert
Das Diagramm kombiniert `today` + `tomorrow` automatisch, sobald beide
Daten vorhanden sind (Tibber veröffentlicht `tomorrow` typischerweise
erst nachmittags). Morgens wurde bestätigt, dass korrekt nur 24h
angezeigt werden (`tomorrow` ist dann leer, das ist erwartetes
Verhalten). Ob die Erweiterung auf bis zu 48h nach Veröffentlichung der
Folgetagspreise tatsächlich zuverlässig greift, wurde noch nicht am
Nachmittag/Abend verifiziert.

### 4. Kurzer Tastendruck für "sofort aktualisieren"
Der Footer-Text auf dem Display bewirbt "KEY kurz: sofort
aktualisieren", das ist aber nie implementiert worden - ein kurzer
Druck bewirkt nichts (nur der Deep-Sleep-Wakeup selbst löst ohnehin
einen vollen Zyklus aus, das ist aber nicht dasselbe wie ein gezielter
manueller Sofort-Refresh bei bereits wachem Gerät).

## Bereits geprüft und nicht (mehr) relevant

- Displayrotation ist korrekt auf `setRotation(2)` gesetzt (180°),
  Nutzer hat den Rahmen entsprechend physisch gedreht.
- AXP2101-Register, GPIO5-Latch, Stack-Overflow-Fix, Heap-Engpass:
  alle bestätigt gelöst, nicht erneut vorschlagen (siehe oben).
- Der frühere Verdacht auf einen Treiber-Mismatch
  (`GDEP073E01` vs. angeblich `ED2208-GCA`) hat sich als falsch
  herausgestellt - `GDEP073E01` ist der korrekte, herstellerbestätigte
  Treiber für dieses Panel.
