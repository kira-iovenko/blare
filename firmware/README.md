# BLARE firmware

Alarm clock firmware for the BLARE PCB: a Seeed XIAO ESP32C3, a 2.25" ST7789 screen (76x284), six keyswitches and a piezo buzzer.

## Features

- Big 7-segment style clock with seconds, the date, and the alarm status
- Gets the time from the internet over WiFi (NTP), or you set it by hand from the menu
- The alarm can repeat every day, on weekdays, on weekends, or ring once, and it has a snooze
- **Stop code:** to turn the alarm off you have to type a random sequence of keys shown on the screen, so you can't switch it off half asleep
- Night mode: the digits turn dim red between 22:00 and 07:00
- Settings are saved to flash and survive a power cut

## Setup

1. Install the [Arduino IDE](https://www.arduino.cc/en/software/) and add the ESP32 boards. Follow the *Software Preparation* steps in the [Seeed guide](https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/).
2. In the Library Manager, install **Adafruit ST7735 and ST7789 Library** and **Adafruit GFX Library**, along with the extra libraries it offers to install.
3. Copy `secrets.example.h` to `secrets.h` and fill in your WiFi name and password. `secrets.h` is git-ignored. To run without WiFi, leave the name empty.
4. Set your `TIMEZONE` in `config.h`.
5. Open `blare.ino`, select the board **XIAO_ESP32C3** and its port, then click Upload.

Arduino CLI:

```sh
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3 firmware
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32C3 -p <port> firmware
```

## Pins

These come from `pcb/blare.kicad_pcb`.

| Function       | XIAO pin | GPIO | Notes                             |
|----------------|----------|------|-----------------------------------|
| Key 1 (SW1)    | D0       | 2    | Strapping pin, don't hold at boot |
| Key 4 (SW2)    | D1       | 3    |                                   |
| Key 2 (SW3)    | D2       | 4    |                                   |
| Key 5 (SW4)    | D3       | 5    |                                   |
| Key 3 (SW5)    | D4       | 6    |                                   |
| Key 6 (SW6)    | D5       | 7    |                                   |
| Buzzer         | D6       | 21   |                                   |
| Screen CS      | D7       | 20   |                                   |
| Screen DC      | D8       | 8    |                                   |
| Screen SDA     | D9       | 9    | Hardware SPI MOSI                 |
| Screen SCL     | D10      | 10   | Hardware SPI clock                |
| Screen RST, BL | 3.3V     | -    | Not controlled by the firmware    |

## Keys

The keys are numbered as you see them from the front:

```
 [1] MENU    [2] UP      [3] ALARM
 [4] BACK    [5] DOWN    [6] OK / SNOOZE
```

| Where         | Key         | What it does                                       |
|---------------|-------------|----------------------------------------------------|
| Clock         | 1 or 6      | Open the settings                                  |
| Clock         | 3           | Turn the alarm on or off, or cancel a snooze       |
| Settings      | 2 / 5       | Previous / next setting                            |
| Settings      | 6           | Edit the setting, or run it (Sync time, Test alarm)|
| Settings      | 1 or 4      | Exit                                               |
| Editing       | 2 / 5       | Change the value (hold to go faster)               |
| Editing       | 6           | Next field, then save                              |
| Editing       | 4           | Previous field                                     |
| Editing       | 1           | Cancel                                             |
| Ringing       | 6           | Snooze                                             |
| Ringing       | any other   | Show the stop code                                 |
| Stop code     | 1-6         | Type the code. A wrong key gives you a new code    |

Settings: alarm time, alarm on/off, repeat, snooze length, stop code length (0 means any key stops the alarm), 12/24 hour, set time, set date, sync time, test alarm.

To move a function to a different key, change the `KEY_*` lines in `config.h`.

## Troubleshooting

- **Blank screen but the board runs** (you can see messages in the Serial Monitor at 115200): per the BLARE docs, the backlight pin is active-low, and on this PCB BL is tied to 3.3V. Connect the screen's BL wire to GND instead of to header pin 8.
- **Picture is upside down:** set `TFT_ROTATION` to `3`.
- **Colours are inverted:** flip `TFT_INVERT`.
- **Picture is shifted or garbled:** adjust `TFT_COL_OFFSET` / `TFT_ROW_OFFSET`, or lower `TFT_SPI_HZ`.
- **Buzzer is silent, or only clicks:** if your buzzer is an *active* type, set `BUZZER_IS_ACTIVE` to `true`.
- **Short chirp when powering on:** GPIO21 is also the chip's boot-log TX pin. This is harmless.
