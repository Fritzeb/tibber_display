// ============================================================================
// VERSION: BETA - basiert auf v1.0.0 (stable)
// Branch: experimente
// Aktuelle Experimente:
// - Blitz-Symbol im Batteriegehaeuse (weisse Fuellung, schwarze Kontur)
//   statt links daneben.
// - Tibber wird nicht mehr stuendlich kontaktiert, nur noch einmal fuer den
//   neuen Tag und einmal nachmittags (AppConfig::TOMORROW_FETCH_HOUR) fuer
//   die Morgen-Preise. Alle anderen stuendlichen Wakes zeichnen nur aus dem
//   Cache neu. today+tomorrow werden dafuer beide kompakt in NVS gecacht
//   (Storage::loadDayArray/saveDayArray) statt wie bisher nur today als JSON.
//   NOCH NICHT AUF HARDWARE UEBER EINEN VOLLEN TAG VERIFIZIERT - besonders
//   der Tageswechsel um Mitternacht und das 14-Uhr-Fenster verdienen
//   Beobachtung.
// - Kurzer Tastendruck (KeyHoldResult::SHORT_PRESS) erzwingt jetzt einen
//   sofortigen Preisabruf unabhaengig vom Zeitfenster - war durch die
//   Abruf-Entkopplung oben sonst unmoeglich (manueller Refresh ausserhalb
//   der beiden taeglichen Fenster).
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <GxEPD2_7C.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <qrcode.h>
#include <time.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp_system.h>
#include <driver/rtc_io.h>
#include <algorithm>
#include <vector>

// Waveshare ESP32-S3 PhotoPainter: fixed internal connections.
#define EPD_BUSY 13
#define EPD_RST  12
#define EPD_DC   8
#define EPD_CS   9
#define EPD_SCK  10
#define EPD_MOSI 11
#define REFRESH_BUTTON 4
#define PMIC_SDA 47
#define PMIC_SCL 48
#define SYS_OUT_LATCH 5

// Optional build-time bootstrap values. Runtime values stored in Preferences win.
// Never print any of these values to Serial or render them on the display.
#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID ""
#endif
#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD ""
#endif
#ifndef APP_TIBBER_TOKEN
#define APP_TIBBER_TOKEN ""
#endif


struct RuntimeConfig {
  String ssid;
  String password;
  String tibberToken;
  bool configured() const { return !ssid.isEmpty() && !password.isEmpty() && tibberToken.length() >= 10; }
};

struct PricePoint {
  String startsAt;
  float totalEurPerKwh = 0;
  String currency = "EUR";
  PricePoint() = default;
  // Expliziter Konstruktor: Default-Werte oben machen die Struktur unter
  // C++11 (PlatformIOs Standardeinstellung fuer diese Arduino-Toolchain) zu
  // keinem Aggregate mehr - push_back({a, b, c}) braucht daher diesen
  // Konstruktor statt Aggregate-Initialisierung.
  PricePoint(String startsAt_, float totalEurPerKwh_, String currency_)
      : startsAt(std::move(startsAt_)), totalEurPerKwh(totalEurPerKwh_), currency(std::move(currency_)) {}
};

struct PriceSnapshot {
  String fetchedAt, homeId, staleReason;
  uint16_t intervalMinutes = 60;
  int currentIndex = -1;
  std::vector<PricePoint> today, tomorrow;
  bool isStale = true;
};

struct PriceAssessment {
  String category = "UNBEKANNT";
  float averageCt = 0, medianCt = 0, p25Ct = 0, p75Ct = 0, currentCt = 0, deltaPercent = 0;
  String nextCheapStart;
  int nextCheapInHours = -1, currentCheapRemainingHours = -1;
};

enum class KeyHoldResult { NONE, SHORT_PRESS, MAINTENANCE_TOGGLE, FACTORY_RESET };

struct BatteryStatus {
  bool valid = false;
  uint8_t percent = 0;
  bool charging = false;
};

// Forward declarations


// Forward declarations
BatteryStatus readBatteryStatus();

KeyHoldResult readKeyHold();

bool axpReadRegister(uint8_t reg, uint8_t &value);
bool axpWriteRegister(uint8_t reg, uint8_t value);
void initAxp2101();

time_t parseIso(const String &s);
String localTime(const String &s);
String isoNow();
String formatIsoLocal(time_t t);
String localDateString(time_t t);
PriceAssessment assess(const PriceSnapshot &s);
int computeCurrentIndex(const PriceSnapshot &s);
uint32_t nextSleepSeconds();
void sleepFor(uint32_t seconds, bool enableTimerWakeup = true);
String suffix();

namespace AppConfig {
constexpr char TIMEZONE[] = "CET-1CEST,M3.5.0,M10.5.0/3";
// A short timeout avoids spending battery energy searching for an unavailable access point.
constexpr uint32_t WIFI_TIMEOUT_MS = 12000;
constexpr uint32_t PORTAL_TIMEOUT_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t FACTORY_RESET_HOLD_MS = 15000;
constexpr uint32_t UPDATE_OFFSET_SECONDS = 90;
constexpr uint16_t NETWORK_UPDATE_MINUTES = 60;
// Save battery overnight: fetch at 00:01, then pause automatic network updates until 05:01.
constexpr uint8_t NIGHT_PAUSE_FIRST_HOUR = 1;
constexpr uint8_t NIGHT_PAUSE_RESUME_HOUR = 5;
constexpr uint32_t NTP_SYNC_INTERVAL_SECONDS = 12UL * 60UL * 60UL;
// Tibber-Tagespreise aendern sich nach Veroeffentlichung nicht mehr - ein
// staendliches Neuverbinden ist daher fast immer verschwendete Batterie.
// Tibber kontaktieren wir nur noch: einmal fuer den neuen Tag (falls die
// Vorschau vom Vortag fehlt) und einmal ab dieser Uhrzeit fuer die
// Morgen-Preise (Day-Ahead-Auktion ist ueblicherweise ab dem fruehen
// Nachmittag veroeffentlicht). Alle anderen stuendlichen Wakes zeichnen nur
// aus dem Cache neu (JETZT-Balken, Batterie, Tasten) - kein WLAN noetig.
constexpr uint8_t TOMORROW_FETCH_HOUR = 14;
constexpr uint8_t CHEAP_PERCENTILE = 25;
constexpr uint8_t EXPENSIVE_PERCENTILE = 75;
constexpr float VERY_EXPENSIVE_CT = 40.0F;
constexpr uint16_t NEXT_WINDOW_MINUTES = 60;
constexpr uint16_t FALLBACK_SLEEP_SECONDS = 900;
constexpr char TIBBER_URL[] = "https://api.tibber.com/v1-beta/gql";
constexpr char AP_PREFIX[] = "PhotoPainter-Setup-";
constexpr char AP_PASSWORD_PREFIX[] = "preis-";
}

class Storage {
 public:
  bool loadConfig(RuntimeConfig &cfg) {
    Preferences p; if (!p.begin("pricecfg", true)) return false;
    cfg.ssid = p.getString("ssid", ""); cfg.password = p.getString("pass", "");
    cfg.tibberToken = p.getString("token", ""); p.remove("home"); p.end();
    if (!cfg.configured() && strlen(APP_WIFI_SSID) && strlen(APP_WIFI_PASSWORD) && strlen(APP_TIBBER_TOKEN) >= 10) {
      cfg.ssid = APP_WIFI_SSID; cfg.password = APP_WIFI_PASSWORD; cfg.tibberToken = APP_TIBBER_TOKEN;
    }
    return cfg.configured();
  }
  bool saveConfig(const RuntimeConfig &cfg) {
    Preferences p; if (!p.begin("pricecfg", false)) return false;
    bool ok = p.putString("ssid", cfg.ssid) > 0 && p.putString("pass", cfg.password) > 0 &&
              p.putString("token", cfg.tibberToken) > 0;
    // Older firmware may have stored an optional Home ID; it is no longer used.
    p.remove("home"); p.end(); return ok;
  }
  void eraseConfigAndSnapshot() { Preferences p; if (p.begin("pricecfg", false)) { p.clear(); p.end(); } if (p.begin("tibber", false)) { p.clear(); p.end(); } }
  // Kompaktes Binaerformat statt JSON: nur Startzeit + Intervall + rohes
  // Float-Array pro Tag, keine wiederholten ISO-Zeitstempel-Strings. Bei
  // 24 Punkten heute+morgen ~200 Byte statt ~3500 Byte JSON - haelt auch bei
  // 15-Minuten-Aufloesung (96 Punkte/Tag) sicher Abstand vom NVS-Platzlimit,
  // das hier schon einmal zuschlug (siehe Kommentar in der alten Version).
  bool loadSnapshot(PriceSnapshot &s) {
    Preferences p; if (!p.begin("tibber", true)) return false;
    if (!p.isKey("tCnt")) { p.end(); return false; }
    s.homeId = p.getString("homeId", "");
    s.intervalMinutes = p.getUShort("ivalMin", 60);
    String currency = p.getString("curr", "EUR");
    s.fetchedAt = p.getString("fetchedAt", "");
    loadDayArray(p, "t", s.intervalMinutes, currency, s.today);
    loadDayArray(p, "m", s.intervalMinutes, currency, s.tomorrow);
    s.isStale = true; s.staleReason = "offline";
    p.end();
    return !s.today.empty();
  }
  bool saveSnapshot(const PriceSnapshot &s) {
    Preferences p; if (!p.begin("tibber", false)) return false;
    String currency = s.today.empty() ? String("EUR") : s.today[0].currency;
    auto writeAll = [&]() {
      p.putString("homeId", s.homeId);
      p.putUShort("ivalMin", s.intervalMinutes);
      p.putString("curr", currency);
      p.putString("fetchedAt", s.fetchedAt);
      return saveDayArray(p, "t", s.today) && saveDayArray(p, "m", s.tomorrow);
    };
    bool ok = writeAll();
    if (!ok) {
      // Vermutliche NVS-Fragmentierung nach vielen Schreibvorgaengen:
      // Namespace einmalig leeren und erneut versuchen. Loescht dabei auch
      // WLAN-Hint und NTP-Sync-Zeitstempel im selben Namespace mit.
      p.clear();
      ok = writeAll();
      Serial.println(ok ? "Preis-Cache nach NVS-Bereinigung gespeichert."
                         : "Preis-Cache weiterhin nicht speicherbar.");
    }
    p.end();
    return ok;
  }
  bool loadLastTimeSync(time_t &value) { Preferences p; if (!p.begin("tibber", true)) return false; value = static_cast<time_t>(p.getULong64("ntpSync", 0)); p.end(); return value > 0; }
  void saveLastTimeSync(time_t value) { Preferences p; if (p.begin("tibber", false)) { p.putULong64("ntpSync", static_cast<uint64_t>(value)); p.end(); } }
  bool loadMaintenanceMode() {
    Preferences p; if (!p.begin("settings", true)) return false;
    bool v = p.getBool("maint", false); p.end(); return v;
  }
  void saveMaintenanceMode(bool enabled) {
    Preferences p; if (p.begin("settings", false)) { p.putBool("maint", enabled); p.end(); }
  }
  // Persistiert Reset-/Wakeup-Ursache und einen Zaehler fuer Spontan-Wakeups
  // (EXT0 ohne tatsaechlichen Tastendruck). Ohne angeschlossenen Rechner sieht
  // niemand eine reine Serial-Logzeile live - dieser Zaehler bleibt dagegen bis
  // zur naechsten Verbindung auslesbar.
  uint32_t recordWakeDiagnostics(int resetReason, int wakeupCause, bool spurious) {
    Preferences p; if (!p.begin("settings", false)) return 0;
    p.putInt("rstReason", resetReason);
    p.putInt("wakeCause", wakeupCause);
    uint32_t count = p.getUInt("spuriousCnt", 0);
    if (spurious) { count += 1; p.putUInt("spuriousCnt", count); }
    p.end();
    return count;
  }
  bool loadWifiHint(uint8_t *bssid, int32_t &channel) {
    Preferences p; if (!p.begin("tibber", true) || !p.isKey("wifiBssid")) return false;
    size_t count = p.getBytes("wifiBssid", bssid, 6); channel = p.getInt("wifiChan", 0); p.end();
    return count == 6 && channel > 0;
  }
  void saveWifiHint(const uint8_t *bssid, int32_t channel) {
    if (!bssid || channel <= 0) return;
    Preferences p; if (p.begin("tibber", false)) { p.putBytes("wifiBssid", bssid, 6); p.putInt("wifiChan", channel); p.end(); }
  }
  bool snapshotHasSamePrices(const PriceSnapshot &a, const PriceSnapshot &b) {
    if (a.homeId != b.homeId || a.intervalMinutes != b.intervalMinutes || a.today.size() != b.today.size() || a.tomorrow.size() != b.tomorrow.size()) return false;
    auto same = [](const std::vector<PricePoint> &x, const std::vector<PricePoint> &y) { for (size_t i = 0; i < x.size(); ++i) if (x[i].startsAt != y[i].startsAt || x[i].totalEurPerKwh != y[i].totalEurPerKwh || x[i].currency != y[i].currency) return false; return true; };
    return same(a.today, b.today) && same(a.tomorrow, b.tomorrow);
  }

 private:
  static bool loadDayArray(Preferences &p, const char *prefix, uint16_t intervalMinutes, const String &currency, std::vector<PricePoint> &out) {
    out.clear();
    uint16_t count = p.getUShort((String(prefix) + "Cnt").c_str(), 0);
    if (count == 0) return true;
    int64_t start = p.getLong64((String(prefix) + "Start").c_str(), 0);
    if (start <= 0) return true;
    std::vector<float> prices(count);
    size_t got = p.getBytes((String(prefix) + "Px").c_str(), prices.data(), count * sizeof(float));
    if (got != count * sizeof(float)) return true;
    out.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
      time_t pointTime = static_cast<time_t>(start) + static_cast<time_t>(i) * intervalMinutes * 60;
      out.push_back(PricePoint(formatIsoLocal(pointTime), prices[i], currency));
    }
    return true;
  }
  static bool saveDayArray(Preferences &p, const char *prefix, const std::vector<PricePoint> &points) {
    if (points.empty()) { p.putUShort((String(prefix) + "Cnt").c_str(), 0); return true; }
    time_t start = parseIso(points[0].startsAt);
    if (start <= 0) { p.putUShort((String(prefix) + "Cnt").c_str(), 0); return false; }
    std::vector<float> prices; prices.reserve(points.size());
    for (const auto &pt : points) prices.push_back(pt.totalEurPerKwh);
    bool ok = p.putLong64((String(prefix) + "Start").c_str(), static_cast<int64_t>(start)) > 0;
    ok = ok && p.putBytes((String(prefix) + "Px").c_str(), prices.data(), prices.size() * sizeof(float)) > 0;
    ok = ok && p.putUShort((String(prefix) + "Cnt").c_str(), static_cast<uint16_t>(points.size())) > 0;
    return ok;
  }
};

time_t parseIso(const String &s) { struct tm t = {}; if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &t)) return 0; t.tm_isdst = -1; return mktime(&t); }
String localTime(const String &s) { time_t v = parseIso(s); if (v <= 0) return "?"; struct tm t; localtime_r(&v, &t); char b[8]; strftime(b, sizeof(b), "%H:%M", &t); return b; }
String formatIsoLocal(time_t t) { struct tm tv; localtime_r(&t, &tv); char b[32]; strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S%z", &tv); return b; }
String isoNow() { return formatIsoLocal(time(nullptr)); }
// Kalendertag (lokal) als Vergleichsschluessel - fuer die Entscheidung, ob
// der Preis-Cache noch zum aktuellen bzw. naechsten Tag passt.
String localDateString(time_t t) { struct tm tv; localtime_r(&t, &tv); char b[11]; strftime(b, sizeof(b), "%Y-%m-%d", &tv); return b; }

class DisplayDriver {
  GxEPD2_7C<GxEPD2_730c_GDEP073E01, GxEPD2_730c_GDEP073E01::HEIGHT / 2> display;
  bool ready = false;
  void header(const char *title) { display.setTextColor(GxEPD_BLACK); display.setFont(&FreeMonoBold18pt7b); display.setCursor(24, 38); display.print(title); display.drawLine(20, 49, 780, 49, GxEPD_BLACK); }
  void line(int y, const String &text, uint16_t color = GxEPD_BLACK) { display.setTextColor(color); display.setFont(&FreeMonoBold12pt7b); display.setCursor(26, y); display.print(text); }
 public:
  DisplayDriver() : display(GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)) {}
  bool begin() { if (ready) return true; SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS); display.init(115200, true, 2, false); display.setRotation(2); ready = true; return true; }
  void sleep() { if (ready) { display.powerOff(); ready = false; } }
  bool ensureReady() { return begin(); }
  void renderSetup(const String &apSsid, const String &apPassword) {
    const String qrText = "WIFI:T:WPA;S:" + apSsid + ";P:" + apPassword + ";;";
    uint8_t qrData[qrcode_getBufferSize(4)]; QRCode qr; qrcode_initText(&qr, qrData, 4, ECC_LOW, qrText.c_str());
    display.firstPage(); do {
      display.fillScreen(GxEPD_WHITE); header("STROMPREIS EINRICHTEN");
      const int scale = 5, x0 = 30, y0 = 75;
      for (uint8_t y = 0; y < qr.size; ++y) for (uint8_t x = 0; x < qr.size; ++x) if (qrcode_getModule(&qr, x, y)) display.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, GxEPD_BLACK);
      line(290, "1. QR-Code mit dem Handy scannen.");
      line(320, "2. Mit diesem WLAN verbinden:", GxEPD_BLUE); line(350, apSsid);
      line(380, "Passwort: " + apPassword);
      line(410, "3. Im Browser: http://192.168.4.1");
      line(450, "Dort WLAN und Tibber-Zugang sicher eintragen.", GxEPD_RED);
    } while (display.nextPage());
  }
  void renderPortalSaved() { display.firstPage(); do { display.fillScreen(GxEPD_WHITE); header("EINRICHTUNG GESPEICHERT"); line(130, "Die Zugangsdaten wurden gespeichert.", GxEPD_BLACK); line(180, "Der Rahmen startet jetzt neu und", GxEPD_BLACK); line(215, "laedt die Strompreise.", GxEPD_BLACK); } while (display.nextPage()); }
  void renderNoData(const String &reason, bool maintenanceMode) { display.firstPage(); do { display.fillScreen(GxEPD_WHITE); header("NOCH KEINE PREISDATEN"); if (maintenanceMode) {
      const int barY = 66, barH = 20;
      display.fillRect(20, barY, 760, barH, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
      display.setFont(&FreeMonoBold9pt7b);
      display.setCursor(28, barY + 15);
      display.print("WARTUNGSMODUS AKTIV - KEY CA. 5 SEK. HALTEN ZUM BEENDEN");
    } line(130, "WLAN oder Tibber-Verbindung pruefen.", GxEPD_RED); line(180, "Status: " + reason); line(250, "KEY 15 Sekunden halten: Neueinrichtung"); } while (display.nextPage()); }
  void render(const PriceSnapshot &s, const PriceAssessment &a, bool maintenanceMode, const BatteryStatus &battery) {
    auto centered = [this](const String &text, int centerX, int baselineY) {
      int16_t x1, y1; uint16_t w, h;
      display.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
      display.setCursor(centerX - static_cast<int>(w) / 2, baselineY);
      display.print(text);
    };
    auto rightAligned = [this](const String &text, int rightX, int baselineY) {
      int16_t x1, y1; uint16_t w, h;
      display.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
      display.setCursor(rightX - w, baselineY);
      display.print(text);
    };
    display.firstPage(); do {
      display.fillScreen(GxEPD_WHITE);
      uint16_t statusColor = a.category == "NIEDRIG" ? GxEPD_GREEN : (a.category == "HOCH" ? GxEPD_RED : GxEPD_YELLOW);
      display.setTextColor(GxEPD_BLACK); display.setFont(&FreeMonoBold24pt7b);
      const String title = "STROMPREIS";
      int16_t titleX1, titleY1; uint16_t titleW, titleH;
      display.getTextBounds(title.c_str(), 0, 0, &titleX1, &titleY1, &titleW, &titleH);
      const int titleBaseline = static_cast<int>(40.5F - titleY1 - static_cast<float>(titleH) / 2.0F);
      display.setCursor(28, titleBaseline); display.print(title);
      display.fillRect(610, 19, 160, 43, statusColor);
      uint16_t statusTextColor = (a.category == "NORMAL") ? GxEPD_BLACK : GxEPD_WHITE;
      display.setTextColor(statusTextColor); display.setFont(&FreeMonoBold12pt7b); centered(a.category, 690, 47);
      if (maintenanceMode) {
        const int barY = 66, barH = 20;
        display.fillRect(20, barY, 760, barH, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
        display.setFont(&FreeMonoBold9pt7b);
        display.setCursor(28, barY + 15);
        display.print("WARTUNGSMODUS AKTIV - KEY CA. 5 SEK. HALTEN ZUM BEENDEN");
      }

      String price = String(a.currentCt, 1);
      display.setFont(&FreeMonoBold24pt7b); display.setTextColor(GxEPD_BLACK);
      int16_t x1, y1; uint16_t priceW, priceH;
      display.getTextBounds(price.c_str(), 0, 0, &x1, &y1, &priceW, &priceH);
      int priceX = 400 - static_cast<int>(priceW) / 2;
      display.setCursor(priceX - 1, 125); display.print(price);
      display.setCursor(priceX + 1, 125); display.print(price);
      display.setCursor(priceX, 124); display.print(price);
      display.setCursor(priceX, 126); display.print(price);
      display.setTextColor(statusColor); display.setCursor(priceX, 125); display.print(price);
      display.setTextColor(GxEPD_BLACK); display.setFont(&FreeMonoBold12pt7b); centered("CT/KWH", 400, 151);

      float minCt = s.today[0].totalEurPerKwh * 100.0F, maxCt = minCt;
      for (const auto &p : s.today) { float ct = p.totalEurPerKwh * 100.0F; minCt = std::min(minCt, ct); maxCt = std::max(maxCt, ct); }
      display.setFont(&FreeMonoBold9pt7b); display.setTextColor(GxEPD_BLACK);
      display.setCursor(80, 180); display.print("MIN " + String(minCt, 1) + " CT");
      centered("AVG " + String(a.averageCt, 1) + " CT", 400, 180);
      rightAligned("MAX " + String(maxCt, 1) + " CT", 720, 180);

      const int chartX = 56, chartY = 225, chartW = 710, chartH = 160;
      std::vector<PricePoint> points;
      points.reserve(s.today.size() + s.tomorrow.size());
      points.insert(points.end(), s.today.begin(), s.today.end());
      points.insert(points.end(), s.tomorrow.begin(), s.tomorrow.end());
      const int todayCount = static_cast<int>(s.today.size());
      const int n = static_cast<int>(points.size());
      auto barLeft = [chartX, chartW, n](int index) {
        return chartX + 1 + (index * (chartW - 2)) / std::max(1, n);
      };
      auto barRight = [chartX, chartW, n](int index) {
        return chartX + 1 + ((index + 1) * (chartW - 2)) / std::max(1, n);
      };

      display.drawRect(chartX, chartY, chartW, chartH, GxEPD_BLACK);
      // Skalierung ueber ALLE sichtbaren Balken (heute+morgen), nicht nur
      // ueber maxCt (heute) - sonst ragen morgige Balken ueber den oberen
      // Rand hinaus, wenn sie teurer als der teuerste heutige Preis sind.
      float chartMaxSourceCt = maxCt;
      for (const auto &p : points) chartMaxSourceCt = std::max(chartMaxSourceCt, p.totalEurPerKwh * 100.0F);
      const float chartMaxCt = std::max(1.0F, chartMaxSourceCt * 1.15F);

      // Nine positions produce eight equal value bands. Every other position is a labelled main mark.
      display.setFont(&FreeMonoBold9pt7b);
      for (int division = 0; division <= 8; ++division) {
        const int y = chartY + chartH - (division * chartH) / 8;
        const float value = chartMaxCt * division / 8.0F;
        if ((division % 2) == 0) {
          display.drawLine(chartX + 1, y, chartX + chartW - 2, y, GxEPD_BLACK);
          rightAligned(String(static_cast<int>(roundf(value))), 52, y + 4);
        } else {
          display.drawLine(chartX - 4, y, chartX, y, GxEPD_BLACK);
        }
      }

      for (int i = 0; i < n; ++i) {
        const float ct = points[i].totalEurPerKwh * 100.0F;
        const int h = static_cast<int>((chartH - 2) * ct / chartMaxCt);
        const int x = barLeft(i);
        const int w = std::max(1, barRight(i) - x - 1);
        const uint16_t col = ct < a.p25Ct ? GxEPD_GREEN : (ct > a.p75Ct ? GxEPD_RED : GxEPD_YELLOW);
        display.fillRect(x, chartY + chartH - 1 - h, w, h, col);
      }

      // Ticks are derived from the actual local start time of every interval, not from its array index.
      display.setFont(&FreeMonoBold9pt7b); display.setTextColor(GxEPD_BLACK);
      for (int i = 0; i < n; ++i) {
        time_t starts = parseIso(points[i].startsAt);
        struct tm local = {};
        if (starts <= 0) continue;
        localtime_r(&starts, &local);
        if (local.tm_min != 0 || local.tm_sec != 0) continue;
        const int tickX = barLeft(i) + std::max(1, barRight(i) - barLeft(i)) / 2;
        display.drawLine(tickX, chartY + chartH, tickX, chartY + chartH + 2, GxEPD_BLACK);
        if ((local.tm_hour % 3) == 0) {
          display.drawLine(tickX, chartY + chartH, tickX, chartY + chartH + 6, GxEPD_BLACK);
          char hour[3]; snprintf(hour, sizeof(hour), "%02d", local.tm_hour);
          centered(String(hour), tickX, 410);
        }
      }

      if (todayCount > 0 && !s.tomorrow.empty()) {
        const int dayMarkerX = barLeft(todayCount);
        display.drawLine(dayMarkerX, chartY, dayMarkerX, chartY + chartH - 1, GxEPD_BLACK);
        display.drawLine(dayMarkerX + 1, chartY, dayMarkerX + 1, chartY + chartH - 1, GxEPD_BLACK);
        const String dayLabel = "<- HEUTE | MORGEN ->";
        display.setFont(&FreeMonoBold9pt7b);
        int16_t labelX1, labelY1, titleX1, titleY1; uint16_t labelW, labelH, titleW, titleH;
        display.getTextBounds(dayLabel.c_str(), 0, 0, &labelX1, &labelY1, &labelW, &labelH);
        display.getTextBounds("PREISVERLAUF", 0, 0, &titleX1, &titleY1, &titleW, &titleH);
        const int labelCenter = std::max(dayMarkerX, chartX + static_cast<int>(titleW) + static_cast<int>(labelW) / 2 + 10);
        centered(dayLabel, labelCenter, 210);
      }

      if (s.currentIndex >= 0 && s.currentIndex < n) {
        const int markerX = barLeft(s.currentIndex) + std::max(1, barRight(s.currentIndex) - barLeft(s.currentIndex)) / 2;
        display.drawLine(markerX, chartY + 1, markerX, chartY + chartH - 2, GxEPD_BLACK);
        display.drawLine(markerX + 1, chartY + 1, markerX + 1, chartY + chartH - 2, GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);
        uint16_t jetztTextColor = (a.category == "NORMAL") ? GxEPD_BLACK : GxEPD_WHITE;
        display.setTextColor(jetztTextColor);
        const String nowLabel = "JETZT";
        int16_t labelTextX1, labelTextY1; uint16_t labelTextW, labelTextH;
        display.getTextBounds(nowLabel.c_str(), 0, 0, &labelTextX1, &labelTextY1, &labelTextW, &labelTextH);
        const int labelW = static_cast<int>(labelTextW) + 8, labelH = 18;
        const int labelX = markerX - labelW / 2, labelY = chartY;
        display.fillRect(labelX, labelY, labelW, labelH, statusColor);
        display.drawRect(labelX, labelY, labelW, labelH, GxEPD_BLACK);
        centered(nowLabel, markerX, labelY + 13);
      }

      display.setFont(&FreeMonoBold9pt7b); display.setTextColor(GxEPD_BLACK);
      if (a.currentCheapRemainingHours >= 0) {
        display.setCursor(56, 466); display.print("NIEDRIG NOCH " + String(a.currentCheapRemainingHours) + " STD.");
      } else if (a.nextCheapInHours >= 0) {
        display.setCursor(56, 466); display.print("NIEDRIG AB " + localTime(a.nextCheapStart) + " (" + String(a.nextCheapInHours) + " STD.)");
      } else {
        display.setCursor(56, 466); display.print("KEINE NIEDRIGE PHASE ABSEHBAR");
      }
      const String updateText = "LETZTES UPDATE: " + localTime(s.fetchedAt) + (s.isStale ? " (OFFLINE)" : "");
      display.setCursor(400, 466); display.print(updateText);
      if (battery.valid) {
        // Rechtsbuendig mit dem Diagrammende (chartX + chartW = 56 + 710 = 766),
        // Pluspol-Nase (2px) mit eingerechnet.
        const int bx = 739, bw = 25, bh = 14, by = 466 - bh;
        display.drawRect(bx, by, bw, bh, GxEPD_BLACK);
        display.drawRect(bx + bw, by + 3, 2, bh - 6, GxEPD_BLACK); // Pluspol-Nase
        uint16_t fillColor = battery.percent >= 50 ? GxEPD_GREEN
                           : battery.percent >= 20 ? GxEPD_YELLOW
                           : GxEPD_RED;
        int fillW = (bw - 2) * battery.percent / 100;
        if (fillW > 0) display.fillRect(bx + 1, by + 1, fillW, bh - 2, fillColor);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);
        String pctText = String(battery.percent) + "%";
        int16_t tx, ty; uint16_t tw, th;
        display.getTextBounds(pctText, 0, 0, &tx, &ty, &tw, &th);
        display.setCursor(bx - tw - 6, by + bh - 3);
        display.print(pctText);
        if (battery.charging) {
          // Blitz sitzt jetzt im Batteriegehaeuse statt links davon, damit
          // beides als eine Einheit wirkt. Weisse Fuellung mit schwarzer
          // Kontur, damit er auf jeder Fuellfarbe (gruen/gelb/rot) lesbar
          // bleibt. Bolt ist 9px breit, 14px hoch - zentriert im 25x14-Gehaeuse.
          const int lx = bx + 8;
          const int ly = by;
          display.fillTriangle(lx + 6, ly, lx, ly + 8, lx + 5, ly + 8, GxEPD_WHITE);
          display.fillTriangle(lx + 5, ly + 8, lx + 9, ly + 6, lx + 3, ly + 14, GxEPD_WHITE);
          display.drawTriangle(lx + 6, ly, lx, ly + 8, lx + 5, ly + 8, GxEPD_BLACK);
          display.drawTriangle(lx + 5, ly + 8, lx + 9, ly + 6, lx + 3, ly + 14, GxEPD_BLACK);
        }
      }
    } while (display.nextPage());
  }
};

class SetupPortal {
  WebServer server{80}; DNSServer dns; Storage &storage; DisplayDriver &display; bool saved = false;
  String scannedNetworksJson = "[]";
  String page() {
    return F("<!doctype html><html lang='de'><meta name='viewport' content='width=device-width,initial-scale=1'><style>:root{--page:#F2F2F7;--card:#FFFFFF;--ink:#1C1C1E;--accent:#2E8B9E;--accent2:#F0665A;--accent-soft:rgba(46,139,158,0.12);--muted:#6E6E73;--hairline:rgba(60,60,67,0.16);--sans:-apple-system,'SF Pro Text','Segoe UI',Roboto,Helvetica,Arial,sans-serif}*{box-sizing:border-box}body{margin:0;background:var(--page);color:var(--ink);font-family:var(--sans);line-height:1.5;font-size:16px;display:flex;justify-content:center;padding:24px 16px 60px}.wrap{width:100%;max-width:430px}.hero{display:flex;justify-content:space-between;align-items:center;background:var(--accent);color:#fff;border-radius:18px;padding:18px 20px;margin-bottom:22px}.hero .name{font-weight:700;font-size:17px}.hero .badge{display:flex;align-items:center;gap:6px;font-size:13px;font-weight:600;color:#fff}.hero .badge .dot{width:7px;height:7px;border-radius:50%;background:var(--accent2)}.ios-card{background:var(--card);border-radius:16px;box-shadow:0 1px 2px rgba(0,0,0,0.04);margin-bottom:18px;overflow:hidden}.intro{font-size:14px;color:var(--muted);margin:0;padding:14px 18px}.field{padding:12px 18px;border-top:1px solid var(--hairline)}.field:first-of-type{border-top:none}label{display:block;font-size:13px;font-weight:600;color:var(--muted);margin-bottom:6px}.select-wrap{position:relative}select,input{width:100%;font-family:var(--sans);font-size:16px;padding:10px 12px;border:none;background:var(--page);color:var(--ink);border-radius:10px;appearance:none}.select-wrap::after{content:'▾';position:absolute;right:14px;top:50%;transform:translateY(-50%);color:var(--muted);pointer-events:none;font-size:13px}input.ssid-manual{margin-top:8px}select:focus,input:focus{outline:none;box-shadow:0 0 0 3px var(--accent-soft)}input::placeholder{color:#9a9a9e}.submit-wrap{padding:16px 18px 18px}button{width:100%;padding:14px 16px;border:none;border-radius:12px;background:var(--accent);color:#fff;font-family:var(--sans);font-size:16px;font-weight:600;cursor:pointer}button:active{opacity:0.75}h2{font-size:15px;font-weight:700;margin:0;padding:14px 18px 4px}ol{margin:0;padding:10px 18px 16px 34px;font-size:15px}ol li{margin-bottom:9px}ol li:last-child{margin-bottom:0}a{color:var(--accent);text-decoration:none}.fineprint{font-size:13.5px;color:var(--muted);padding:14px 18px}.fineprint:not(:first-child){border-top:1px solid var(--hairline)}.fineprint strong{color:var(--ink);font-weight:600;display:block;margin-bottom:2px}</style><div class='wrap'><div class='hero'><span class='name'>Strompreis-Rahmen</span><span class='badge'><span class='dot'></span>Setup</span></div><div class='ios-card'><p class='intro'>Die Daten werden nur im geschützten Speicher dieses Rahmens abgelegt. Das Tibber-Token wird weder angezeigt noch seriell ausgegeben.</p><form method='post' action='/save'><div class='field'><label for='ssidSelect'>WLAN-Name</label><div class='select-wrap'><select id='ssidSelect'><option value=''>– Netzwerk wählen oder unten eintragen –</option></select></div><input required class='ssid-manual' id='ssid' name='ssid' maxlength='32' placeholder='oder manuell eintragen'></div><div class='field'><label for='password'>WLAN-Passwort</label><input required id='password' name='password' type='password' maxlength='63'></div><div class='field'><label for='token'>Tibber Personal Access Token</label><input required id='token' name='token' type='password'></div><div class='submit-wrap'><button type='submit'>Speichern und starten</button></div></form></div><div class='ios-card'><h2>Tibber-Token besorgen</h2><ol><li>Öffne im Browser: <a href='https://developer.tibber.com/settings/access-token' target='_blank'>developer.tibber.com/settings/access-token</a></li><li>Melde dich mit deinem normalen Tibber-Konto an.</li><li>Klicke auf 'Create Token' (Token erstellen).</li><li>Der Token wird angezeigt – kopiere ihn sofort. Er wird oft nur einmal vollständig angezeigt.</li><li>Füge ihn hier im Feld 'Tibber Personal Access Token' ein.</li></ol></div><div class='ios-card'><p class='fineprint'><strong>Ohne Tibber</strong>Diese Firmware fragt derzeit ausschließlich Tibber direkt ab. Kostenlose Alternativen wie aWATTar oder ENTSO-E haben je nach Land andere Datenformate und enthalten nicht unbedingt deinen vollständigen Bruttopreis.</p><p class='fineprint'><strong>Werksreset</strong>Die KEY-Taste am Rahmen 15 Sekunden gedrückt halten. Das löscht WLAN, Token und gespeicherte Preise; danach erscheint der Einrichtungsbildschirm wieder.</p></div></div><script>function syncSsid(){document.getElementById('ssid').value=document.getElementById('ssidSelect').value;}document.getElementById('ssidSelect').addEventListener('change',syncSsid);fetch('/networks').then(function(r){return r.json();}).then(function(list){var seen={};var sel=document.getElementById('ssidSelect');list.forEach(function(ssid){if(!ssid||seen[ssid])return;seen[ssid]=true;var opt=document.createElement('option');opt.value=ssid;opt.textContent=ssid;sel.appendChild(opt);});}).catch(function(){});</script></html>");
  }
 public:
  SetupPortal(Storage &s, DisplayDriver &d) : storage(s), display(d) {}
  bool run(const String &ssid, const String &password) {
    display.renderSetup(ssid, password);
    WiFi.mode(WIFI_AP);
    WiFi.mode(WIFI_AP_STA);
    int scanCount = WiFi.scanNetworks();
    scannedNetworksJson = "[";
    for (int i = 0; i < scanCount; ++i) {
      if (i > 0) scannedNetworksJson += ",";
      scannedNetworksJson += "\"" + WiFi.SSID(i) + "\"";
    }
    scannedNetworksJson += "]";
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    bool apOk = WiFi.softAP(ssid.c_str(), password.c_str(), 1, false, 4);
    Serial.println(apOk ? "AP-START: erfolgreich" : "AP-START: FEHLGESCHLAGEN");
    Serial.println("AP-IP: " + WiFi.softAPIP().toString());
    Serial.println("AP-MAC: " + WiFi.softAPmacAddress());
    dns.start(53, "*", WiFi.softAPIP());

    WiFi.onEvent([](WiFiEvent_t event) {
      switch (event) {
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
          Serial.println("AP-EVENT: Client verbunden (Layer 2, vor IP-Vergabe)");
          break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
          Serial.println("AP-EVENT: Client getrennt/Verbindung abgelehnt");
          break;
        case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
          Serial.println("AP-EVENT: IP-Adresse an Client vergeben");
          break;
        default:
          break;
      }
    });

    server.on("/", HTTP_GET, [this](){ server.send(200, "text/html; charset=utf-8", page()); });
    server.on("/networks", HTTP_GET, [this](){ server.send(200, "application/json", scannedNetworksJson); });
    server.on("/generate_204", HTTP_GET, [this](){ server.sendHeader("Location", "/"); server.send(302); });
    server.on("/hotspot-detect.html", HTTP_GET, [this](){ server.sendHeader("Location", "/"); server.send(302); });
    server.on("/save", HTTP_POST, [this](){ RuntimeConfig c{server.arg("ssid"), server.arg("password"), server.arg("token")}; if (!c.configured() || !storage.saveConfig(c)) { server.send(400, "text/plain", "Eingaben unvollstaendig oder Speicherfehler."); return; } saved = true; server.send(200, "text/html", "<h1>Gespeichert</h1><p>Der Rahmen startet jetzt neu.</p>"); });
    server.onNotFound([this](){ server.sendHeader("Location", "/"); server.send(302); }); server.begin();

    Serial.println("AP SSID='" + ssid + "' PASSWORT='" + password + "'");
    Serial.println("Einrichtungsportal gestartet; Geheimnisse werden nicht geloggt.");
    uint32_t start = millis(); while (!saved && millis() - start < AppConfig::PORTAL_TIMEOUT_MS) { dns.processNextRequest(); server.handleClient(); delay(2); }
    server.stop(); dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); return saved;
  }
};

class TibberConnection {
  Storage &storage;
 public:
  explicit TibberConnection(Storage &s) : storage(s) {}
  bool connect(const RuntimeConfig &cfg) {
    WiFi.persistent(false); WiFi.setAutoReconnect(false); WiFi.mode(WIFI_STA);
    // Reduzierte Sendeleistung fuer den normalen Betrieb: Heimrouter sind meist
    // nah genug, volle Leistung (wie im Setup-AP) ist hier unnoetiger Verbrauch.
    // Bei Verbindungsproblemen durch schwaches Signal hier zuerst pruefen/anheben.
    WiFi.setTxPower(WIFI_POWER_15dBm);
    uint8_t bssid[6] = {}; int32_t channel = 0;
    // Reusing the last access point and radio channel avoids a full scan on most wakes.
    if (storage.loadWifiHint(bssid, channel)) WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str(), channel, bssid, true);
    else WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
    uint32_t start = millis(); while (WiFi.status() != WL_CONNECTED && millis() - start < AppConfig::WIFI_TIMEOUT_MS) delay(100);
    if (WiFi.status() != WL_CONNECTED) { Serial.println("WLAN nicht erreichbar."); WiFi.disconnect(true); WiFi.mode(WIFI_OFF); return false; }
    storage.saveWifiHint(WiFi.BSSID(), WiFi.channel()); Serial.println("WLAN verbunden."); return true;
  }
  void off() { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
};

class PriceProvider {
  static void points(JsonArray a, std::vector<PricePoint> &out) { for (JsonObject x : a) if (!x["startsAt"].isNull() && !x["total"].isNull()) out.push_back({x["startsAt"] | "", x["total"] | 0.0F, x["currency"] | "EUR"}); }
 public:
  bool fetch(const RuntimeConfig &cfg, PriceSnapshot &out, String &reason) {
    WiFiClientSecure tls; tls.setInsecure();
    tls.setTimeout(10000);
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(tls, AppConfig::TIBBER_URL)) { reason = "api"; return false; }
    http.addHeader("Authorization", String("Bearer ") + cfg.tibberToken); http.addHeader("Content-Type", "application/json");
    const char *body = R"({"query":"query GetPriceInfo { viewer { homes { id currentSubscription { priceInfo { current { total startsAt currency } today { total startsAt currency } tomorrow { total startsAt currency } } } } } }"})";
    int code = http.POST(body);
    String payload = code > 0 ? http.getString() : ""; http.end(); if (code == 401 || code == 403) { reason = "authorisierung"; return false; } if (code != 200) { reason = "api"; return false; }
    JsonDocument d; if (deserializeJson(d, payload) || !d["errors"].isNull()) { reason = "api"; return false; } JsonArray homes = d["data"]["viewer"]["homes"].as<JsonArray>(); if (homes.isNull() || homes.size() == 0) { reason = "api"; return false; }
    // Electricity prices are market-wide for the account's homes, so use the
    // first returned Home. No Home ID is requested or stored in this project.
    JsonObject home = homes[0];
    JsonObject info = home["currentSubscription"]["priceInfo"]; if (info.isNull()) { reason = "keine-preise"; return false; } out = PriceSnapshot(); out.homeId = home["id"].as<String>(); points(info["today"].as<JsonArray>(), out.today); points(info["tomorrow"].as<JsonArray>(), out.tomorrow); if (out.today.empty()) { reason = "keine-preise"; return false; }
    String cur = info["current"]["startsAt"] | ""; for (size_t i = 0; i < out.today.size(); ++i) if (out.today[i].startsAt == cur) out.currentIndex = i; if (out.currentIndex < 0 && !cur.isEmpty()) { out.today.push_back({cur, info["current"]["total"] | 0.0F, info["current"]["currency"] | "EUR"}); out.currentIndex = out.today.size() - 1; }
    if (out.today.size() > 1) { time_t a = parseIso(out.today[0].startsAt), b = parseIso(out.today[1].startsAt); if (a && b > a) out.intervalMinutes = (b - a) / 60; }
    out.fetchedAt = isoNow(); out.isStale = false; return true;
  }
};

// Leitet den aktuellen Preis-Index aus der Wanduhrzeit ab, statt ihn aus dem
// Cache zu uebernehmen. Dadurch stimmt der JETZT-Balken auch auf reinen
// Redraw-Wakes ohne Tibber-Verbindung, solange die heutigen Preise im Cache
// stehen.
int computeCurrentIndex(const PriceSnapshot &s) {
  if (s.today.empty()) return -1;
  time_t start = parseIso(s.today[0].startsAt);
  time_t now = time(nullptr);
  if (start <= 0 || now <= 0 || s.intervalMinutes == 0) return 0;
  long idx = static_cast<long>((now - start) / (s.intervalMinutes * 60));
  if (idx < 0) idx = 0;
  if (idx >= static_cast<long>(s.today.size())) idx = static_cast<long>(s.today.size()) - 1;
  return static_cast<int>(idx);
}

PriceAssessment assess(const PriceSnapshot &s) {
  PriceAssessment a; if (s.currentIndex < 0 || s.today.empty()) return a; std::vector<float> v; for (const auto &p : s.today) v.push_back(p.totalEurPerKwh * 100); std::sort(v.begin(), v.end()); float sum = 0; for (float x : v) sum += x; a.averageCt = sum / v.size(); a.medianCt = v[v.size()/2]; a.p25Ct = a.averageCt * 0.9F; a.p75Ct = a.averageCt * 1.1F; a.currentCt = s.today[s.currentIndex].totalEurPerKwh*100; a.deltaPercent = a.averageCt ? 100*(a.currentCt-a.averageCt)/a.averageCt : 0; a.category = a.currentCt < a.p25Ct ? "NIEDRIG" : (a.currentCt > a.p75Ct ? "HOCH" : "NORMAL");
  std::vector<PricePoint> all; for (size_t i=s.currentIndex;i<s.today.size();++i) all.push_back(s.today[i]); all.insert(all.end(),s.tomorrow.begin(),s.tomorrow.end()); size_t required=std::max<size_t>(1,(AppConfig::NEXT_WINDOW_MINUTES+s.intervalMinutes-1)/s.intervalMinutes);
  if (a.currentCt <= a.p25Ct) { size_t n=0; while(n<all.size() && all[n].totalEurPerKwh*100<=a.p25Ct) ++n; if(n) { time_t end=parseIso(all[n-1].startsAt)+s.intervalMinutes*60, now=time(nullptr); if(end>now && now>0) a.currentCheapRemainingHours=(end-now+3599)/3600; } }
  size_t run=0; for(size_t i=1;i<all.size();++i) { if(all[i].totalEurPerKwh*100<=a.p25Ct) { if(!run) a.nextCheapStart=all[i].startsAt; if(++run>=required) { time_t start=parseIso(a.nextCheapStart),now=time(nullptr); if(start>0&&now>0) a.nextCheapInHours=std::max(0,(int)((start-now+1800)/3600)); break; } } else { run=0; a.nextCheapStart=""; } } return a;
}

uint32_t nextSleepSeconds() {
  time_t now = time(nullptr);
  if (now < 1700000000) return AppConfig::FALLBACK_SLEEP_SECONDS;

  const uint32_t period = AppConfig::NETWORK_UPDATE_MINUTES * 60UL;
  time_t nextWake = now + period - (now % period) + AppConfig::UPDATE_OFFSET_SECONDS;

  // The normal next wake after the 00:01 update would be 01:01. Skip all
  // automatic wakes from 01:00 through 04:59 and resume at 05:01:30 local time.
  struct tm local;
  localtime_r(&nextWake, &local);
  if (local.tm_hour >= AppConfig::NIGHT_PAUSE_FIRST_HOUR &&
      local.tm_hour < AppConfig::NIGHT_PAUSE_RESUME_HOUR) {
    local.tm_hour = AppConfig::NIGHT_PAUSE_RESUME_HOUR;
    local.tm_min = AppConfig::UPDATE_OFFSET_SECONDS / 60;
    local.tm_sec = AppConfig::UPDATE_OFFSET_SECONDS % 60;
    local.tm_isdst = -1; // Let the Europe/Berlin timezone choose summer or winter time.
    nextWake = mktime(&local);
  }
  return nextWake > now ? static_cast<uint32_t>(nextWake - now) : AppConfig::FALLBACK_SLEEP_SECONDS;
}
void sleepFor(uint32_t seconds, bool enableTimerWakeup) {
  if (enableTimerWakeup) { esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL); }
  // pinMode(INPUT_PULLUP) haelt den Pull-Up nur im normalen Digitalbetrieb.
  // Fuer EXT0-Wakeup muss der Pull-Up ueber die RTC-GPIO-API gesetzt werden,
  // sonst kann der Pin waehrend des eigentlichen Deep Sleep floaten und durch
  // Stoerungen einen Spontan-Wakeup ohne Tastendruck ausloesen.
  rtc_gpio_pullup_en((gpio_num_t)REFRESH_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)REFRESH_BUTTON);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)REFRESH_BUTTON, 0);
  esp_deep_sleep_start();
}


KeyHoldResult readKeyHold() {
  if (digitalRead(REFRESH_BUTTON) != LOW) return KeyHoldResult::NONE;
  uint32_t start = millis();
  while (digitalRead(REFRESH_BUTTON) == LOW) {
    if (millis() - start >= AppConfig::FACTORY_RESET_HOLD_MS) return KeyHoldResult::FACTORY_RESET;
    delay(25);
  }
  uint32_t held = millis() - start;
  if (held >= 3000) return KeyHoldResult::MAINTENANCE_TOGGLE;
  return KeyHoldResult::SHORT_PRESS;
}
String suffix() { uint64_t mac=ESP.getEfuseMac(); char b[7]; snprintf(b,sizeof(b),"%06llX",mac&0xFFFFFFULL); return b; }

bool axpReadRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<uint8_t>(0x34), static_cast<uint8_t>(1)) != 1) return false;
  value = Wire.read();
  return true;
}

bool axpWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void initAxp2101() {
  uint8_t value = 0;
  if (axpReadRegister(0x00, value)) Serial.println("AXP2101 antwortet.");
  else Serial.println("AXP2101 nicht erreichbar.");

  for (uint8_t reg = 0x92; reg <= 0x95; ++reg) {
    if (axpReadRegister(reg, value)) {
      uint8_t updated = (value & 0xE0) | 0x1C;
      if (updated != value) axpWriteRegister(reg, updated);
    }
  }

  if (axpReadRegister(0x90, value)) axpWriteRegister(0x90, value | 0x0F);
  delay(10);
}



BatteryStatus readBatteryStatus() {
  BatteryStatus b;
  uint8_t hi = 0, lo = 0;
  if (!axpReadRegister(0x34, hi) || !axpReadRegister(0x35, lo)) return b;
  uint16_t raw = (static_cast<uint16_t>(hi) << 8) | lo; // 14-Bit-Rohwert lt. Spezifikation
  uint16_t mv = raw; // 1 LSB = 1 mV
  if (mv < 2500 || mv > 4500) return b; // Plausibilitaetsbereich
  int pct = (static_cast<int>(mv) - 3300) / 9;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  b.percent = static_cast<uint8_t>(pct);
  b.valid = true;
  uint8_t status = 0;
  if (axpReadRegister(0x01, status)) {
    uint8_t direction = (status >> 5) & 0x03; // Bits 6-5
    b.charging = (direction == 0x01);
    // Diagnose: Lade-Bitinterpretation stammt aus dem urspruenglichen Handoff
    // und wurde nie auf echter Hardware verifiziert. Rohwert + alle Bits
    // mitloggen, um die tatsaechliche Bitbelegung beim Laden pruefen zu koennen.
    Serial.printf("AXP2101 Reg 0x01=0x%02X Bits[7..0]=%d%d%d%d%d%d%d%d direction(Bits6-5)=%u charging=%d\n",
                  status,
                  (status >> 7) & 1, (status >> 6) & 1, (status >> 5) & 1, (status >> 4) & 1,
                  (status >> 3) & 1, (status >> 2) & 1, (status >> 1) & 1, status & 1,
                  direction, b.charging ? 1 : 0);
  }
  return b;
}

// Global lifetime: DisplayDriver's GxEPD2 buffer (~192 KB) must not live on
// the loopTask stack (default 8 KB) - see the stack-overflow fix in chat.
Storage storage;
DisplayDriver display;

void setup() {
  // Kein delay() nach Serial.begin(): im Batteriebetrieb haengt kein Rechner
  // am USB-Port, jede Wartezeit hier kostet nur unnoetig Strom bei jedem der
  // 24 Wakes/Tag. Native USB-CDC muss nicht "anlaufen" wie klassisches UART.
  Serial.begin(115200);

  pinMode(SYS_OUT_LATCH, OUTPUT);
  digitalWrite(SYS_OUT_LATCH, HIGH);

  const esp_reset_reason_t resetReason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();

  setenv("TZ", AppConfig::TIMEZONE, 1); tzset(); pinMode(REFRESH_BUTTON, INPUT_PULLUP);
  Wire.begin(PMIC_SDA, PMIC_SCL);
  Wire.setTimeOut(50);

  initAxp2101();

  // Bluetooth is unused. Keep it disabled before any network work.
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) esp_bt_controller_disable();

  bool maintenanceMode = storage.loadMaintenanceMode();
  const KeyHoldResult keyHold = readKeyHold();

  // Spontan-Wakeup: das Geraet ist per EXT0 (Taste) aufgewacht, aber es liegt
  // kein echter Tastendruck vor. Deutet auf einen Floating-Pin waehrend des
  // Deep Sleep hin (siehe rtc_gpio_pullup_en in sleepFor()).
  const bool spuriousExt0 = wakeupCause == ESP_SLEEP_WAKEUP_EXT0 && keyHold == KeyHoldResult::NONE;
  uint32_t spuriousCount = storage.recordWakeDiagnostics(static_cast<int>(resetReason), static_cast<int>(wakeupCause), spuriousExt0);
  Serial.printf("Boot: reset=%d wakeup=%d spontane-ext0-wakeups=%u\n", static_cast<int>(resetReason), static_cast<int>(wakeupCause), spuriousCount);

  switch (keyHold) {
    case KeyHoldResult::FACTORY_RESET:
      storage.eraseConfigAndSnapshot();
      Serial.println("Werksreset: Konfiguration und Cache geloescht.");
      break;
    case KeyHoldResult::MAINTENANCE_TOGGLE:
      maintenanceMode = !maintenanceMode;
      storage.saveMaintenanceMode(maintenanceMode);
      Serial.println(maintenanceMode ? "Wartungsmodus aktiviert." : "Wartungsmodus deaktiviert.");
      break;
    case KeyHoldResult::SHORT_PRESS:
      Serial.println("Kurzer Tastendruck: erzwinge Preisabruf unabhaengig vom Zeitfenster.");
      break;
    case KeyHoldResult::NONE:
      break;
  }
  const bool manualRefresh = keyHold == KeyHoldResult::SHORT_PRESS;
  RuntimeConfig cfg;
  if (!storage.loadConfig(cfg)) {
    String ap = String(AppConfig::AP_PREFIX) + suffix(), pw = String(AppConfig::AP_PASSWORD_PREFIX) + suffix();
    display.ensureReady();
    SetupPortal portal(storage, display);
    if (portal.run(ap, pw)) { display.renderPortalSaved(); delay(2000); ESP.restart(); }
    display.sleep(); sleepFor(AppConfig::FALLBACK_SLEEP_SECONDS, false);
  }
  PriceSnapshot snapshot; bool cached = storage.loadSnapshot(snapshot); PriceSnapshot previous = snapshot; String failure;

  // Tagespreise aendern sich nach Veroeffentlichung nicht mehr - Tibber muss
  // daher nicht bei jedem stuendlichen Wake kontaktiert werden. Entscheidung
  // rein anhand des Kalendertags: brauchen wir "heute" noch (fehlt oder
  // veraltet), und/oder ist "morgen" noch nicht da und das Nachmittagsfenster
  // schon erreicht?
  const time_t now = time(nullptr);
  const bool nowValid = now >= 1700000000;
  const String todayDate = nowValid ? localDateString(now) : "";
  String todayCachedDate = snapshot.today.empty() ? "" : localDateString(parseIso(snapshot.today[0].startsAt));

  if (nowValid && todayCachedDate != todayDate) {
    const String tomorrowCachedDate = snapshot.tomorrow.empty() ? "" : localDateString(parseIso(snapshot.tomorrow[0].startsAt));
    if (tomorrowCachedDate == todayDate) {
      // Was gestern Nachmittag als "morgen" geholt wurde, ist jetzt "heute" -
      // Tageswechsel kostet dadurch keine eigene Tibber-Verbindung.
      snapshot.today = snapshot.tomorrow;
      snapshot.tomorrow.clear();
      todayCachedDate = todayDate;
      Serial.println("Tageswechsel: Cache aus 'morgen' uebernommen, kein Abruf noetig.");
    }
  }

  const bool needToday = !nowValid || todayCachedDate != todayDate;
  const String tomorrowWantedDate = nowValid ? localDateString(now + 86400) : "";
  const String tomorrowCachedDateNow = snapshot.tomorrow.empty() ? "" : localDateString(parseIso(snapshot.tomorrow[0].startsAt));
  const bool needTomorrow = nowValid && tomorrowCachedDateNow != tomorrowWantedDate;
  struct tm nowLocal = {}; if (nowValid) localtime_r(&now, &nowLocal);
  const bool tomorrowWindowOpen = nowValid && nowLocal.tm_hour >= AppConfig::TOMORROW_FETCH_HOUR;
  const bool shouldConnect = needToday || (needTomorrow && tomorrowWindowOpen) || manualRefresh;

  if (shouldConnect) {
    TibberConnection network(storage);
    if (network.connect(cfg)) {
      time_t last = 0, nowAfterConnect = time(nullptr);
      if (nowAfterConnect < 1700000000 || !storage.loadLastTimeSync(last) || nowAfterConnect - last >= AppConfig::NTP_SYNC_INTERVAL_SECONDS) {
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
        uint32_t start = millis(); while (time(nullptr) < 1700000000 && millis() - start < 8000) delay(200);
        if (time(nullptr) >= 1700000000) storage.saveLastTimeSync(time(nullptr));
      }
      PriceSnapshot fresh; PriceProvider provider;
      if (provider.fetch(cfg, fresh, failure)) {
        snapshot = fresh;
        if (!cached || !storage.snapshotHasSamePrices(previous, fresh)) storage.saveSnapshot(snapshot);
      } else if (!needToday) {
        // Nur der Morgen-Abruf ist fehlgeschlagen, "heute" ist weiterhin
        // gueltig - das ist kein Offline-Zustand, einfach naechste Stunde
        // erneut versuchen.
        Serial.println("Morgen-Preise noch nicht verfuegbar oder Abruf fehlgeschlagen, naechster Versuch in einer Stunde.");
      }
      network.off();
    } else if (needToday) {
      snapshot.isStale = true; snapshot.staleReason = "wifi";
    } else {
      // WLAN-Verbindung fuer den Nachmittagsabruf fehlgeschlagen, "heute"
      // ist aber weiterhin gueltig - bisher lief das komplett unbemerkt durch.
      Serial.println("WLAN fuer Morgen-Abruf nicht erreichbar, naechster Versuch in einer Stunde.");
    }
  }

  cached = !snapshot.today.empty();
  if (cached) {
    // Wichtig: Zeit hier NEU auswerten, nicht die "nowValid"/"todayDate" von
    // oben wiederverwenden - falls die Uhr erst durch den NTP-Sync in diesem
    // Zyklus gueltig wurde, waere der alte Stand faelschlich noch "ungueltig"
    // und wuerde trotz frisch geholter Preise OFFLINE anzeigen.
    const time_t finalNow = time(nullptr);
    const bool finalNowValid = finalNow >= 1700000000;
    const String finalTodayDate = finalNowValid ? localDateString(finalNow) : "";
    const String cachedTodayDate = localDateString(parseIso(snapshot.today[0].startsAt));
    snapshot.isStale = !finalNowValid || cachedTodayDate != finalTodayDate;
    if (!snapshot.isStale) snapshot.staleReason = "";
    else if (snapshot.staleReason.isEmpty()) snapshot.staleReason = failure.isEmpty() ? "wifi" : failure;
    snapshot.currentIndex = computeCurrentIndex(snapshot);
  }

  // The display and its SPI bus are initialized only for an actual draw, never for a network-only wake.
  display.ensureReady();
  if (!cached) display.renderNoData(failure.isEmpty() ? "WLAN nicht erreichbar" : failure, maintenanceMode);
  else display.render(snapshot, assess(snapshot), maintenanceMode, readBatteryStatus());
  display.sleep();
  if (maintenanceMode) {
    Serial.println("Wartungsmodus aktiv - kein Deep Sleep. KEY 3 Sek. halten zum Beenden.");
    while (true) {
      if (digitalRead(REFRESH_BUTTON) == LOW) {
        uint32_t start = millis();
        while (digitalRead(REFRESH_BUTTON) == LOW) delay(25);
        uint32_t held = millis() - start;
        if (held >= 3000) {
          storage.saveMaintenanceMode(false);
          Serial.println("Wartungsmodus deaktiviert - Neustart.");
          ESP.restart();
        }
      }
      delay(50);
    }
  } else {
    sleepFor(nextSleepSeconds());
  }
}
void loop() {}
