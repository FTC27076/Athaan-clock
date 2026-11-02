// config.h - edit this file, then upload.
#pragma once

// ---- Wi-Fi (used at boot and once a day to set the clock) ----
#define WIFI_SSID     "Platinum"
#define WIFI_PASSWORD "maGhs1985"

// ---- Location ----
// true: find the city, coordinates and UTC offset from your internet
//       connection (ip-api.com). City-level accuracy, usually within 20 km,
//       which moves prayer times by about a minute.
// false: use the fixed values below (more accurate if you set your own coordinates).
#define USE_IP_LOCATION   true
#define HOME_PLACE        "Toronto, ON"
#define HOME_COUNTRY      "CA"
#define HOME_LAT          43.6532
#define HOME_LON          -79.3832
#define HOME_UTC_OFFSET_MIN  (-300)   // minutes from UTC, including daylight time

// ---- Prayer settings (both can be changed with the buttons; saved) ----
// METHOD_AUTO picks the usual method for the country (ISNA in the US and Canada).
// Or force one: prayer::ISNA, MWL, EGYPTIAN, KARACHI, UMM_AL_QURA, DUBAI,
// KUWAIT, QATAR, SINGAPORE, TURKEY, TEHRAN
#define METHOD_AUTO    255
#define DEFAULT_METHOD METHOD_AUTO
#define DEFAULT_HANAFI false          // false = Shafi/Maliki/Hanbali Asr, true = Hanafi
#define DEFAULT_24H    false

// ---- Display ----
#define BACKLIGHT_LEVEL 200           // 0-255

// ---- Pins: the ESP32-S3 handheld build doc ----
#define PIN_TFT_CS    10
#define PIN_TFT_DC    9
#define PIN_TFT_RST   14
#define PIN_TFT_MOSI  11
#define PIN_TFT_SCK   12
#define PIN_TFT_BL    21   // through Q1 (PNP): LOW = backlight on
#define PIN_BTN_A     5    // SW1: Shafi / Hanafi
#define PIN_BTN_B     6    // SW2: next calculation method (Auto first)
#define PIN_BTN_C     7    // SW3: 12 h / 24 h
#define PIN_JOY_SW    4    // joystick click: sync clock and location now
