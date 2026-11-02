// FrameoMini - a small prayer-time dashboard for the ESP32-S3 handheld.
//
// Based on the Today page of family-frame (photo-frame-dashboard), cut down
// to what fits a 320x240 screen: clock and date, your location, today's
// prayer times with the change from yesterday, and the next prayer with a
// live countdown.
//
// Why it is fast:
//  - Prayer times are computed on the chip once a day (about 20 ms), not fetched.
//  - Wi-Fi is on only at boot and for one sync a day, then switched off.
//  - Nothing is redrawn unless it changed: the clock once a minute, the
//    countdown once a second (into a small off-screen sprite, so no flicker),
//    the list and card only when the next prayer or the date changes.
//  - SPI at 40 MHz with DMA through LovyanGFX.
//
// Libraries: LovyanGFX (Library Manager). Board: ESP32S3 Dev Module.
// Edit config.h first.

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <time.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"
#include "prayer_calc.h"

// ------------------------------------------------------------------ display
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;

 public:
  LGFX() {
    auto b = bus.config();
    b.spi_host = SPI2_HOST;        // FSPI: IO10-12 are its native pins
    b.spi_mode = 0;
    b.freq_write = 40000000;       // drop to 27000000 if the screen shows noise
    b.freq_read = 16000000;
    b.spi_3wire = false;
    b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = PIN_TFT_SCK;
    b.pin_mosi = PIN_TFT_MOSI;
    b.pin_miso = -1;               // 8-pin module: no MISO
    b.pin_dc = PIN_TFT_DC;
    bus.config(b);
    panel.setBus(&bus);

    auto p = panel.config();
    p.pin_cs = PIN_TFT_CS;
    p.pin_rst = PIN_TFT_RST;
    p.pin_busy = -1;
    p.panel_width = 240;
    p.panel_height = 320;
    p.readable = false;
    p.bus_shared = false;
    panel.config(p);

    auto l = light.config();
    l.pin_bl = PIN_TFT_BL;
    l.invert = true;               // PNP high-side switch: LOW = on
    l.freq = 12000;
    l.pwm_channel = 7;
    light.config(l);
    panel.setLight(&light);

    setPanel(&panel);
  }
};

static LGFX tft;
static LGFX_Sprite clockSpr(&tft);
static LGFX_Sprite countSpr(&tft);

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// family-frame colours: cream page, black ink, dark green prayer card
static constexpr uint16_t BG         = rgb(242, 237, 230);
static constexpr uint16_t INK        = rgb(17, 17, 17);
static constexpr uint16_t MUTED      = rgb(120, 116, 110);
static constexpr uint16_t BAR_TEXT   = rgb(242, 237, 230);
static constexpr uint16_t BAR_MUTED  = rgb(170, 166, 160);
static constexpr uint16_t CARD       = rgb(17, 43, 36);
static constexpr uint16_t CARD_TEXT  = rgb(240, 236, 228);
static constexpr uint16_t CARD_MUTED = rgb(150, 178, 166);
static constexpr uint16_t LATER      = rgb(176, 84, 24);   // time moved later than yesterday
static constexpr uint16_t EARLIER    = rgb(36, 92, 196);   // time moved earlier

// Layout (landscape, 320 x 240)
static constexpr int BAR_H = 21;
static constexpr int LEFT_X = 6, LEFT_W = 168;
static constexpr int CLOCK_Y = 27, CLOCK_H = 50;
static constexpr int DATE_Y = 80;
static constexpr int CARD_Y = 104, CARD_H = 130;
static constexpr int LIST_X = 180, LIST_W = 136, LIST_Y = 36, ROW_H = 33;

// ------------------------------------------------------------------ state
struct Geo {
  char place[40];
  char cc[3];
  double lat, lon;
  int32_t offsetSec;    // UTC offset including daylight time
};

static Geo geo;
static Preferences prefs;
static uint8_t methodSetting = DEFAULT_METHOD;   // METHOD_AUTO or a prayer::Method
static bool hanafi = DEFAULT_HANAFI;
static bool use24h = DEFAULT_24H;
static bool online = true;                       // last sync reached the internet

static prayer::Times yday, today, tmrw;
static long shownYmd = -1;        // local date the times are for (yyyymmdd)
static int nextIdx = -1;          // index into today/tmrw
static bool nextTomorrow = false;
static int64_t nextAt = 0;
static int shownMinute = -1;
static time_t lastSecond = 0;
static long lastSyncYmd = -1;
static volatile bool ntpDone = false;

static const char *const NAMES[prayer::COUNT] = {"Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha"};
static const prayer::Index PRAYERS[] = {prayer::FAJR, prayer::DHUHR, prayer::ASR, prayer::MAGHRIB, prayer::ISHA};

// ------------------------------------------------------------------ helpers
static prayer::Method method() {
  return methodSetting == METHOD_AUTO ? prayer::methodForCountry(geo.cc) : (prayer::Method)methodSetting;
}

static time_t localNow() { return time(nullptr) + geo.offsetSec; }

static void localTm(int64_t localSecs, struct tm &out) {
  time_t t = (time_t)localSecs;
  gmtime_r(&t, &out);
}

static long ymdOf(const struct tm &t) {
  return (t.tm_year + 1900) * 10000L + (t.tm_mon + 1) * 100L + t.tm_mday;
}

static const char *prayerName(int idx, bool isTomorrow) {
  if (idx == prayer::DHUHR) {
    struct tm t;
    localTm(localNow() + (isTomorrow ? 86400 : 0), t);
    if (t.tm_wday == 5) return "Jumuah";
  }
  return NAMES[idx];
}

// "5:49" or "17:49"; adds " AM"/" PM" in 12-hour mode when ampm is true.
static void fmtClock(int64_t utc, char *buf, size_t n, bool ampm) {
  struct tm t;
  localTm(utc + geo.offsetSec, t);
  if (use24h) {
    snprintf(buf, n, "%d:%02d", t.tm_hour, t.tm_min);
  } else {
    int h = t.tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, n, ampm ? "%d:%02d %s" : "%d:%02d", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  }
}

// UTF-8 -> plain ASCII for the built-in fonts ("Montréal" -> "Montreal").
static void toAscii(const char *in, char *out, size_t n) {
  static const char LATIN1[] =
      "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"   // U+00C0..U+00DF
      "aaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00E0..U+00FF
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < n; i++) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x80) {
      out[o++] = (char)c;
    } else if (c == 0xC3 && in[i + 1]) {
      uint8_t c2 = (uint8_t)in[++i];
      if (c2 >= 0x80 && c2 <= 0xBF) out[o++] = LATIN1[c2 - 0x80];
    } else {
      while ((((uint8_t)in[i + 1]) & 0xC0) == 0x80) i++;   // skip other characters
    }
  }
  out[o] = 0;
}

// ------------------------------------------------------------------ settings
static void loadSettings() {
  prefs.begin("frameo", true);
  methodSetting = prefs.getUChar("method", DEFAULT_METHOD);
  hanafi = prefs.getBool("hanafi", DEFAULT_HANAFI);
  use24h = prefs.getBool("h24", DEFAULT_24H);
  bool cached = prefs.isKey("lat");
  if (cached && USE_IP_LOCATION) {
    geo.lat = prefs.getDouble("lat", HOME_LAT);
    geo.lon = prefs.getDouble("lon", HOME_LON);
    geo.offsetSec = prefs.getInt("off", HOME_UTC_OFFSET_MIN * 60);
    prefs.getString("place", geo.place, sizeof geo.place);
    prefs.getString("cc", geo.cc, sizeof geo.cc);
  }
  prefs.end();
  if (!cached || !USE_IP_LOCATION) {
    strlcpy(geo.place, HOME_PLACE, sizeof geo.place);
    strlcpy(geo.cc, HOME_COUNTRY, sizeof geo.cc);
    geo.lat = HOME_LAT;
    geo.lon = HOME_LON;
    geo.offsetSec = HOME_UTC_OFFSET_MIN * 60;
  }
}

static void saveSettings() {
  prefs.begin("frameo", false);
  prefs.putUChar("method", methodSetting);
  prefs.putBool("hanafi", hanafi);
  prefs.putBool("h24", use24h);
  prefs.end();
}

static void saveGeo() {
  prefs.begin("frameo", false);
  prefs.putDouble("lat", geo.lat);
  prefs.putDouble("lon", geo.lon);
  prefs.putInt("off", geo.offsetSec);
  prefs.putString("place", geo.place);
  prefs.putString("cc", geo.cc);
  prefs.end();
}

// ------------------------------------------------------------------ network
static void onNtp(struct timeval *) { ntpDone = true; }

static bool jsonString(const String &s, const char *key, char *out, size_t n) {
  String k = String("\"") + key + "\":\"";
  int i = s.indexOf(k);
  if (i < 0) return false;
  i += k.length();
  int j = s.indexOf('"', i);
  if (j < 0) return false;
  strlcpy(out, s.substring(i, j).c_str(), n);
  return true;
}

static bool jsonNumber(const String &s, const char *key, double &out) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k);
  if (i < 0) return false;
  out = strtod(s.c_str() + i + k.length(), nullptr);
  return true;
}

// City, coordinates and current UTC offset from the public IP (ip-api.com,
// free, no key; plain HTTP, which is quicker than TLS on the ESP32).
static bool fetchGeo() {
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin("http://ip-api.com/json/?fields=status,countryCode,region,city,lat,lon,offset")) return false;
  int code = http.GET();
  String body = code == 200 ? http.getString() : String();
  http.end();
  if (body.indexOf("\"success\"") < 0) return false;

  double lat, lon, off;
  char city[40] = "", region[16] = "", cc[3] = "";
  if (!jsonNumber(body, "lat", lat) || !jsonNumber(body, "lon", lon) || !jsonNumber(body, "offset", off)) return false;
  jsonString(body, "city", city, sizeof city);
  jsonString(body, "region", region, sizeof region);
  jsonString(body, "countryCode", cc, sizeof cc);

  char raw[64];
  if (region[0] && strlen(region) <= 3) snprintf(raw, sizeof raw, "%s, %s", city, region);
  else snprintf(raw, sizeof raw, "%s", city);
  toAscii(raw, geo.place, sizeof geo.place);
  strlcpy(geo.cc, cc, sizeof geo.cc);
  geo.lat = lat;
  geo.lon = lon;
  geo.offsetSec = (int32_t)off;
  return true;
}

static void drawStatus(const char *note);

// Wi-Fi on, set the clock, refresh the location, Wi-Fi off. 3-15 s.
static void syncNow() {
  drawStatus("syncing...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);

  online = WiFi.status() == WL_CONNECTED;
  if (online) {
    ntpDone = false;
    sntp_set_time_sync_notification_cb(onNtp);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    t0 = millis();
    while (!ntpDone && millis() - t0 < 10000) delay(50);
    if (USE_IP_LOCATION && fetchGeo()) saveGeo();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  struct tm t;
  localTm(localNow(), t);
  lastSyncYmd = ymdOf(t);
}

static bool clockValid() { return time(nullptr) > 1700000000; }

// ------------------------------------------------------------------ prayer logic
static void recompute() {
  struct tm t;
  localTm(localNow(), t);
  shownYmd = ymdOf(t);
  const int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
  struct tm yd, tm1;
  localTm(localNow() - 86400, yd);
  localTm(localNow() + 86400, tm1);
  const prayer::Method meth = method();
  today = prayer::compute(y, m, d, geo.lat, geo.lon, meth, hanafi);
  yday = prayer::compute(yd.tm_year + 1900, yd.tm_mon + 1, yd.tm_mday, geo.lat, geo.lon, meth, hanafi);
  tmrw = prayer::compute(tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday, geo.lat, geo.lon, meth, hanafi);
}

static void findNext() {
  const int64_t now = time(nullptr);
  nextIdx = -1;
  if (today.valid) {
    for (prayer::Index i : PRAYERS)
      if (today.utc[i] > now) { nextIdx = i; nextTomorrow = false; nextAt = today.utc[i]; return; }
  }
  if (tmrw.valid) { nextIdx = prayer::FAJR; nextTomorrow = true; nextAt = tmrw.utc[prayer::FAJR]; }
}

// ------------------------------------------------------------------ drawing
static void drawStatus(const char *note) {
  tft.fillRect(0, 0, 320, BAR_H, INK);
  tft.setFont(&fonts::Font2);

  char right[32];
  if (note) snprintf(right, sizeof right, "%s", note);
  else snprintf(right, sizeof right, "%s%s - %s", methodSetting == METHOD_AUTO ? "" : "* ",
                prayer::METHODS[method()].name, hanafi ? "Hanafi" : "Shafi");
  tft.setTextDatum(top_right);
  tft.setTextColor(BAR_MUTED, INK);
  tft.drawString(right, 314, 3);
  const int room = 314 - tft.textWidth(right) - 14;

  char left[48];
  snprintf(left, sizeof left, "%s%s%s%s", geo.place, geo.cc[0] ? ", " : "", geo.cc, online ? "" : " (offline)");
  while (strlen(left) > 1 && tft.textWidth(left) > room) left[strlen(left) - 1] = 0;
  tft.setTextDatum(top_left);
  tft.setTextColor(BAR_TEXT, INK);
  tft.drawString(left, 6, 3);
}

static void drawClock() {
  struct tm t;
  localTm(localNow(), t);
  int h = t.tm_hour;
  if (!use24h) { h %= 12; if (h == 0) h = 12; }
  char buf[8];
  snprintf(buf, sizeof buf, "%d:%02d", h, t.tm_min);

  clockSpr.fillScreen(BG);
  clockSpr.setFont(&fonts::Font7);
  clockSpr.setTextColor(INK, BG);
  clockSpr.setTextDatum(top_left);
  clockSpr.drawString(buf, 0, 1);
  if (!use24h) {
    clockSpr.setFont(&fonts::Font2);
    clockSpr.setTextDatum(top_right);
    clockSpr.drawString(t.tm_hour < 12 ? "AM" : "PM", LEFT_W - 1, 2);
  }
  clockSpr.pushSprite(LEFT_X, CLOCK_Y);
}

static void drawDate() {
  static const char *const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct tm t;
  localTm(localNow(), t);
  char buf[32];
  snprintf(buf, sizeof buf, "%s, %s %d", DAYS[t.tm_wday], MONTHS[t.tm_mon], t.tm_mday);
  tft.fillRect(LEFT_X, DATE_Y, LEFT_W, 20, BG);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(INK, BG);
  tft.setTextDatum(top_left);
  tft.drawString(buf, LEFT_X + 2, DATE_Y + 2);
}

static void drawCard() {
  tft.fillRoundRect(LEFT_X, CARD_Y, LEFT_W, CARD_H, 8, CARD);
  tft.setTextDatum(top_left);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(CARD_MUTED, CARD);
  tft.drawString("NEXT PRAYER", LEFT_X + 10, CARD_Y + 8);
  if (nextIdx < 0) {
    tft.setTextColor(CARD_TEXT, CARD);
    tft.drawString("No times for this", LEFT_X + 10, CARD_Y + 40);
    tft.drawString("location today", LEFT_X + 10, CARD_Y + 58);
    return;
  }
  tft.setFont(&fonts::FreeSerifBold18pt7b);
  tft.setTextColor(CARD_TEXT, CARD);
  tft.drawString(prayerName(nextIdx, nextTomorrow), LEFT_X + 10, CARD_Y + 28);

  char t[16], line[32];
  fmtClock(nextAt, t, sizeof t, true);
  snprintf(line, sizeof line, nextTomorrow ? "tomorrow at %s" : "at %s", t);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(CARD_MUTED, CARD);
  tft.drawString(line, LEFT_X + 10, CARD_Y + 64);
  tft.drawString("starts in", LEFT_X + 10, CARD_Y + 80);
}

static void drawCountdown() {
  countSpr.fillScreen(CARD);
  if (nextIdx >= 0) {
    int64_t s = nextAt - time(nullptr);
    if (s < 0) s = 0;
    char buf[20];
    if (s >= 3600) snprintf(buf, sizeof buf, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(buf, sizeof buf, "%d:%02d", (int)(s / 60), (int)(s % 60));
    countSpr.setFont(&fonts::Font4);
    countSpr.setTextColor(CARD_TEXT, CARD);
    countSpr.setTextDatum(top_left);
    countSpr.drawString(buf, 0, 1);
  }
  countSpr.pushSprite(LEFT_X + 10, CARD_Y + CARD_H - 34);
}

static void drawList() {
  tft.fillRect(LIST_X - 2, BAR_H + 1, LIST_W + 4, 240 - BAR_H - 1, BG);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(MUTED, BG);
  tft.setTextDatum(top_left);
  tft.drawString("TODAY", LIST_X + 8, BAR_H + 5);
  tft.setTextDatum(top_right);
  tft.drawString("VS YESTERDAY", LIST_X + LIST_W - 4, BAR_H + 5);

  if (!today.valid) return;
  for (int i = 0; i < prayer::COUNT; i++) {
    const int y = LIST_Y + i * ROW_H, mid = y + ROW_H / 2 - 1;
    const bool isNext = !nextTomorrow && i == nextIdx;
    const bool isSunrise = i == prayer::SUNRISE;
    const uint16_t bg = isNext ? CARD : BG;
    if (isNext) tft.fillRoundRect(LIST_X, y, LIST_W, ROW_H - 2, 6, CARD);

    tft.setFont(&fonts::Font2);
    tft.setTextDatum(middle_left);
    tft.setTextColor(isNext ? CARD_TEXT : (isSunrise ? MUTED : INK), bg);
    tft.drawString(prayerName(i, false), LIST_X + 8, mid);

    char t[12];
    fmtClock(today.utc[i], t, sizeof t, false);
    tft.setFont(&fonts::FreeSansBold9pt7b);
    tft.setTextDatum(middle_right);
    tft.drawString(t, LIST_X + LIST_W - 30, mid);

    if (yday.valid) {
      const int diff = (int)((today.utc[i] - yday.utc[i] - 86400) / 60);
      char dbuf[8];
      snprintf(dbuf, sizeof dbuf, diff > 0 ? "+%d" : "%d", diff);
      tft.setFont(&fonts::Font2);
      tft.setTextColor(isNext ? CARD_MUTED : (diff > 0 ? LATER : diff < 0 ? EARLIER : MUTED), bg);
      tft.drawString(dbuf, LIST_X + LIST_W - 5, mid);
    }
  }
}

static void drawAll() {
  tft.fillScreen(BG);
  drawStatus(nullptr);
  drawClock();
  drawDate();
  drawCard();
  drawCountdown();
  drawList();
  struct tm t;
  localTm(localNow(), t);
  shownMinute = t.tm_min;
}

static void drawMessage(const char *a, const char *b) {
  tft.fillScreen(BG);
  tft.setTextDatum(middle_center);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(INK, BG);
  tft.drawString(a, 160, 108);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(MUTED, BG);
  tft.drawString(b, 160, 134);
}

// Recompute and redraw everything (after a sync or a settings change).
static void refresh() {
  recompute();
  findNext();
  drawAll();
}

// ------------------------------------------------------------------ buttons
static const uint8_t BUTTON_PINS[4] = {PIN_BTN_A, PIN_BTN_B, PIN_BTN_C, PIN_JOY_SW};
static bool buttonLevel[4] = {true, true, true, true};
static uint32_t buttonChangedAt[4] = {0, 0, 0, 0};

// True once per press of button i (active LOW, 40 ms debounce).
static bool pressed(uint8_t i) {
  const bool v = digitalRead(BUTTON_PINS[i]);
  if (v != buttonLevel[i] && millis() - buttonChangedAt[i] > 40) {
    buttonLevel[i] = v;
    buttonChangedAt[i] = millis();
    return !v;
  }
  return false;
}

static void handleButtons() {
  bool changed = false;
  if (pressed(0)) { hanafi = !hanafi; changed = true; }
  if (pressed(1)) {
    if (methodSetting == METHOD_AUTO) methodSetting = 0;
    else if (methodSetting + 1 >= prayer::METHOD_COUNT) methodSetting = METHOD_AUTO;
    else methodSetting++;
    changed = true;
  }
  if (pressed(2)) { use24h = !use24h; changed = true; }
  if (pressed(3)) { syncNow(); refresh(); }
  if (changed) { saveSettings(); refresh(); }
}

// ------------------------------------------------------------------ main
void setup() {
  for (uint8_t pin : BUTTON_PINS) pinMode(pin, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(BACKLIGHT_LEVEL);
  clockSpr.createSprite(LEFT_W, CLOCK_H);
  countSpr.createSprite(LEFT_W - 20, 30);

  loadSettings();
  drawMessage("Connecting...", WIFI_SSID);
  syncNow();
  if (!clockValid()) drawMessage("No time yet", "Check Wi-Fi in config.h; retrying");
  else refresh();
}

void loop() {
  handleButtons();

  const time_t now = time(nullptr);
  if (now == lastSecond) { delay(10); return; }
  lastSecond = now;

  if (!clockValid()) {               // never synced: retry once a minute
    if (now % 60 == 0) { syncNow(); if (clockValid()) refresh(); }
    return;
  }

  struct tm t;
  localTm(localNow(), t);

  if (ymdOf(t) != shownYmd) { refresh(); return; }              // midnight
  if (t.tm_hour == 3 && t.tm_min == 17 && lastSyncYmd != ymdOf(t)) { syncNow(); refresh(); return; }

  if (nextIdx >= 0 && now >= nextAt) {                         // a prayer time arrived
    findNext();
    drawCard();
    drawList();
  }
  if (t.tm_min != shownMinute) { shownMinute = t.tm_min; drawClock(); }
  drawCountdown();
}
