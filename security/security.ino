/*
 * Охоронний пристрій на Arduino Uno
 * Датчики: PIR HC-SR501 (рух), ультразвуковий далекомір HC-SR04 (відстань).
 * Індикація: LCD 16x2 з I2C-адаптером PCF8574, зумер, червоний та зелений світлодіоди.
 * Керування: кнопка «Arm/Disarm» (постановка / зняття з охорони).
 * Ліцензія: MIT.
 * Бібліотеки: LiquidCrystal I2C 1.1.2 (Frank de Brabander), Wire (вбудована).
 */
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- Призначення пінів ----------
#define PIR_PIN      2    // вихід OUT датчика HC-SR501
#define BUTTON_PIN   4    // кнопка Arm/Disarm (INPUT_PULLUP)
#define LED_GREEN    6    // зелений світлодіод - «знято з охорони»
#define LED_RED      7    // червоний світлодіод - «охорона / тривога»
#define BUZZER_PIN   8    // зумер (через резистор 100 Ом)
#define TRIG_PIN     9    // HC-SR04 TRIG
#define ECHO_PIN     10   // HC-SR04 ECHO
// LCD підключено по шині I2C: SDA - A4, SCL - A5

// ---------- Налаштування ----------
#define LCD_ADDR          0x27   // I2C-адреса адаптера PCF8574
#define DIST_THRESHOLD_CM 50     // поріг спрацювання далекоміра, см
#define EXIT_DELAY_MS     5000   // затримка на вихід після постановки, мс
#define ALARM_TIME_MS     15000  // тривалість звукової тривоги, мс
#define MEASURE_PERIOD_MS 200    // період опитування датчиків, мс
#define DEBOUNCE_MS       50     // антидребезг кнопки, мс

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// Стани системи (скінченний автомат)
enum State { DISARMED, ARMING, ARMED, ALARM };
State state = DISARMED;

unsigned long stateSince = 0;     // час входу в поточний стан
unsigned long lastMeasure = 0;
int  distanceCm = -1;             // остання виміряна відстань, см
bool motion = false;              // останній стан PIR
char alarmSource[6] = "";         // що викликало тривогу: PIR / DIST
uint16_t alarmCount = 0;          // лічильник тривог

// Кнопка з антидребезгом
bool lastBtnReading = HIGH, btnState = HIGH;
unsigned long lastBtnChange = 0;

// Вимірювання відстані HC-SR04. Повертає см або -1, якщо відлуння немає
int readDistanceCm() {
  noTone(BUZZER_PIN);                   // пауза звуку на час виміру (~6 мс),
                                        // щоб переривання таймера не спотворювали результат
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);         // імпульс 10 мкс запускає вимір
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  // pulseInLong() рахує час через micros(), тому не збивається
  // перериваннями таймера, які генерує tone() під час тривоги
  unsigned long us = pulseInLong(ECHO_PIN, HIGH, 30000UL); // тайм-аут 30 мс (~5 м)
  if (us == 0) return -1;
  return (int)(us / 58);                // 58 мкс на 1 см (туди й назад)
}

// Повертає true один раз на кожне натискання кнопки
bool buttonPressed() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastBtnReading) lastBtnChange = millis();
  lastBtnReading = reading;
  if (millis() - lastBtnChange > DEBOUNCE_MS && reading != btnState) {
    btnState = reading;
    if (btnState == LOW) return true;   // натискання (активний LOW)
  }
  return false;
}

// Перехід у новий стан
void setState(State s) {
  state = s;
  stateSince = millis();
  if (s != ALARM) noTone(BUZZER_PIN);
  Serial.print(F("STATE -> "));
  Serial.println(s == DISARMED ? F("DISARMED") : s == ARMING ? F("ARMING")
                 : s == ARMED ? F("ARMED") : F("ALARM"));
}

// Друкує рядок row рівно 16 символами (решта заповнюється пробілами)
void printRow(uint8_t row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// Виведення інформації на LCD 16x2
// Рядок 1 - стан системи, рядок 2 - відстань і стан PIR
void updateLcd() {
  char s[17];
  switch (state) {
    case DISARMED: printRow(0, "STATUS: DISARMED"); break;
    case ARMING:
      snprintf(s, sizeof(s), "ARMING IN %d s",
               (int)((EXIT_DELAY_MS - (millis() - stateSince)) / 1000 + 1));
      printRow(0, s);
      break;
    case ARMED:    printRow(0, "STATUS: ARMED"); break;
    case ALARM:
      snprintf(s, sizeof(s), "!ALARM! %s #%u", alarmSource, alarmCount);
      printRow(0, s);
      break;
  }
  // формат "123cm PIR:1" (11 символів)
  if (distanceCm < 0) snprintf(s, sizeof(s), "---cm PIR:%d", motion);
  else snprintf(s, sizeof(s), "%3dcm PIR:%d", distanceCm, motion);
  printRow(1, s);
}

// Світлодіоди та зумер відповідно до стану
void updateOutputs() {
  bool blink = (millis() / 250) % 2;
  switch (state) {
    case DISARMED:
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_RED, LOW);
      break;
    case ARMING:                        // червоний блимає, короткі сигнали
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, blink);
      if ((millis() - stateSince) % 1000 < 60) tone(BUZZER_PIN, 2000);
      else noTone(BUZZER_PIN);
      break;
    case ARMED:
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, HIGH);
      break;
    case ALARM:                         // сирена з двох тонів
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, blink);
      if (millis() - stateSince < ALARM_TIME_MS) tone(BUZZER_PIN, blink ? 1000 : 1500);
      else noTone(BUZZER_PIN);
      break;
  }
}

void setup() {
  Serial.begin(9600);
  pinMode(PIR_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  lcd.init();                           // ініціалізація LCD через I2C
  lcd.backlight();                      // увімкнення підсвічування
  printRow(0, "SECURITY SYSTEM");
  printRow(1, "SR501+SR04");
  delay(1500);
  setState(DISARMED);
}

void loop() {
  // 1. Кнопка: постановка / зняття з охорони
  if (buttonPressed()) {
    if (state == DISARMED) setState(ARMING);
    else setState(DISARMED);
  }

  // 2. Періодичне опитування датчиків
  if (millis() - lastMeasure >= MEASURE_PERIOD_MS) {
    lastMeasure = millis();
    motion = digitalRead(PIR_PIN) == HIGH;
    distanceCm = readDistanceCm();

    Serial.print(F("D=")); Serial.print(distanceCm);
    Serial.print(F("cm PIR=")); Serial.println(motion);

    // 3. Логіка станів
    if (state == ARMING && millis() - stateSince >= EXIT_DELAY_MS) {
      setState(ARMED);
    } else if (state == ARMED) {
      bool distTrig = distanceCm > 0 && distanceCm < DIST_THRESHOLD_CM;
      if (motion || distTrig) {
        strcpy(alarmSource, motion ? "PIR" : "DIST");
        alarmCount++;
        setState(ALARM);
      }
    }
    updateLcd();
  }

  // 4. Індикація
  updateOutputs();
}
