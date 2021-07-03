#include <tinyNeoPixel_Static.h>
#include <TimerOne.h>
#ifdef __AVR__
  #include <avr/power.h>
#endif
#include <math.h>

#define BUTTON_PIN PIN_PB2
#define LEDS_PIN PIN_PB4

#define NUMPIXELS 16
#define DELAYVAL 500
#define LONG_PRESS_DELAY 1500
#define SEQUENCE_DELAY 500
#define DEBOUNCE_DELAY 50
#define MAX_PATTERNS 10
#define FADE_DELAY 1000
#define MOMENTARY_DELAY 3000

// 2.8 gamma correction
const uint8_t PROGMEM gamma8[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2,
    2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5,
    5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10,
    10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
    25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
    37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
    51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
    69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
    90, 92, 93, 95, 96, 98, 99, 101, 102, 104, 105, 107, 109, 110, 112, 114,
    115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142,
    144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 167, 169, 171, 173, 175,
    177, 180, 182, 184, 186, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213,
    215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247, 249, 252, 255
};

#define gamma(x) (pgm_read_byte(&gamma8[(int)(x)]))

uint8_t hueToColorComponent(double hue) {
  hue = fmod(hue, 360.0);
  if (hue < 0) { hue += 360.0; }
  if (hue > 240.0) { return 0; }
  if (hue > 180.0) { return (255.0 / 60.0) * (240.0 - hue); }
  if (hue < 60.0) { return (255.0 / 60.0) * hue; }
  return 255;
}

void hueToColor(double hue, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = hueToColorComponent(hue);
    g = hueToColorComponent(hue + 120.0);
    b = hueToColorComponent(hue + 240.0);
}

byte pixel_buffer[NUMPIXELS*3];
tinyNeoPixel pixels(NUMPIXELS, LEDS_PIN, NEO_GRB + NEO_KHZ800, pixel_buffer);

struct Pattern {
  byte r = 255, g = 255, b = 255;
  float brightness = 0.5f, jitter = 1.0f;
} patterns[MAX_PATTERNS];

enum SettingsStage {
  idle,
  settingHue,
  settingBrightness,
  settingJitter,
  numStages,
};

struct MenuState {
  int currentPattern = 1;
  bool powerOn = false;
  SettingsStage settingsStage = idle;
} menu;

struct InputState {
  unsigned long lastPressTime;
  unsigned long lastReleaseTime;
  int clicks;
  bool longPress;
  bool enterSettings;
  int quickAccessReturn;
} input;

void resetInputState() {
  input.clicks = 0;
  input.lastPressTime = millis();
  input.lastReleaseTime = millis();
  input.longPress = false;
  input.enterSettings = false;
  input.quickAccessReturn = -1;
}

typedef void (*TimerCallback)();
TimerCallback timerCallback = NULL;
unsigned long timerTarget = 0;
void setTimer(TimerCallback callback, unsigned long timeout);
void setTimer(TimerCallback callback, unsigned long timeout) {
  timerCallback = callback;
  timerTarget = millis() + timeout;
}
void clearTimer() {
  timerCallback = NULL;
}
void updateTimer() {
  if (timerCallback && millis() >= timerTarget) {
    timerCallback();
    timerCallback = NULL;
  }
}

void setPower(bool enabled) {
  menu.powerOn = enabled;
}

void setCurrentPattern(int patternIndex) {
  setPower(true);
  if (menu.currentPattern == patternIndex) return;
  menu.currentPattern = patternIndex;
}

void cycleSettingStages() {
  switch (menu.settingsStage) {
    case idle:
      menu.settingsStage = settingHue;
      break;
    case settingHue:
      menu.settingsStage = settingBrightness;
      break;
    case settingBrightness:
      menu.settingsStage = settingJitter;
      break;
    case settingJitter:
      menu.settingsStage = idle;
      break;
  }
}

void onButtonPress() {
}

void cancelInput() {
  clearTimer();
  resetInputState();
}

void onButtonRelease() {
  if (input.lastReleaseTime - input.lastPressTime < DEBOUNCE_DELAY) return;
  if (input.enterSettings) return;

  if (menu.settingsStage != idle) {
    cycleSettingStages();
    cancelInput();
    return;
  }

  if (!input.longPress) {
    input.clicks += 1;
  }

  if (input.quickAccessReturn != -1 && input.lastReleaseTime - input.lastPressTime >= MOMENTARY_DELAY) {
    if (input.quickAccessReturn == -2) {
      setPower(false);
    } else {
      setCurrentPattern(input.quickAccessReturn);
    }
    return;
  }
}

void onButtonLongPress() {
  if (menu.settingsStage != idle) return;
  if (input.clicks == 0) {
    input.quickAccessReturn = menu.powerOn ? menu.currentPattern : -2;
    setCurrentPattern(0);
  } else if (input.clicks == 1) {
    input.enterSettings = true;
    cycleSettingStages();
  }
}

void onInputStateCommit() {
  if (menu.settingsStage != idle) return;
  if (input.quickAccessReturn != -1) return;
  if (input.enterSettings) return;

  if (input.clicks == 1) {
    setPower(!menu.powerOn);
    return;
  }

  if (input.clicks - 1 < MAX_PATTERNS) {
    setPower(true);
    setCurrentPattern(input.clicks - 1);
  }
}

void commitInputState() {
  clearTimer();
  onInputStateCommit();
  resetInputState();
}

void longPressButton() {
  clearTimer();
  input.longPress = true;
  onButtonLongPress();
}

bool previousButtonState = false;
void checkButtonState() {
  bool buttonState = digitalRead(BUTTON_PIN) == LOW;
  if (buttonState == previousButtonState) return;
  previousButtonState = buttonState;
  if (buttonState) {
    input.lastPressTime = millis();
    input.longPress = false;
    setTimer(longPressButton, LONG_PRESS_DELAY);
    onButtonPress();
  } else {
    input.lastReleaseTime = millis();
    setTimer(commitInputState, SEQUENCE_DELAY);
    onButtonRelease();
  }
}

void buttonInterrupt() {
  checkButtonState();
}

void setup() {
#if defined(__AVR_ATtiny85__) && (F_CPU == 16000000)
  clock_prescale_set(clock_div_1);
#endif
  
  pinMode(LEDS_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(PIN_PB3, OUTPUT);

  Timer1.initialize();

  resetInputState();
  checkButtonState();
//  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonInterrupt, CHANGE);
}

void renderPattern(const struct Pattern& p); // Compiler bug requires me to have this?

void renderPattern(const struct Pattern& p) {
  pixels.clear();
  for (int i=0; i<NUMPIXELS; i++) {
    float value = p.brightness;
    pixels.setPixelColor(i, pixels.Color(
      gamma(p.r * value),
      gamma(p.g * value),
      gamma(p.b * value)
    ));
  }
  pixels.show();
}

void renderSetHue() {
  Pattern &p = patterns[menu.currentPattern];
  double hue = modf((double)millis() / 5000.0, NULL) * 360.0;
  hueToColor(hue, p.r, p.g, p.b);
  renderPattern(p);
}

void renderSetBrightness() {
  Pattern &p = patterns[menu.currentPattern];
  p.brightness = modf((double)millis() / 5000.0, NULL);
  renderPattern(p);
}

void renderSetJitter() {
  Pattern &p = patterns[menu.currentPattern];
  p.jitter = modf((double)millis() / 5000.0, NULL);
  renderPattern(p);
}

void loop() {
  checkButtonState();
  updateTimer();
  
  digitalWrite(PIN_PB3, digitalRead(BUTTON_PIN));
  
  switch (menu.settingsStage) {
    case idle: {
      Pattern p = patterns[menu.currentPattern];
      if (!menu.powerOn) {
        p.brightness = 0.0f;
      }
      renderPattern(p);
      break;
    }
    case settingHue: {
      renderSetHue();
      break;
    }
    case settingBrightness: {
      renderSetBrightness();
      break;
    }
    case settingJitter: {
      renderSetJitter();
      break;
    }
  }
  delay(30);
}
