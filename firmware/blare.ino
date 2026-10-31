// BLARE alarm clock firmware - Seeed XIAO ESP32C3 + 2.25" ST7789 (76x284)
//
// Features
//  - Big 7-segment style clock with seconds, date and alarm status
//  - Time from the internet (WiFi + NTP) or set by hand from the menu
//  - Alarm with repeat (every day / weekdays / weekends / once) and snooze
//  - To stop the alarm you type a random key code - no sleepy dismissing!
//  - Night mode dims the digits; settings are saved to flash
//
// Board: "XIAO_ESP32C3". Libraries: Adafruit GFX, Adafruit ST7735 and ST7789.
// Pins and options are in config.h, WiFi in secrets.h (see secrets.example.h).

#include <Adafruit_GFX.h>    // graphics library
#include <Adafruit_ST7789.h> // driver for the ST7789 screen
#include <SPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

// ===========================================================================
// Screen
// ===========================================================================

// Exposes the column/row offsets the library keeps protected (from the docs).
// Uses hardware SPI on our own pins, which is much faster than software SPI.
class BlareTFT : public Adafruit_ST7789 {
public:
  BlareTFT() : Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST) {}
  void setOffsets(uint8_t col, uint8_t row) {
    _colstart = _colstart2 = col;
    _rowstart = _rowstart2 = row;
  }
};

BlareTFT tft;
// Everything is drawn into this RAM copy of the screen first and then sent in
// one go, so the display never flickers.
GFXcanvas16 canvas(SCREEN_W, SCREEN_H);

// ===========================================================================
// Types (up here so Arduino's auto-generated prototypes can see them)
// ===========================================================================

struct Note {
  uint16_t freq; // 0 = silence
  uint16_t ms;
};

struct Key {
  bool down;          // debounced state
  bool raw;           // last reading
  uint32_t changedAt; // when raw last changed
  uint32_t repeatAt;  // next auto-repeat while held
};

struct Segment {    // one piece of a menu value, e.g. "07" ":" "30"
  char text[12];
  int8_t field;     // which editVal this shows, -1 = label only
};

// ===========================================================================
// State
// ===========================================================================

enum Repeat : uint8_t { REPEAT_DAILY, REPEAT_WEEKDAYS, REPEAT_WEEKENDS, REPEAT_ONCE, REPEAT_COUNT };
const char *const REPEAT_NAMES[REPEAT_COUNT] = { "Every day", "Weekdays", "Weekends", "Once" };

struct Settings {
  uint8_t alarmHour = DEFAULT_ALARM_HOUR;
  uint8_t alarmMinute = DEFAULT_ALARM_MINUTE;
  bool alarmOn = false;
  uint8_t repeat = REPEAT_DAILY;
  uint8_t snoozeMin = DEFAULT_SNOOZE_MIN;
  uint8_t codeLen = DEFAULT_CODE_LEN;
  bool use24h = true;
} settings;

Preferences prefs;

enum Screen : uint8_t { SCR_CLOCK, SCR_MENU, SCR_RINGING, SCR_DISMISS };
Screen screen = SCR_CLOCK;

enum MenuItem : uint8_t {
  MI_ALARM_TIME, MI_ALARM_ON, MI_REPEAT, MI_SNOOZE, MI_CODE,
  MI_FORMAT, MI_SET_TIME, MI_SET_DATE, MI_SYNC, MI_TEST, MI_COUNT
};
const char *const MENU_NAMES[MI_COUNT] = {
  "Alarm time", "Alarm", "Repeat", "Snooze", "Stop code",
  "Clock format", "Set time", "Set date", "Sync time", "Test alarm"
};

uint8_t menuItem = 0;
bool editing = false;
uint8_t editField = 0;
int editVal[3];
uint32_t lastKeyAt = 0;

time_t snoozeUntil = 0;      // 0 = not snoozed
int32_t lastAlarmMinute = -1; // stops the alarm firing twice in the same minute
uint32_t ringStartedAt = 0;

uint8_t code[MAX_CODE_LEN];
uint8_t codePos = 0;
uint32_t wrongFlashUntil = 0;

char toastText[24] = "";
uint32_t toastUntil = 0;

volatile time_t lastSync = 0; // set by the NTP callback
bool redraw = true;

const char *const DAY_NAMES[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
const char *const MONTH_NAMES[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

// ===========================================================================
// Settings storage
// ===========================================================================

void loadSettings() {
  prefs.begin("blare", true);
  settings.alarmHour = prefs.getUChar("alarmH", settings.alarmHour);
  settings.alarmMinute = prefs.getUChar("alarmM", settings.alarmMinute);
  settings.alarmOn = prefs.getBool("alarmOn", settings.alarmOn);
  settings.repeat = prefs.getUChar("repeat", settings.repeat);
  settings.snoozeMin = prefs.getUChar("snooze", settings.snoozeMin);
  settings.codeLen = min<uint8_t>(prefs.getUChar("codeLen", settings.codeLen), MAX_CODE_LEN);
  settings.use24h = prefs.getBool("use24h", settings.use24h);
  prefs.end();
}

void saveSettings() {
  prefs.begin("blare", false);
  prefs.putUChar("alarmH", settings.alarmHour);
  prefs.putUChar("alarmM", settings.alarmMinute);
  prefs.putBool("alarmOn", settings.alarmOn);
  prefs.putUChar("repeat", settings.repeat);
  prefs.putUChar("snooze", settings.snoozeMin);
  prefs.putUChar("codeLen", settings.codeLen);
  prefs.putBool("use24h", settings.use24h);
  prefs.end();
}

// ===========================================================================
// Time
// ===========================================================================

bool timeValid() {
  return time(nullptr) > 1700000000; // anything before Nov 2023 means "not set"
}

bool localNow(struct tm &t) {
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return timeValid();
}

// Current local time, or the date this firmware was built if the clock is unset.
void localNowOrBuildDate(struct tm &t) {
  if (localNow(t)) return;
  t = {};
  char mon[4];
  int day, year;
  sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  t.tm_mon = (strstr(months, mon) - months) / 3;
  t.tm_mday = day;
  t.tm_year = year - 1900;
  t.tm_hour = 12;
}

void setClock(int year, int month, int day, int hour, int minute, int second) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  t.tm_isdst = -1; // let the time zone rules work out daylight saving
  struct timeval tv = { mktime(&t), 0 };
  settimeofday(&tv, nullptr);
  lastAlarmMinute = -1;
  Serial.printf("Clock set by hand to %04d-%02d-%02d %02d:%02d\n", year, month, day, hour, minute);
}

int daysInMonth(int year, int month) {
  static const uint8_t DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return DAYS[month - 1] + (month == 2 && leap ? 1 : 0);
}

void onTimeSync(struct timeval *tv) {
  lastSync = tv->tv_sec;
}

bool wifiConfigured() {
  return strlen(WIFI_SSID) > 0;
}

void startWiFi() {
  if (!wifiConfigured()) return;
  Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // SNTP keeps re-syncing in the background (hourly) once WiFi is up.
  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
}

// Formats an hour for display, e.g. "07" or " 7" in 12h mode.
void formatHour(char *out, int hour) {
  if (settings.use24h) {
    sprintf(out, "%02d", hour);
  } else {
    int h = hour % 12;
    sprintf(out, "%2d", h == 0 ? 12 : h);
  }
}

// ===========================================================================
// Buzzer - plays a looping or one-shot pattern without blocking loop()
// ===========================================================================

const Note ALARM_PATTERN[] = {
  { BUZZER_FREQ, 90 }, { 0, 80 }, { BUZZER_FREQ, 90 }, { 0, 80 },
  { BUZZER_FREQ, 90 }, { 0, 80 }, { BUZZER_FREQ, 90 }, { 0, 600 },
};
const Note CLICK_PATTERN[] = { { 4000, 6 } };
const Note GOOD_PATTERN[] = { { 2000, 70 }, { 0, 30 }, { 3000, 70 }, { 0, 30 }, { 4000, 120 } };
const Note WRONG_PATTERN[] = { { 600, 250 } };

const Note *soundPattern = nullptr;
uint8_t soundLen = 0, soundIndex = 0;
bool soundLoop = false;
uint32_t noteStartedAt = 0;

void buzzer(uint16_t freq) {
#if BUZZER_IS_ACTIVE
  digitalWrite(BUZZER_PIN, freq ? HIGH : LOW);
#else
  if (freq) tone(BUZZER_PIN, freq);
  else noTone(BUZZER_PIN);
#endif
}

void playSound(const Note *pattern, uint8_t len, bool loop) {
  soundPattern = pattern;
  soundLen = len;
  soundIndex = 0;
  soundLoop = loop;
  noteStartedAt = millis();
  buzzer(pattern[0].freq);
}
#define PLAY(p, loop) playSound(p, sizeof(p) / sizeof(p[0]), loop)

void stopSound() {
  soundPattern = nullptr;
  buzzer(0);
}

void updateSound() {
  if (!soundPattern) return;
  if (millis() - noteStartedAt < soundPattern[soundIndex].ms) return;
  noteStartedAt = millis();
  if (++soundIndex >= soundLen) {
    if (!soundLoop) {
      stopSound();
      return;
    }
    soundIndex = 0;
  }
  buzzer(soundPattern[soundIndex].freq);
}

// ===========================================================================
// Alarm logic
// ===========================================================================

void toast(const char *text, uint32_t ms = 1800) {
  strncpy(toastText, text, sizeof(toastText) - 1);
  toastUntil = millis() + ms;
  redraw = true;
}

bool alarmToday(const struct tm &t) {
  bool weekend = t.tm_wday == 0 || t.tm_wday == 6;
  switch (settings.repeat) {
    case REPEAT_WEEKDAYS: return !weekend;
    case REPEAT_WEEKENDS: return weekend;
    default: return true;
  }
}

void startRinging() {
  if (screen == SCR_RINGING || screen == SCR_DISMISS) return;
  Serial.println("Alarm!");
  screen = SCR_RINGING;
  editing = false;
  ringStartedAt = millis();
  PLAY(ALARM_PATTERN, true);
  redraw = true;
}

void stopRinging(const char *message) {
  stopSound();
  screen = SCR_CLOCK;
  toast(message, 3000);
}

void snooze() {
  snoozeUntil = time(nullptr) + settings.snoozeMin * 60;
  Serial.printf("Snoozing for %d min\n", settings.snoozeMin);
  stopRinging("Snoozing... zZ");
}

void dismiss() {
  snoozeUntil = 0;
  Serial.println("Alarm dismissed");
  stopRinging("Good morning!");
  PLAY(GOOD_PATTERN, false);
}

void newCode() {
  for (uint8_t i = 0; i < settings.codeLen; i++) {
    do {
      code[i] = random(1, 7); // keys 1..6
    } while (i > 0 && code[i] == code[i - 1]); // no repeats, they're confusing
  }
  codePos = 0;
}

void checkAlarm() {
  struct tm t;
  if (!localNow(t)) return;

  if (snoozeUntil && time(nullptr) >= snoozeUntil) {
    snoozeUntil = 0;
    startRinging();
  }

  int32_t minuteStamp = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;
  if (settings.alarmOn && minuteStamp != lastAlarmMinute &&
      t.tm_hour == settings.alarmHour && t.tm_min == settings.alarmMinute && alarmToday(t)) {
    lastAlarmMinute = minuteStamp;
    if (settings.repeat == REPEAT_ONCE) {
      settings.alarmOn = false;
      saveSettings();
    }
    startRinging();
  }

  if ((screen == SCR_RINGING || screen == SCR_DISMISS) &&
      millis() - ringStartedAt > ALARM_MAX_RING_MIN * 60000UL) {
    stopRinging("Alarm timed out");
  }
}

// ===========================================================================
// Menu
// ===========================================================================

uint8_t fieldCount(uint8_t item) {
  switch (item) {
    case MI_ALARM_TIME:
    case MI_SET_TIME: return 2;
    case MI_SET_DATE: return 3;
    case MI_SYNC:
    case MI_TEST: return 0; // actions, not values
    default: return 1;
  }
}

void fieldRange(uint8_t item, uint8_t field, int &lo, int &hi) {
  lo = 0;
  switch (item) {
    case MI_ALARM_TIME:
    case MI_SET_TIME: hi = field == 0 ? 23 : 59; break;
    case MI_SET_DATE:
      if (field == 0) { lo = 1; hi = daysInMonth(editVal[2], editVal[1]); }
      else if (field == 1) { lo = 1; hi = 12; }
      else { lo = 2024; hi = 2099; }
      break;
    case MI_REPEAT: hi = REPEAT_COUNT - 1; break;
    case MI_SNOOZE: lo = 1; hi = 30; break;
    case MI_CODE: hi = MAX_CODE_LEN; break;
    default: hi = 1; break; // on/off style items
  }
}

// The current values of a menu item, in editVal order.
void itemValues(uint8_t item, int *v) {
  struct tm t;
  localNowOrBuildDate(t);
  switch (item) {
    case MI_ALARM_TIME: v[0] = settings.alarmHour; v[1] = settings.alarmMinute; break;
    case MI_ALARM_ON: v[0] = settings.alarmOn; break;
    case MI_REPEAT: v[0] = settings.repeat; break;
    case MI_SNOOZE: v[0] = settings.snoozeMin; break;
    case MI_CODE: v[0] = settings.codeLen; break;
    case MI_FORMAT: v[0] = settings.use24h; break;
    case MI_SET_TIME: v[0] = t.tm_hour; v[1] = t.tm_min; break;
    case MI_SET_DATE: v[0] = t.tm_mday; v[1] = t.tm_mon + 1; v[2] = t.tm_year + 1900; break;
  }
}

void commitEdit() {
  int *v = editVal;
  switch (menuItem) {
    case MI_ALARM_TIME:
      settings.alarmHour = v[0];
      settings.alarmMinute = v[1];
      settings.alarmOn = true; // setting an alarm time means you want it
      lastAlarmMinute = -1;
      break;
    case MI_ALARM_ON: settings.alarmOn = v[0]; break;
    case MI_REPEAT: settings.repeat = v[0]; break;
    case MI_SNOOZE: settings.snoozeMin = v[0]; break;
    case MI_CODE: settings.codeLen = v[0]; break;
    case MI_FORMAT: settings.use24h = v[0]; break;
    case MI_SET_TIME: {
      struct tm t;
      localNowOrBuildDate(t);
      setClock(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, v[0], v[1], 0);
      return;
    }
    case MI_SET_DATE: {
      struct tm t;
      localNowOrBuildDate(t);
      setClock(v[2], v[1], min(v[0], daysInMonth(v[2], v[1])), t.tm_hour, t.tm_min, t.tm_sec);
      return;
    }
  }
  saveSettings();
}

void menuAction() {
  if (menuItem == MI_SYNC) {
    if (wifiConfigured()) {
      startWiFi();
      toast("Syncing...");
    } else {
      toast("No WiFi in secrets.h", 2500);
    }
  } else if (menuItem == MI_TEST) {
    startRinging();
  }
}

// ===========================================================================
// Keys
// ===========================================================================

Key keys[6];

#define DEBOUNCE_MS 25
#define REPEAT_DELAY_MS 450
#define REPEAT_RATE_MS 90

void onKey(uint8_t k, bool repeat);

void setupKeys() {
  for (uint8_t i = 0; i < 6; i++) {
    pinMode(KEY_PINS[i], INPUT_PULLUP);
    keys[i].raw = keys[i].down = digitalRead(KEY_PINS[i]) == LOW; // ignore keys held at boot
  }
}

void pollKeys() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < 6; i++) {
    Key &k = keys[i];
    bool raw = digitalRead(KEY_PINS[i]) == LOW;
    if (raw != k.raw) {
      k.raw = raw;
      k.changedAt = now;
    }
    if (raw != k.down && now - k.changedAt >= DEBOUNCE_MS) {
      k.down = raw;
      if (raw) {
        k.repeatAt = now + REPEAT_DELAY_MS;
        onKey(i, false);
      }
    } else if (k.down && (i == KEY_UP || i == KEY_DOWN) && (int32_t)(now - k.repeatAt) >= 0) {
      k.repeatAt = now + REPEAT_RATE_MS;
      onKey(i, true);
    }
  }
}

void onClockKey(uint8_t k) {
  if (k == KEY_MENU || k == KEY_OK) {
    screen = SCR_MENU;
    menuItem = 0;
    editing = false;
  } else if (k == KEY_ALARM) {
    if (snoozeUntil) {
      snoozeUntil = 0;
      toast("Snooze cancelled");
    } else {
      settings.alarmOn = !settings.alarmOn;
      saveSettings();
      if (settings.alarmOn) {
        char buf[24], h[3];
        formatHour(h, settings.alarmHour);
        snprintf(buf, sizeof(buf), "ALARM ON %s:%02d", h, settings.alarmMinute);
        toast(buf);
      } else {
        toast("ALARM OFF");
      }
    }
  }
}

void onMenuKey(uint8_t k) {
  if (!editing) {
    if (k == KEY_UP) menuItem = (menuItem + MI_COUNT - 1) % MI_COUNT;
    else if (k == KEY_DOWN) menuItem = (menuItem + 1) % MI_COUNT;
    else if (k == KEY_MENU || k == KEY_BACK) screen = SCR_CLOCK;
    else if (k == KEY_OK) {
      if (fieldCount(menuItem) == 0) {
        menuAction();
      } else {
        itemValues(menuItem, editVal);
        editField = 0;
        editing = true;
      }
    }
    return;
  }

  int lo, hi;
  fieldRange(menuItem, editField, lo, hi);
  int &v = editVal[editField];
  if (k == KEY_UP) v = v >= hi ? lo : v + 1;
  else if (k == KEY_DOWN) v = v <= lo ? hi : v - 1;
  else if (k == KEY_MENU) editing = false; // cancel
  else if (k == KEY_BACK) {
    if (editField > 0) editField--;
    else editing = false;
  } else if (k == KEY_OK) {
    if (++editField >= fieldCount(menuItem)) {
      commitEdit();
      editing = false;
      toast("Saved", 1000);
    }
  }
  // Keep the day valid after changing the month or year
  if (menuItem == MI_SET_DATE) {
    editVal[0] = min(editVal[0], daysInMonth(editVal[2], editVal[1]));
  }
}

void onDismissKey(uint8_t k) {
  if (k + 1 == code[codePos]) {
    if (++codePos >= settings.codeLen) dismiss();
  } else {
    wrongFlashUntil = millis() + 400;
    newCode();
  }
}

void onKey(uint8_t k, bool repeat) {
  lastKeyAt = millis();
  redraw = true;
  if (repeat && screen != SCR_MENU) return;

  switch (screen) {
    case SCR_CLOCK:
      if (KEY_CLICK) PLAY(CLICK_PATTERN, false);
      onClockKey(k);
      break;
    case SCR_MENU:
      if (KEY_CLICK) PLAY(CLICK_PATTERN, false);
      onMenuKey(k);
      break;
    case SCR_RINGING:
      if (k == KEY_OK) snooze();
      else if (settings.codeLen == 0) dismiss();
      else {
        newCode();
        screen = SCR_DISMISS;
      }
      break;
    case SCR_DISMISS:
      onDismissKey(k);
      break;
  }
}

// ===========================================================================
// Drawing
// ===========================================================================

// --- 7-segment digits ------------------------------------------------------
// Segment bits: 0=top 1=top-right 2=bottom-right 3=bottom 4=bottom-left 5=top-left 6=middle
const uint8_t SEGMENTS[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
#define SEG_GAP 1

void segH(int x0, int x1, int cy, int h, uint16_t c) {
  x0 += SEG_GAP;
  x1 -= SEG_GAP;
  canvas.fillTriangle(x0, cy, x0 + h, cy - h, x0 + h, cy + h, c);
  canvas.fillRect(x0 + h, cy - h, x1 - x0 - 2 * h + 1, 2 * h + 1, c);
  canvas.fillTriangle(x1, cy, x1 - h, cy - h, x1 - h, cy + h, c);
}

void segV(int cx, int y0, int y1, int h, uint16_t c) {
  y0 += SEG_GAP;
  y1 -= SEG_GAP;
  canvas.fillTriangle(cx, y0, cx - h, y0 + h, cx + h, y0 + h, c);
  canvas.fillRect(cx - h, y0 + h, 2 * h + 1, y1 - y0 - 2 * h + 1, c);
  canvas.fillTriangle(cx, y1, cx - h, y1 - h, cx + h, y1 - h, c);
}

// digit: 0-9, or -1 for blank (only the ghost segments are drawn)
void drawDigit(int x, int y, int w, int hgt, int thick, int digit, uint16_t on, uint16_t ghost) {
  int h = thick / 2;
  int left = x + h, right = x + w - 1 - h;
  int top = y + h, mid = y + hgt / 2, bottom = y + hgt - 1 - h;
  uint8_t bits = digit >= 0 ? SEGMENTS[digit] : 0;
  for (uint8_t s = 0; s < 7; s++) {
    bool lit = bits & (1 << s);
    if (!lit && ghost == COLOR_BG) continue;
    uint16_t c = lit ? on : ghost;
    switch (s) {
      case 0: segH(left, right, top, h, c); break;
      case 1: segV(right, top, mid, h, c); break;
      case 2: segV(right, mid, bottom, h, c); break;
      case 3: segH(left, right, bottom, h, c); break;
      case 4: segV(left, mid, bottom, h, c); break;
      case 5: segV(left, top, mid, h, c); break;
      case 6: segH(left, right, mid, h, c); break;
    }
  }
}

// --- text helpers ----------------------------------------------------------

// Built-in font: each character is 6*size pixels wide, 8*size tall.
void textAt(int x, int y, const char *s, uint8_t size, uint16_t color) {
  canvas.setFont(nullptr);
  canvas.setTextSize(size);
  canvas.setTextColor(color);
  canvas.setCursor(x, y);
  canvas.print(s);
}

int textWidth(const char *s, uint8_t size) {
  return strlen(s) * 6 * size - size; // no trailing gap after the last char
}

void textCentered(int cx, int y, const char *s, uint8_t size, uint16_t color) {
  textAt(cx - textWidth(s, size) / 2, y, s, size, color);
}

void textRight(int right, int y, const char *s, uint8_t size, uint16_t color) {
  textAt(right - textWidth(s, size), y, s, size, color);
}

void drawBell(int x, int y, uint16_t c) { // about 13x15 pixels
  canvas.fillCircle(x + 6, y + 5, 5, c);
  canvas.fillRect(x + 1, y + 5, 11, 5, c);
  canvas.fillRect(x, y + 10, 13, 2, c);
  canvas.fillCircle(x + 6, y + 13, 1, c);
}

// --- clock face ------------------------------------------------------------

#define DIG_W 38
#define DIG_H 52
#define DIG_T 8
#define DIG_GAP 6
#define COLON_W 18
#define SEC_W 14
#define SEC_H 24
#define SEC_T 4
#define SEC_GAP 4
#define CLOCK_W (4 * DIG_W + 2 * DIG_GAP + COLON_W + 10 + 2 * SEC_W + SEC_GAP)
#define CLOCK_X ((SCREEN_W - CLOCK_W) / 2)
#define CLOCK_Y 3
#define BOTTOM_Y 60

bool isNight(int hour) {
  if (NIGHT_START_HOUR == NIGHT_END_HOUR) return false;
  if (NIGHT_START_HOUR < NIGHT_END_HOUR) return hour >= NIGHT_START_HOUR && hour < NIGHT_END_HOUR;
  return hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR;
}

void drawTime(const struct tm &t, bool valid, uint16_t on, uint16_t ghost) {
  int d[4] = { -1, -1, -1, -1 }, s[2] = { -1, -1 };
  if (valid) {
    int h = t.tm_hour;
    if (!settings.use24h) h = h % 12 == 0 ? 12 : h % 12;
    d[0] = h / 10;
    d[1] = h % 10;
    d[2] = t.tm_min / 10;
    d[3] = t.tm_min % 10;
    s[0] = t.tm_sec / 10;
    s[1] = t.tm_sec % 10;
    if (!settings.use24h && d[0] == 0) d[0] = -1; // " 7:30", not "07:30"
  }

  int x = CLOCK_X;
  drawDigit(x, CLOCK_Y, DIG_W, DIG_H, DIG_T, d[0], on, ghost);
  x += DIG_W + DIG_GAP;
  drawDigit(x, CLOCK_Y, DIG_W, DIG_H, DIG_T, d[1], on, ghost);
  x += DIG_W;

  // Colon blinks once a second
  uint16_t colon = (!valid || t.tm_sec % 2 == 0) ? on : ghost;
  int cx = x + COLON_W / 2 - DIG_T / 2;
  canvas.fillRect(cx, CLOCK_Y + DIG_H / 3 - DIG_T / 2, DIG_T, DIG_T, colon);
  canvas.fillRect(cx, CLOCK_Y + 2 * DIG_H / 3 - DIG_T / 2, DIG_T, DIG_T, colon);
  x += COLON_W;

  drawDigit(x, CLOCK_Y, DIG_W, DIG_H, DIG_T, d[2], on, ghost);
  x += DIG_W + DIG_GAP;
  drawDigit(x, CLOCK_Y, DIG_W, DIG_H, DIG_T, d[3], on, ghost);
  x += DIG_W + 10;

  // Small seconds, bottom aligned with the big digits
  int sy = CLOCK_Y + DIG_H - SEC_H;
  drawDigit(x, sy, SEC_W, SEC_H, SEC_T, s[0], on, ghost);
  drawDigit(x + SEC_W + SEC_GAP, sy, SEC_W, SEC_H, SEC_T, s[1], on, ghost);

  if (valid && !settings.use24h) {
    textAt(x + 2, CLOCK_Y + 4, t.tm_hour < 12 ? "AM" : "PM", 2, on);
  }
}

void drawClock() {
  struct tm t;
  bool valid = localNow(t);
  bool night = valid && isNight(t.tm_hour);
  drawTime(t, valid, night ? COLOR_NIGHT_DIGIT : COLOR_DIGIT, night ? COLOR_NIGHT_GHOST : COLOR_DIGIT_GHOST);

  uint16_t text = night ? COLOR_NIGHT_DIGIT : COLOR_TEXT;
  uint16_t dim = night ? COLOR_NIGHT_GHOST : COLOR_DIM;
  char buf[32];

  if (millis() < toastUntil) {
    textCentered(SCREEN_W / 2, BOTTOM_Y, toastText, 2, COLOR_ACCENT);
    return;
  }

  // Left: date, or a hint while the time isn't known yet
  if (valid) {
    snprintf(buf, sizeof(buf), "%s %d %s", DAY_NAMES[t.tm_wday], t.tm_mday, MONTH_NAMES[t.tm_mon]);
    textAt(4, BOTTOM_Y, buf, 2, text);
  } else {
    textAt(4, BOTTOM_Y, wifiConfigured() ? "GETTING TIME..." : "SET TIME: MENU", 2, COLOR_ACCENT);
    return;
  }

  // Right: alarm / snooze status
  if (snoozeUntil) {
    long left = max<long>(0, snoozeUntil - time(nullptr));
    snprintf(buf, sizeof(buf), "zZ %ld:%02ld", left / 60, left % 60);
    textRight(SCREEN_W - 3, BOTTOM_Y, buf, 2, COLOR_ACCENT);
  } else {
    char h[3];
    formatHour(h, settings.alarmHour);
    if (settings.alarmOn) snprintf(buf, sizeof(buf), "%s:%02d", h, settings.alarmMinute);
    else strcpy(buf, "OFF");
    int w = textWidth(buf, 2);
    textRight(SCREEN_W - 3, BOTTOM_Y, buf, 2, settings.alarmOn ? text : dim);
    drawBell(SCREEN_W - 3 - w - 18, BOTTOM_Y, settings.alarmOn ? text : dim);
  }
}

// --- ringing / stop code ---------------------------------------------------

void drawRinging() {
  struct tm t;
  bool valid = localNow(t);
  bool flash = (millis() / 500) % 2;
  uint16_t bg = flash ? COLOR_BAD : COLOR_BG;
  canvas.fillScreen(bg);
  drawTime(t, valid, COLOR_TEXT, bg);
  char hint[24];
  snprintf(hint, sizeof(hint), "%d:SNOOZE  ANY:STOP", KEY_OK + 1);
  textCentered(SCREEN_W / 2, BOTTOM_Y, hint, 2, COLOR_TEXT);
}

void drawDismiss() {
  bool wrong = millis() < wrongFlashUntil;
  textAt(4, 3, "TYPE THE CODE TO STOP THE ALARM", 1, wrong ? COLOR_BAD : COLOR_DIM);

  // Code boxes
  const int boxW = 28, boxH = 38, gap = 6, y = 16;
  int total = settings.codeLen * boxW + (settings.codeLen - 1) * gap;
  int x = (210 - total) / 2;
  canvas.setFont(&FreeSansBold18pt7b);
  canvas.setTextSize(1);
  for (uint8_t i = 0; i < settings.codeLen; i++, x += boxW + gap) {
    uint16_t frame = wrong ? COLOR_BAD : i < codePos ? COLOR_GOOD : i == codePos ? COLOR_TEXT : COLOR_DIM;
    if (i < codePos) canvas.fillRoundRect(x, y, boxW, boxH, 4, COLOR_GOOD);
    else canvas.drawRoundRect(x, y, boxW, boxH, 4, frame);
    if (i == codePos) canvas.drawRoundRect(x + 1, y + 1, boxW - 2, boxH - 2, 3, frame);

    char d[2] = { char('0' + code[i]), 0 };
    int16_t bx, by;
    uint16_t bw, bh;
    canvas.getTextBounds(d, 0, 0, &bx, &by, &bw, &bh);
    canvas.setTextColor(i < codePos ? COLOR_BG : COLOR_TEXT);
    canvas.setCursor(x + (boxW - bw) / 2 - bx, y + (boxH + bh) / 2);
    canvas.print(d);
  }
  canvas.setFont(nullptr);

  // Key map, so you know which key is which number
  textAt(224, 3, "KEYS", 1, COLOR_DIM);
  for (uint8_t i = 0; i < 6; i++) {
    int kx = 220 + (i % 3) * 20, ky = 16 + (i / 3) * 20;
    canvas.drawRoundRect(kx, ky, 18, 18, 3, COLOR_DIM);
    char d[2] = { char('1' + i), 0 };
    textAt(kx + 6, ky + 5, d, 1, COLOR_TEXT);
  }

  struct tm t;
  if (localNow(t)) {
    char buf[8], h[3];
    formatHour(h, t.tm_hour);
    snprintf(buf, sizeof(buf), "%s:%02d", h, t.tm_min);
    textAt(4, BOTTOM_Y, buf, 2, COLOR_DIM);
  }
}

// --- menu ------------------------------------------------------------------

uint8_t valueSegments(uint8_t item, const int *v, Segment *seg) {
  uint8_t n = 0;
  auto add = [&](int8_t field, const char *fmt, auto... args) {
    snprintf(seg[n].text, sizeof(seg[n].text), fmt, args...);
    seg[n++].field = field;
  };
  switch (item) {
    case MI_ALARM_TIME:
    case MI_SET_TIME: {
      char h[3];
      formatHour(h, v[0]);
      add(0, "%s", h);
      add(-1, "%s", ":");
      add(1, "%02d", v[1]);
      if (!settings.use24h) add(-1, "%s", v[0] < 12 ? " AM" : " PM");
      break;
    }
    case MI_SET_DATE:
      add(0, "%d", v[0]);
      add(-1, "%s", " ");
      add(1, "%s", MONTH_NAMES[v[1] - 1]);
      add(-1, "%s", " ");
      add(2, "%d", v[2]);
      break;
    case MI_ALARM_ON: add(0, "%s", v[0] ? "On" : "Off"); break;
    case MI_REPEAT: add(0, "%s", REPEAT_NAMES[v[0]]); break;
    case MI_SNOOZE: add(0, "%d min", v[0]); break;
    case MI_CODE:
      if (v[0]) add(0, "%d keys", v[0]);
      else add(0, "Off");
      break;
    case MI_FORMAT: add(0, "%s", v[0] ? "24 hour" : "12 hour"); break;
    case MI_SYNC:
      if (!wifiConfigured()) add(-1, "No WiFi");
      else if (WiFi.status() != WL_CONNECTED) add(-1, "Offline");
      else if (lastSync) {
        struct tm t;
        time_t ls = lastSync;
        localtime_r(&ls, &t);
        char h[3];
        formatHour(h, t.tm_hour);
        add(-1, "OK %s:%02d", h, t.tm_min);
      } else add(-1, "Online");
      break;
    case MI_TEST: add(-1, "Press %d", KEY_OK + 1); break;
  }
  return n;
}

void drawMenu() {
  char buf[16];
  textAt(4, 3, editing ? "EDIT" : "SETTINGS", 1, COLOR_DIM);
  snprintf(buf, sizeof(buf), "%d/%d", menuItem + 1, MI_COUNT);
  textRight(SCREEN_W - 4, 3, buf, 1, COLOR_DIM);

  canvas.setTextSize(1);
  canvas.setFont(&FreeSansBold12pt7b);
  canvas.setTextColor(COLOR_TEXT);
  canvas.setCursor(4, 31);
  canvas.print(MENU_NAMES[menuItem]);

  int values[3];
  const int *v = editVal;
  if (!editing) {
    itemValues(menuItem, values);
    v = values;
  }
  Segment seg[6];
  uint8_t n = valueSegments(menuItem, v, seg);

  const int baseline = 61;
  canvas.setFont(&FreeSansBold18pt7b);
  canvas.setCursor(6, baseline);
  for (uint8_t i = 0; i < n; i++) {
    bool active = editing && seg[i].field == editField;
    if (active) {
      int16_t bx, by;
      uint16_t bw, bh;
      canvas.getTextBounds(seg[i].text, canvas.getCursorX(), baseline, &bx, &by, &bw, &bh);
      canvas.fillRoundRect(bx - 4, baseline - 28, bw + 8, 34, 4, COLOR_ACCENT);
    }
    canvas.setTextColor(active ? COLOR_BG : editing ? COLOR_DIM : COLOR_ACCENT);
    canvas.print(seg[i].text);
  }
  canvas.setFont(nullptr);

  char hint[40];
  if (editing) {
    snprintf(hint, sizeof(hint), "%d/%d:CHANGE  %d:NEXT  %d:BACK",
             KEY_UP + 1, KEY_DOWN + 1, KEY_OK + 1, KEY_BACK + 1);
  } else {
    snprintf(hint, sizeof(hint), "%d/%d:MOVE  %d:%s  %d:EXIT", KEY_UP + 1, KEY_DOWN + 1,
             KEY_OK + 1, fieldCount(menuItem) ? "EDIT" : "GO", KEY_MENU + 1);
  }
  textRight(SCREEN_W - 4, 68, hint, 1, COLOR_DIM);

  if (millis() < toastUntil) {
    textRight(SCREEN_W - 4, 18, toastText, 1, COLOR_GOOD);
  }
}

void render() {
  canvas.fillScreen(COLOR_BG);
  switch (screen) {
    case SCR_CLOCK: drawClock(); break;
    case SCR_MENU: drawMenu(); break;
    case SCR_RINGING: drawRinging(); break;
    case SCR_DISMISS: drawDismiss(); break;
  }
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
}

void drawSplash() {
  canvas.fillScreen(COLOR_BG);
  canvas.setFont(&FreeSansBold18pt7b);
  canvas.setTextColor(COLOR_DIGIT);
  canvas.setCursor(80, 46);
  canvas.print("BLARE");
  canvas.setFont(nullptr);
  textCentered(SCREEN_W / 2, 62, wifiConfigured() ? "connecting to wifi..." : "no wifi - set the time in the menu", 1, COLOR_DIM);
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
}

// ===========================================================================
// setup() runs once, loop() runs forever
// ===========================================================================

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.begin(115200); // lets the board talk to your computer
  Serial.println("\nBLARE starting");

  // Hardware SPI on our pins. The library's own SPI.begin() then does nothing.
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);
  tft.init(76, 284);                            // panel size (portrait)
  tft.setOffsets(TFT_COL_OFFSET, TFT_ROW_OFFSET); // offsets for the weird resolution
  tft.invertDisplay(TFT_INVERT);
  tft.setRotation(TFT_ROTATION);
  tft.setSPISpeed(TFT_SPI_HZ);
  if (!canvas.getBuffer()) Serial.println("Not enough RAM for the screen buffer!");
  Serial.println("TFT Initialized!");
  drawSplash();

  setupKeys();
  loadSettings();
  randomSeed(esp_random());

  setenv("TZ", TIMEZONE, 1); // so a hand-set clock uses the right time zone too
  tzset();
  sntp_set_time_sync_notification_cb(onTimeSync);
  startWiFi();
  delay(1200); // let the splash be seen
}

void loop() {
  static int lastSecond = -1;
  static uint32_t lastHalfSecond = 0;
  static time_t seenSync = 0;

  pollKeys();
  updateSound();
  checkAlarm();

  if (seenSync != lastSync) {
    seenSync = lastSync;
    Serial.println("Time synced from the internet");
    redraw = true;
  }

  // Leave menus and the stop-code screen alone for a while and they close
  if (screen == SCR_MENU && millis() - lastKeyAt > 30000) {
    screen = SCR_CLOCK;
    editing = false;
    redraw = true;
  }
  if (screen == SCR_DISMISS && millis() - lastKeyAt > DISMISS_IDLE_MS) {
    screen = SCR_RINGING;
    redraw = true;
  }

  // Redraw when the second ticks, twice a second for blinking, or on input
  time_t now = time(nullptr);
  uint32_t half = millis() / 500;
  if (now % 60 != lastSecond || half != lastHalfSecond) {
    lastSecond = now % 60;
    lastHalfSecond = half;
    redraw = true;
  }
  if (redraw) {
    redraw = false;
    render();
  }

  delay(5);
}
