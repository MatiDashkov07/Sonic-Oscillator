#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==========================================
// 1. HARDWARE CONFIGURATION (ESP32-S3)
// ==========================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
#define I2C_SDA 4
#define I2C_SCL 5

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Pin Mapping
const int SW_PIN        = 15;
const int POT_PIN_PITCH = 1;
const int POT_PIN_TONE  = 2;
const int BUZZER_PIN    = 16;
const int PIN_DT        = 7;
const int PIN_CLK       = 6;

// Audio Constants
const int LEDC_CHANNEL    = 0;
const int LEDC_RESOLUTION = 8;
const int BASE_FREQ       = 5000;
const int MIN_FREQ_SAFE   = 350;

// ==========================================
// 2. STATE MANAGEMENT
// ==========================================
enum UIState {
  STATE_PLAYING,
  STATE_MENU    
};

UIState currentUIState = STATE_MENU;
unsigned long lastInteractionTime = 0;
const unsigned long MENU_TIMEOUT = 10000;

const char* menuItems[] = {"SQUARE", "SAW", "TRIANGLE", "NOISE"};
int menuIndex = 0;           

// === תיקון קריטי: מתחיל במינוס 1 (כלום) ===
int selectedMode = -1; 

bool buttonActive = false;       
bool longPressHandled = false;   
unsigned long pressStartTime = 0; 
const unsigned long LONG_PRESS_TIME = 800; 
bool isMuted = true; 

volatile int virtualPosition = 0; 
int lastPosition = 0;
volatile unsigned long lastInterruptTime = 0;

// DSP & Locking Mechanism
int currentPitch = 0;
int currentTone = 0;
int lastAppliedFreq = 0;
int lastAppliedDuty = -1;

unsigned long lastKnobMoveTime = 0;
const int LOCK_TIMEOUT = 500; 
const int HYSTERESIS = 4;     

bool forceUpdate = false;
unsigned long lastDisplayUpdate = 0;

// ==========================================
// 3. ISR & HELPER FUNCTIONS
// ==========================================
void IRAM_ATTR updateEncoder() {
  unsigned long interruptTime = millis();
  if (interruptTime - lastInterruptTime > 10) {
    if (digitalRead(PIN_CLK) != digitalRead(PIN_DT)) {
      virtualPosition++;
    } else {
      virtualPosition--;
    }
    lastInterruptTime = interruptTime;
  }
}

int readStableADC(int pin) {
  long sum = 0;
  for(int i=0; i<32; i++) {
    sum += analogRead(pin);
    delayMicroseconds(10); 
  }
  return sum / 32;
}

void playFeedbackTone(int frequency, int duration) {
    ledcChangeFrequency(LEDC_CHANNEL, frequency, LEDC_RESOLUTION);
    ledcWrite(LEDC_CHANNEL, 128); 
    delay(duration);
    ledcWrite(LEDC_CHANNEL, 0); 
    forceUpdate = true; 
    lastAppliedFreq = 0; 
}

// ==========================================
// 4. GRAPHICS
// ==========================================
void drawCenteredText(String text, int y, int size) {
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (SCREEN_WIDTH - w) / 2;
  display.setCursor(x, y);
  display.print(text);
}

void showSplashScreen() {
  display.clearDisplay();
  display.drawRect(0, 0, 128, 32, SSD1306_WHITE);
  display.display(); delay(200);
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText("SONIC LAB", 5, 2);
  display.display(); delay(500);
  drawCenteredText("v3.8 SafeMode", 22, 1);
  display.display(); delay(1500); 
}

void drawWaveIcon(int mode, int x, int y) {
  uint16_t color = SSD1306_WHITE;
  switch(mode) {
    case 0: // Square
      display.drawLine(x, y+10, x+5, y+10, color);
      display.drawLine(x+5, y+10, x+5, y+2, color);
      display.drawLine(x+5, y+2, x+15, y+2, color);
      display.drawLine(x+15, y+2, x+15, y+10, color);
      display.drawLine(x+15, y+10, x+20, y+10, color);
      break;
    case 1: // Saw
      display.drawLine(x, y+10, x+10, y+2, color);
      display.drawLine(x+10, y+2, x+10, y+10, color);
      display.drawLine(x+10, y+10, x+20, y+2, color);
      display.drawLine(x+20, y+2, x+20, y+10, color);
      break;
    case 2: // Triangle
      display.drawLine(x, y+10, x+5, y+2, color);
      display.drawLine(x+5, y+2, x+10, y+10, color);
      display.drawLine(x+10, y+10, x+15, y+2, color);
      display.drawLine(x+15, y+2, x+20, y+10, color);
      break;
    case 3: // Noise
      for(int i=0; i<20; i+=2) {
         int h = (i % 5) * 2 + 2; 
         int offset = (i % 3 == 0) ? 4 : 8;
         display.drawLine(x+i, y+offset, x+i, y+offset+h, color);
      }
      break;
  }
}

void updateDisplay() {
  display.clearDisplay();

  if (isMuted) {
    display.fillRect(0, 0, 128, 32, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    drawCenteredText("MUTE", 9, 2);
    display.setTextColor(SSD1306_WHITE);
  } 
  else if (currentUIState == STATE_MENU || selectedMode == -1) { 
    // מציגים תפריט גם אם אנחנו ב-Playing אבל עוד לא נבחר מצב
    drawCenteredText("- SELECT MODE -", 0, 1);
    drawCenteredText(menuItems[menuIndex], 12, 2);
    display.fillTriangle(4, 20, 10, 14, 10, 26, SSD1306_WHITE);
    display.fillTriangle(124, 20, 118, 14, 118, 26, SSD1306_WHITE);
    int startDots = 64 - (4*10)/2;
    for(int i=0; i<4; i++) {
       if (i == menuIndex) display.fillRect(startDots + (i*10), 30, 6, 2, SSD1306_WHITE);
       else display.drawPixel(startDots + (i*10) + 2, 30, SSD1306_WHITE);
    }
  } 
  else { // PLAYING (רק אם נבחר משהו)
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(menuItems[selectedMode]);
    
    int currentFreq = map(currentPitch, 0, 4095, MIN_FREQ_SAFE, (selectedMode==3 ? 5000 : 2000));
    String freqStr = String(currentFreq) + " Hz";
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(freqStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(128 - w, 0);
    display.print(freqStr);

    drawWaveIcon(selectedMode, 2, 14); 
    display.drawRect(30, 14, 90, 14, SSD1306_WHITE);
    int barW = map(currentPitch, 0, 4095, 0, 86);
    display.fillRect(32, 16, barW, 10, SSD1306_WHITE);  
  }
  display.display();
}

// ==========================================
// 5. SETUP
// ==========================================
void setup() {
  // הגנה מפני קריסה בהדלקה (נותן זמן ל-USB להתייצב)
  delay(100); 

  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) for(;;);
  
  showSplashScreen();

  pinMode(SW_PIN, INPUT_PULLUP);
  pinMode(PIN_DT, INPUT);
  pinMode(PIN_CLK, INPUT);

  ledcSetup(LEDC_CHANNEL, BASE_FREQ, LEDC_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, LEDC_CHANNEL);
  attachInterrupt(digitalPinToInterrupt(PIN_DT), updateEncoder, FALLING);

  currentPitch = readStableADC(POT_PIN_PITCH);
  currentTone = readStableADC(POT_PIN_TONE);

  updateDisplay(); 
  Serial.println("--- SYSTEM READY ---");
}

void loop() {
  // A. כפתור
  int reading = digitalRead(SW_PIN);
  if (reading == LOW && !buttonActive) {
    buttonActive = true; pressStartTime = millis(); longPressHandled = false; delay(5); 
  }
  
  if (buttonActive && reading == LOW) {
    if ((millis() - pressStartTime > LONG_PRESS_TIME) && !longPressHandled) {
      isMuted = !isMuted; longPressHandled = true;
      playFeedbackTone(500, 100);
      forceUpdate = true; 
      updateDisplay(); 
    }
  }
  
  if (reading == HIGH && buttonActive) {
    buttonActive = false;
    if (!longPressHandled) {
       selectedMode = menuIndex; // כאן נבחר הגל לראשונה!
       currentUIState = STATE_PLAYING; 
       playFeedbackTone(2000, 50);
       forceUpdate = true; 
    }
  }

  // B. UI Logic
  if (virtualPosition != lastPosition) {
    lastPosition = virtualPosition;
    menuIndex = abs(virtualPosition) % 4;
    currentUIState = STATE_MENU;
    lastInteractionTime = millis(); 
  }
  
  if (currentUIState == STATE_MENU) {
    if (millis() - lastInteractionTime > MENU_TIMEOUT) {
      currentUIState = STATE_PLAYING;
    }
  }

  if (millis() - lastDisplayUpdate > 33) {
    updateDisplay();
    lastDisplayUpdate = millis();
  }

  // C. AUDIO ENGINE (SAFE START)
  // הסאונד יפעל רק אם אנחנו לא במיוט וגם נבחר מצב חוקי (גדול מ-1-)
  if (!isMuted && selectedMode != -1) {
      
      int newPitch = readStableADC(POT_PIN_PITCH);
      int newTone = readStableADC(POT_PIN_TONE);
      
      bool shouldUpdatePitch = false;
      bool shouldUpdateTone = false;

      // מנגנון נעילה
      if (abs(newPitch - currentPitch) > HYSTERESIS) {
          currentPitch = newPitch;
          lastKnobMoveTime = millis();
          shouldUpdatePitch = true;
      } else {
          if (millis() - lastKnobMoveTime < LOCK_TIMEOUT) {
              currentPitch = newPitch;
              shouldUpdatePitch = true;
          }
      }

      if (abs(newTone - currentTone) > HYSTERESIS) {
          currentTone = newTone;
          shouldUpdateTone = true;
      }

      if (shouldUpdatePitch || shouldUpdateTone || forceUpdate) {
          
          int targetFrequency = map(currentPitch, 0, 4095, MIN_FREQ_SAFE, 2000);
          if (targetFrequency < MIN_FREQ_SAFE) targetFrequency = MIN_FREQ_SAFE;
          int targetDuty = map(currentTone, 0, 4095, 0, 127); 

          if (selectedMode == 3) { // NOISE
             int noiseCeiling = map(currentPitch, 0, 4095, 600, 5000);
             if (noiseCeiling < 600) noiseCeiling = 600;
             ledcWrite(LEDC_CHANNEL, random(0, 255)); 
             ledcChangeFrequency(LEDC_CHANNEL, random(600, noiseCeiling), LEDC_RESOLUTION);
          } 
          else { // STANDARD
             if (targetFrequency != lastAppliedFreq || forceUpdate) {
                 ledcChangeFrequency(LEDC_CHANNEL, targetFrequency, LEDC_RESOLUTION);
                 lastAppliedFreq = targetFrequency;
             }
             if (targetDuty != lastAppliedDuty || forceUpdate) {
                 ledcWrite(LEDC_CHANNEL, targetDuty);
                 lastAppliedDuty = targetDuty;
             }
          }
          forceUpdate = false; 
      }
  } else {
    // השתקה מוחלטת
    if (lastAppliedDuty != 0) {
        ledcWrite(LEDC_CHANNEL, 0);
        lastAppliedDuty = 0;
    }
  }
}