// prayer_calc.h - prayer times for a microcontroller.
//
// A C++ port of the astronomy in family-frame's server/prayer.py, which is
// itself a port of batoulapps/adhan-js 4.4.6. It adds the other adhan-js
// methods (MWL, Egyptian, Karachi, Umm al-Qura, Dubai, Kuwait, Qatar,
// Singapore, Turkey, Tehran) with their constants copied from adhan-js.
// High-latitude rule: Middle of the Night, as in family-frame.
//
// Plain C++ (math.h only), so it compiles on the ESP32 and on a PC for tests.
// Uses double: the ESP32-S3 does this in software, but it runs once a day.
#pragma once
#include <math.h>
#include <stdint.h>

namespace prayer {

enum Index : uint8_t { FAJR, SUNRISE, DHUHR, ASR, MAGHRIB, ISHA, COUNT };

enum Method : uint8_t {
  ISNA, MWL, EGYPTIAN, KARACHI, UMM_AL_QURA, DUBAI, KUWAIT, QATAR,
  SINGAPORE, TURKEY, TEHRAN, METHOD_COUNT
};

struct MethodParams {
  const char *name;
  float fajrAngle, ishaAngle;
  uint8_t ishaInterval;   // minutes after maghrib, 0 = use ishaAngle
  float maghribAngle;     // 0 = maghrib at sunset
  int8_t adj[COUNT];      // minutes: fajr, sunrise, dhuhr, asr, maghrib, isha
  bool roundUp;           // Singapore rounds up; everyone else to nearest
};

// Constants from adhan-js src/CalculationMethod.ts.
static const MethodParams METHODS[METHOD_COUNT] = {
  {"ISNA",        15.0f, 15.0f,  0, 0.0f, {0,  0, 1, 0, 0, 0}, false},
  {"MWL",         18.0f, 17.0f,  0, 0.0f, {0,  0, 1, 0, 0, 0}, false},
  {"Egyptian",    19.5f, 17.5f,  0, 0.0f, {0,  0, 1, 0, 0, 0}, false},
  {"Karachi",     18.0f, 18.0f,  0, 0.0f, {0,  0, 1, 0, 0, 0}, false},
  {"Umm al-Qura", 18.5f,  0.0f, 90, 0.0f, {0,  0, 0, 0, 0, 0}, false},
  {"Dubai",       18.2f, 18.2f,  0, 0.0f, {0, -3, 3, 3, 3, 0}, false},
  {"Kuwait",      18.0f, 17.5f,  0, 0.0f, {0,  0, 0, 0, 0, 0}, false},
  {"Qatar",       18.0f,  0.0f, 90, 0.0f, {0,  0, 0, 0, 0, 0}, false},
  {"Singapore",   20.0f, 18.0f,  0, 0.0f, {0,  0, 1, 0, 0, 0}, true },
  {"Turkey",      18.0f, 17.0f,  0, 0.0f, {0, -7, 5, 4, 7, 0}, false},
  {"Tehran",      17.7f, 14.0f,  0, 4.5f, {0,  0, 0, 0, 0, 0}, false},
};

// The method most mosques use in a country (ISO code from the IP lookup).
// Anything not listed uses Muslim World League.
inline Method methodForCountry(const char *cc) {
  struct Map { const char *cc; Method m; };
  static const Map map[] = {
    {"US", ISNA}, {"CA", ISNA},
    {"SA", UMM_AL_QURA}, {"AE", DUBAI}, {"KW", KUWAIT}, {"QA", QATAR},
    {"SG", SINGAPORE}, {"MY", SINGAPORE}, {"ID", SINGAPORE}, {"BN", SINGAPORE},
    {"TR", TURKEY}, {"IR", TEHRAN},
    {"EG", EGYPTIAN}, {"SY", EGYPTIAN}, {"IQ", EGYPTIAN}, {"LB", EGYPTIAN},
    {"PK", KARACHI}, {"IN", KARACHI}, {"BD", KARACHI}, {"AF", KARACHI},
  };
  if (!cc) return MWL;
  for (const Map &e : map)
    if (e.cc[0] == cc[0] && e.cc[1] == cc[1]) return e.m;
  return MWL;
}

struct Times {
  bool valid;
  int64_t utc[COUNT];   // Unix seconds, rounded to the minute
};

// ---------------------------------------------------------------- internals
namespace detail {

inline double rad(double d) { return d * M_PI / 180.0; }
inline double deg(double r) { return r * 180.0 / M_PI; }
inline double normalize(double n, double m) { return n - m * floor(n / m); }
inline double unwind(double a) { return normalize(a, 360.0); }
inline double jsRound(double x) { return floor(x + 0.5); }
inline double quadrantShift(double a) {
  if (a >= -180 && a <= 180) return a;
  return a - 360 * jsRound(a / 360);
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian).
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

inline double julianDay(int year, int month, int day) {
  double Y = month > 2 ? year : year - 1;
  double M = month > 2 ? month : month + 12;
  double A = trunc(Y / 100);
  double B = trunc(2 - A + trunc(A / 4));
  double i0 = trunc(365.25 * (Y + 4716));
  double i1 = trunc(30.6001 * (M + 1));
  return i0 + i1 + day + B - 1524.5;
}

struct Solar { double decl, ra, sidereal; };

inline Solar solarCoordinates(double jd) {
  const double T = (jd - 2451545.0) / 36525;
  const double L0 = unwind(280.4664567 + 36000.76983 * T + 0.0003032 * T * T);
  const double Lp = unwind(218.3165 + 481267.8813 * T);
  const double Om = unwind(125.04452 - 1934.136261 * T + 0.0020708 * T * T + T * T * T / 450000);
  const double M = unwind(357.52911 + 35999.05029 * T - 0.0001537 * T * T);
  const double Mr = rad(M);
  const double C = (1.914602 - 0.004817 * T - 0.000014 * T * T) * sin(Mr) +
                   (0.019993 - 0.000101 * T) * sin(2 * Mr) + 0.000289 * sin(3 * Mr);
  const double Omega2 = 125.04 - 1934.136 * T;
  const double lambda = rad(unwind(L0 + C - 0.00569 - 0.00478 * sin(rad(Omega2))));
  const double JD = T * 36525 + 2451545.0;
  const double theta0 = unwind(280.46061837 + 360.98564736629 * (JD - 2451545) +
                               0.000387933 * T * T - T * T * T / 38710000);
  const double dPsi = -17.2 / 3600 * sin(rad(Om)) - 1.32 / 3600 * sin(2 * rad(L0)) -
                      0.23 / 3600 * sin(2 * rad(Lp)) + 0.21 / 3600 * sin(2 * rad(Om));
  const double dEps = 9.2 / 3600 * cos(rad(Om)) + 0.57 / 3600 * cos(2 * rad(L0)) +
                      0.1 / 3600 * cos(2 * rad(Lp)) - 0.09 / 3600 * cos(2 * rad(Om));
  const double eps0 = 23.439291 - 0.013004167 * T - 0.0000001639 * T * T + 0.0000005036 * T * T * T;
  const double epsApp = rad(eps0 + 0.00256 * cos(rad(125.04 - 1934.136 * T)));
  Solar s;
  s.decl = deg(asin(sin(epsApp) * sin(lambda)));
  s.ra = unwind(deg(atan2(cos(epsApp) * sin(lambda), cos(lambda))));
  s.sidereal = theta0 + dPsi * 3600 * cos(rad(eps0 + dEps)) / 3600;
  return s;
}

inline double interp(double y2, double y1, double y3, double n) {
  const double a = y2 - y1, b = y3 - y2, c = b - a;
  return y2 + n / 2 * (a + b + n * c);
}
inline double interpAngles(double y2, double y1, double y3, double n) {
  const double a = unwind(y2 - y1), b = unwind(y3 - y2), c = b - a;
  return y2 + n / 2 * (a + b + n * c);
}

struct SolarTime {
  double lat, lon, m0, transit, sunrise, sunset;
  Solar s, prev, next;

  SolarTime(int y, int mo, int d, double latitude, double longitude) : lat(latitude), lon(longitude) {
    const double jd = julianDay(y, mo, d);
    s = solarCoordinates(jd);
    prev = solarCoordinates(jd - 1);
    next = solarCoordinates(jd + 1);
    const double Lw = -lon;
    m0 = normalize((s.ra + Lw - s.sidereal) / 360, 1);
    const double expected = normalize((12.0 - lon / 15.0) / 24.0, 1);
    if (m0 - expected > 0.5) m0 -= 1.0;
    else if (expected - m0 > 0.5) m0 += 1.0;
    const double theta = unwind(s.sidereal + 360.985647 * m0);
    const double a = unwind(interpAngles(s.ra, prev.ra, next.ra, m0));
    const double H = quadrantShift(theta - Lw - a);
    transit = (m0 + H / -360) * 24;
    sunrise = hourAngle(-50.0 / 60.0, false);
    sunset = hourAngle(-50.0 / 60.0, true);
  }

  // Hours (UT) when the sun is at altitude h0; NaN if it never gets there.
  double hourAngle(double h0, bool afterTransit) const {
    const double Lw = -lon;
    const double t1 = sin(rad(h0)) - sin(rad(lat)) * sin(rad(s.decl));
    const double t2 = cos(rad(lat)) * cos(rad(s.decl));
    const double H0 = deg(acos(t1 / t2));
    const double m = afterTransit ? m0 + H0 / 360 : m0 - H0 / 360;
    const double theta = unwind(s.sidereal + 360.985647 * m);
    const double a = unwind(interpAngles(s.ra, prev.ra, next.ra, m));
    const double delta = interp(s.decl, prev.decl, next.decl, m);
    const double H = theta - Lw - a;
    const double h = deg(asin(sin(rad(lat)) * sin(rad(delta)) +
                              cos(rad(lat)) * cos(rad(delta)) * cos(rad(H))));
    const double dm = (h - h0) / (360 * cos(rad(delta)) * cos(rad(lat)) * sin(rad(H)));
    return (m + dm) * 24;
  }

  double afternoon(double shadow) const {
    const double inverse = shadow + tan(rad(fabs(lat - s.decl)));
    return hourAngle(deg(atan(1.0 / inverse)), true);
  }
};

// Seconds since the epoch for "hours after UT midnight of day0". NaN stays NaN.
inline double utcAt(int64_t day0, double hours) {
  if (isnan(hours)) return NAN;
  return (double)(day0 * 86400) + floor(hours * 3600);
}

inline int64_t roundMinute(double t, bool up) {
  const int64_t whole = (int64_t)floor(t);
  const int64_t sec = ((whole % 60) + 60) % 60;
  const int64_t off = up ? 60 - sec : (sec >= 30 ? 60 - sec : -sec);
  return whole + off;
}

}  // namespace detail

// Prayer times for the local civil date y-m-d at latitude/longitude.
// hanafi = true doubles the Asr shadow length.
inline Times compute(int y, int m, int d, double lat, double lon, Method method, bool hanafi) {
  using namespace detail;
  const MethodParams &p = METHODS[method < METHOD_COUNT ? method : MWL];
  Times out;
  out.valid = false;

  const int64_t day0 = daysFromCivil(y, m, d);
  const SolarTime st(y, m, d, lat, lon);
  // Tomorrow's civil date, for tomorrow's sunrise.
  int64_t day1 = day0 + 1;
  // Convert day1 back to y/m/d (Howard Hinnant's civil_from_days).
  int64_t z = day1 + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const int d1 = (int)(doy - (153 * mp + 2) / 5 + 1);
  const int m1 = (int)(mp < 10 ? mp + 3 : mp - 9);
  const int y1 = (int)(yoe + era * 400 + (m1 <= 2));
  const SolarTime tomorrow(y1, m1, d1, lat, lon);

  const double dhuhr = utcAt(day0, st.transit);
  const double sunrise = utcAt(day0, st.sunrise);
  const double sunset = utcAt(day0, st.sunset);
  const double asr = utcAt(day0, st.afternoon(hanafi ? 2 : 1));
  const double tomorrowSunrise = utcAt(day1, tomorrow.sunrise);
  if (isnan(dhuhr) || isnan(sunrise) || isnan(sunset) || isnan(asr) || isnan(tomorrowSunrise))
    return out;  // polar day or night: no sunrise or sunset at all
  const double night = tomorrowSunrise - sunset;

  double fajr = utcAt(day0, st.hourAngle(-p.fajrAngle, false));
  const double safeFajr = sunrise - 0.5 * night;
  if (isnan(fajr) || safeFajr > fajr || !(fajr < sunrise)) fajr = safeFajr;

  double isha;
  if (p.ishaInterval > 0) {
    isha = sunset + p.ishaInterval * 60.0;
  } else {
    isha = utcAt(day0, st.hourAngle(-p.ishaAngle, true));
    const double safeIsha = sunset + 0.5 * night;
    if (isnan(isha) || !(sunset < isha) || safeIsha < isha) isha = safeIsha;
  }

  double maghrib = sunset;
  if (p.maghribAngle > 0) {
    const double angled = utcAt(day0, st.hourAngle(-p.maghribAngle, true));
    if (!isnan(angled) && sunset < angled && isha > angled) maghrib = angled;
  }

  const double raw[COUNT] = {fajr, sunrise, dhuhr, asr, maghrib, isha};
  for (int i = 0; i < COUNT; i++)
    out.utc[i] = roundMinute(raw[i] + p.adj[i] * 60.0, p.roundUp);
  out.valid = true;
  for (int i = 1; i < COUNT; i++)
    if (out.utc[i] < out.utc[i - 1]) out.valid = false;
  return out;
}

}  // namespace prayer
