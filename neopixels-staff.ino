#include <tinyNeoPixel_Static.h>
#ifdef __AVR__
  #include <avr/power.h>
  #include <avr/sleep.h>
#endif
#include <math.h>
#include <EEPROM.h>

#define BUTTON_PIN PIN_PB2
#define LEDS_PIN PIN_PB4
#define STATUS_PIN PIN_PB0 // Optional LED. Indicates wether the CPU is in sleep mode or not
#define NUMPIXELS 16

#define DELAYVAL 500
#define LONG_PRESS_DELAY 1500
#define SEQUENCE_DELAY 500
#define DEBOUNCE_DELAY 30
#define MAX_PATTERNS 10
#define MOMENTARY_DELAY 3000
#define JITTER_SPEED 0.0005
#define FADE_SPEED 0.001
#define POWEROFF_SPEED 0.001
#define EEPROM_VERSION 0x42

#define DEFAULT_BRIGHTNESS 0.3f
#define DEFAULT_JITTER 0.4f

// 2.6 gamma correction
const uint8_t PROGMEM gamma8[] = {
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,
  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,  3,
  3,  3,  4,  4,  4,  4,  5,  5,  5,  5,  5,  6,  6,  6,  6,  7,
  7,  7,  8,  8,  8,  9,  9,  9, 10, 10, 10, 11, 11, 11, 12, 12,
  13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20,
  20, 21, 21, 22, 22, 23, 24, 24, 25, 25, 26, 27, 27, 28, 29, 29,
  30, 31, 31, 32, 33, 34, 34, 35, 36, 37, 38, 38, 39, 40, 41, 42,
  42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
  58, 59, 60, 61, 62, 63, 64, 65, 66, 68, 69, 70, 71, 72, 73, 75,
  76, 77, 78, 80, 81, 82, 84, 85, 86, 88, 89, 90, 92, 93, 94, 96,
  97, 99,100,102,103,105,106,108,109,111,112,114,115,117,119,120,
  122,124,125,127,129,130,132,134,136,137,139,141,143,145,146,148,
  150,152,154,156,158,160,162,164,166,168,170,172,174,176,178,180,
  182,184,186,188,191,193,195,197,199,202,204,206,209,211,213,215,
  218,220,223,225,227,230,232,235,237,240,242,245,247,250,252,255
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
  byte ar = 255, ag = 255, ab = 255;
  float brightness = DEFAULT_BRIGHTNESS;
  float jitter = DEFAULT_JITTER;

  void blend(const Pattern &o, float mix) {
    float mix1 = 1.0f - mix;
    r = (byte)((float)r * mix1 + (float)o.r * mix);
    g = (byte)((float)g * mix1 + (float)o.g * mix);
    b = (byte)((float)b * mix1 + (float)o.b * mix);
    ar = (byte)((float)ar * mix1 + (float)o.ar * mix);
    ag = (byte)((float)ag * mix1 + (float)o.ag * mix);
    ab = (byte)((float)ab * mix1 + (float)o.ab * mix);
    brightness = brightness * mix1 + o.brightness * mix;
    jitter = jitter * mix1 + o.jitter * mix;
  }
} patterns[MAX_PATTERNS];

enum SettingsStage {
  idle,
  settingTint,
  settingBrightness,
  settingJitter,
  settingAccent,
  batteryMeter,
};

struct MenuState {
  int currentPattern = 1;
  int lastPattern = 1;
  bool powerOn = false;
  SettingsStage settingsStage = idle;
  unsigned long settingsEnterTime = 0;
  unsigned long time = 0;
  unsigned long dt = 0;
  double animationCursor = 0.0f;
  double fadeCursor = 0.0f;
  double powerOffCursor = 0.0f;
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
  menu.lastPattern = menu.currentPattern;
  menu.currentPattern = patternIndex;
  menu.fadeCursor = 1.0;
}

void saveToEEPROM();

void cycleSettingStages() {
  menu.settingsEnterTime = millis();
  switch (menu.settingsStage) {
    case idle:
      menu.settingsStage = settingTint;
      patterns[menu.currentPattern] = Pattern();
      break;
    case settingTint:
      menu.settingsStage = settingBrightness;
      break;
    case settingBrightness:
      menu.settingsStage = settingJitter;
      break;
    case settingJitter:
      menu.settingsStage = settingAccent;
      break;
    case settingAccent:
      saveToEEPROM();
      menu.settingsStage = idle;
      break;
    case batteryMeter:
      menu.settingsStage = idle;
      break;
  }
}

void enterBatteryMeter() {
  menu.settingsStage = batteryMeter;
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
  } else if (input.clicks == 2) {
    input.enterSettings = true;
    enterBatteryMeter();
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

float lerp(float x, float y, float a) {
  return x * (1.0f - a) + y * a;
}

void renderPattern(const struct Pattern& p); // Compiler bug requires me to have this?

void renderPattern(const struct Pattern& p) {
  menu.animationCursor = fmod(menu.animationCursor + (double)menu.dt * JITTER_SPEED, 60.0);

  float animationCursorf = (float)menu.animationCursor;
  float animationCursor6 = animationCursorf * 6.0f;
  float animationCursor10 = animationCursorf * 10.0f;

  float jitterBase = 0.5f * p.jitter;
  float jitterScale = 0.25f * p.jitter;

  for (int i=0; i<NUMPIXELS; i++) {
    float fi = (float)i;
    float jitter = jitterBase + jitterScale * (
      sin(animationCursor6 + 0.1f * fi * fi) + 
      sin(animationCursor10 + fi)
    );
    float mix = 1.0f - jitter;
    float alpha = mix * p.brightness;
    
    pixels.setPixelColor(i, pixels.Color(
      gamma((p.ar * jitter + p.r * mix) * alpha),
      gamma((p.ag * jitter + p.g * mix) * alpha),
      gamma((p.ab * jitter + p.b * mix) * alpha)
    ));
  }
  pixels.show();
}

#define getSettingValue(period) modf((double)(millis() - menu.settingsEnterTime) * (1.0 / (period)), NULL)

void renderSetTint() {
  Pattern &p = patterns[menu.currentPattern];
  double hue = getSettingValue(10000.0) * (360.0 + 60.0);
  if (hue > 360.0) {
    p.r = p.g = p.b = 255;
  } else {
    hueToColor(hue, p.r, p.g, p.b);
  }
  renderPattern(p);
}

void renderSetAccent() {
  Pattern &p = patterns[menu.currentPattern];
  double hue = getSettingValue(10000.0) * (60.0 + 360.0 + 60.0);
  if (hue <= 60.0) {
    p.ar = p.r;
    p.ag = p.g;
    p.ab = p.b;
  } else if (hue > 360.0 + 60.0) {
    p.ar = p.ag = p.ab = 255;
  } else {
    hueToColor(hue - 60.0, p.ar, p.ag, p.ab);
  }
  renderPattern(p);
}

double withDefault(double x, double defaultValue, double from, double to) {
  if (x < 0.1f) return defaultValue;
  return from + (x - 0.1f) * (1.0f / 0.9f) * (to - from);
}

void renderSetBrightness() {
  Pattern &p = patterns[menu.currentPattern];
  p.brightness = withDefault(getSettingValue(10000.0), DEFAULT_BRIGHTNESS, 0.0, 1.0);
  renderPattern(p);
}

void renderSetJitter() {
  Pattern &p = patterns[menu.currentPattern];
  p.jitter = withDefault(getSettingValue(10000.0), DEFAULT_JITTER, 0.0, 1.0);
  renderPattern(p);
}

#define BATTERY_VOLTAGE_FULL 4.2
#define BATTERY_VOLTAGE_EMPTY 2.8

void renderBatteryMeter() {
  unsigned long adcValue = analogRead(12); // Read internal 1.1V reference relative to VCC
  double batteryVoltage = (1024.0 * 1.1) / adcValue;
  double batteryUsage = max(0.0, min(1.0, 
    (batteryVoltage - BATTERY_VOLTAGE_EMPTY) / 
    (BATTERY_VOLTAGE_FULL - BATTERY_VOLTAGE_EMPTY)
  ));

  for (int i=0; i<NUMPIXELS; i++) {
    float value = max(0.0, min(1.0,
      batteryUsage * (NUMPIXELS - 1) - (NUMPIXELS - i - 2)
    ));
    value *= 0.2f; // Let's not kill the battery while displaying it

    if (i < 2) {
      pixels.setPixelColor(i, pixels.Color(
        gamma(255 * value),
        0,
        0
      ));

    } else if (i >= NUMPIXELS - 2) {
      pixels.setPixelColor(i, pixels.Color(
        0,
        gamma(255 * value),
        0
      ));

    } else {
      pixels.setPixelColor(i, pixels.Color(
        gamma(255 * value),
        gamma(255 * value),
        0
      ));

    }
  }
  pixels.show();

}

void renderBlank() {
  pixels.clear();
  pixels.show();
}

void loadFromEEPROM() {
  if (EEPROM.read(0) == EEPROM_VERSION) {
    for (size_t i = 0; i < sizeof(patterns); i += 1) {
      ((byte*)patterns)[i] = EEPROM.read(i + 1);
    }
  }
}

void saveToEEPROM() {
  EEPROM.write(0, EEPROM_VERSION);
  for (size_t i = 0; i < sizeof(patterns); i += 1) {
    EEPROM.write(i + 1, ((byte*)patterns)[i]);
  }
}

void sleep() {
  digitalWrite(STATUS_PIN, LOW);
  // Configure INT0 as low level interrupt
  MCUCR &= ~(_BV(ISC01) | _BV(ISC00));
    
  GIMSK |= _BV(INT0);                     // Enable INT0 interrupt 
  ADCSRA &= ~_BV(ADEN);                   // ADC off
  set_sleep_mode(SLEEP_MODE_PWR_DOWN); 

  sleep_enable();                         // Sets the Sleep Enable bit in the MCUCR Register (SE BIT)
  sei();                                  // Enable interrupts
  
  sleep_cpu();                            // sleep

  cli();                                  // Disable interrupts
  sleep_disable();                        // Clear SE bit
  ADCSRA |= _BV(ADEN);                    // ADC on
  GIMSK &= ~_BV(INT0);                    // Disable INT0 interrupt 

  sei();                                  // Enable interrupts
  digitalWrite(STATUS_PIN, HIGH);
}

ISR(INT0_vect) {}

void setup() {
#if defined(__AVR_ATtiny85__) && (F_CPU == 16000000)
  clock_prescale_set(clock_div_1);
#endif
  
  pinMode(LEDS_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(STATUS_PIN, OUTPUT);
  digitalWrite(STATUS_PIN, HIGH);

  analogReference(DEFAULT);

  menu.time = millis();
  loadFromEEPROM();

  resetInputState();
}

void loop() {
  unsigned long time = millis();
  menu.dt = time - menu.time;
  menu.time = time;

  if (menu.powerOn) {
    if (menu.powerOffCursor != 1.0) {
      menu.powerOffCursor = min(1.0, menu.powerOffCursor + menu.dt * POWEROFF_SPEED);
    }    
  } else {
    if (menu.powerOffCursor != 0.0) {
      menu.powerOffCursor = max(0.0, menu.powerOffCursor - menu.dt * POWEROFF_SPEED);
    }
  }

  if (menu.fadeCursor != 0.0) {
    menu.fadeCursor = max(0.0, menu.fadeCursor - menu.dt * FADE_SPEED);
  }
  
  checkButtonState();
  updateTimer();
  
  switch (menu.settingsStage) {
    case idle: {
      if (menu.powerOffCursor == 0.0) {
        renderBlank();
        if (!timerCallback && !menu.powerOn) {
          sleep();
          return;
        }
        break;
      }
      Pattern p = patterns[menu.currentPattern];

      if (menu.fadeCursor != 0.0) {
        p.blend(patterns[menu.lastPattern], menu.fadeCursor);
      }
      p.brightness *= menu.powerOffCursor;

      renderPattern(p);
      break;
    }
    case settingTint: {
      renderSetTint();
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
    case settingAccent: {
      renderSetAccent();
      break;
    }
    case batteryMeter: {
      renderBatteryMeter();
      break;
    }
  }

  delay(15);
}
