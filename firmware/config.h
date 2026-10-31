// BLARE alarm clock - hardware and behaviour settings.
// Everything you are likely to want to tweak lives in this file.
#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pins - taken from pcb/blare.kicad_pcb (XIAO pin -> net)
// ---------------------------------------------------------------------------
// Screen (J1 header: GND, VCC, SCL, SDA, RST, DC, CS, BL)
#define TFT_SCLK D10  // GPIO10, "SCL" on the screen
#define TFT_MOSI D9   // GPIO9,  "SDA" on the screen
#define TFT_DC   D8   // GPIO8
#define TFT_CS   D7   // GPIO20
#define TFT_RST  -1   // tied to 3.3V on the PCB, so we use a software reset
// BL is tied to 3.3V on the PCB, so the firmware can't switch the backlight.

#define BUZZER_PIN D6 // GPIO21

// The six keyswitches, numbered as seen from the front of the clock:
//
//     [1] SW1   [2] SW3   [3] SW5
//     [4] SW2   [5] SW4   [6] SW6
//
// Each one shorts its pin to GND, so we use the internal pull-ups.
// Note: D0 (GPIO2) is a boot strapping pin - don't hold key 1 while plugging in.
static const uint8_t KEY_PINS[6] = { D0, D2, D4, D1, D3, D5 };

// What each key does (index into KEY_PINS, 0 = key 1). Swap these to remap.
#define KEY_MENU  0  // [1] open settings / cancel
#define KEY_UP    1  // [2] previous item / increase value
#define KEY_ALARM 2  // [3] alarm on/off (also cancels a snooze)
#define KEY_BACK  3  // [4] back
#define KEY_DOWN  4  // [5] next item / decrease value
#define KEY_OK    5  // [6] select / confirm / SNOOZE while ringing

// ---------------------------------------------------------------------------
// Screen (2.25" ST7789, 76x284) - values from the BLARE docs
// ---------------------------------------------------------------------------
#define SCREEN_W     284
#define SCREEN_H     76
#define TFT_COL_OFFSET 82
#define TFT_ROW_OFFSET 18
#define TFT_ROTATION 1        // landscape; use 3 if the picture is upside down
#define TFT_INVERT   false
#define TFT_SPI_HZ   20000000 // 20 MHz is safe over 20 cm jumper wires; try 40 MHz

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// POSIX time zone string, handles daylight saving automatically. Examples:
//   US Pacific  "PST8PDT,M3.2.0,M11.1.0"     US Eastern "EST5EDT,M3.2.0,M11.1.0"
//   US Central  "CST6CDT,M3.2.0,M11.1.0"     UK         "GMT0BST,M3.5.0/1,M10.5.0"
//   Central EU  "CET-1CEST,M3.5.0,M10.5.0/3" Ukraine    "EET-2EEST,M3.5.0/3,M10.5.0/4"
// More: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define TIMEZONE     "PST8PDT,M3.2.0,M11.1.0"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
// WiFi name/password go in secrets.h (copy secrets.example.h). Without WiFi,
// set the clock by hand from the menu - it resets when the power is cut.

// ---------------------------------------------------------------------------
// Alarm
// ---------------------------------------------------------------------------
#define BUZZER_IS_ACTIVE  false  // true if your buzzer beeps by itself on DC
#define BUZZER_FREQ       2700   // Hz, near the resonance of most piezos
#define KEY_CLICK         true   // tiny click on every key press
#define ALARM_MAX_RING_MIN 15    // stop ringing after this long
#define DISMISS_IDLE_MS   20000  // stop-code screen gives up after this

// Defaults used until you change them in the menu (then saved to flash)
#define DEFAULT_ALARM_HOUR   7
#define DEFAULT_ALARM_MINUTE 0
#define DEFAULT_SNOOZE_MIN   9
#define DEFAULT_CODE_LEN     4   // keys to type to stop the alarm, 0 = any key
#define MAX_CODE_LEN         6

// ---------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
#define COLOR_BG          rgb(0, 0, 0)
#define COLOR_DIGIT       rgb(255, 160, 0)  // the big clock digits
#define COLOR_DIGIT_GHOST rgb(36, 20, 0)    // unlit segments, = COLOR_BG to hide
#define COLOR_TEXT        rgb(255, 255, 255)
#define COLOR_DIM         rgb(120, 120, 120)
#define COLOR_ACCENT      rgb(0, 200, 255)
#define COLOR_GOOD        rgb(0, 220, 80)
#define COLOR_BAD         rgb(255, 0, 0)

// Night mode: darker red digits so the clock doesn't light up the room.
// Set both hours equal to disable.
#define NIGHT_START_HOUR  22
#define NIGHT_END_HOUR    7
#define COLOR_NIGHT_DIGIT rgb(140, 0, 0)
#define COLOR_NIGHT_GHOST rgb(20, 0, 0)
