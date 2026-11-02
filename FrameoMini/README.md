# FrameoMini

This is a prayer-time dashboard for the ESP32-S3 handheld (ILI9341, 320×240). It is a cut-down version of the Today page in family-frame (github.com/jbradf0rd/photo-frame-dashboard).

## Setup

1. Arduino IDE: in **Boards Manager**, install **esp32** by Espressif. In **Library Manager**, install **LovyanGFX**.
2. Select **Tools → Board → ESP32S3 Dev Module**, and set **USB CDC On Boot** to Enabled.
3. Open `config.h` and enter your Wi-Fi name and password. Leave `USE_IP_LOCATION` on, or set your own coordinates.
4. Upload.

## Buttons

| Button | Action |
| --- | --- |
| SW1 | Switch Asr between Shafi and Hanafi |
| SW2 | Step to the next calculation method. Auto comes first. A `*` in the top bar means you chose the method yourself. |
| SW3 | Switch between 12 h and 24 h |
| Joystick click | Sync the time and location now |

Each setting change is saved and survives a restart.

## Screen

- **Top bar:** your region, followed by the calculation method and the Asr setting.
- **Left side:** the clock, the date, and the next prayer with a countdown to the second.
- **Right side:** today's times. The number next to each time shows how many minutes it moved compared with yesterday: orange means later, blue means earlier. Dhuhr is shown as Jumuah on Fridays.

## Files

- `prayer_calc.h`: the prayer-time maths. It is a C++ port of family-frame's `prayer.py` (adhan-js 4.4.6). It was checked against that file on 1,142 random dates, locations, methods and Asr settings, with every time matching to the minute.
- `config.h`: Wi-Fi, location, defaults and pins.
