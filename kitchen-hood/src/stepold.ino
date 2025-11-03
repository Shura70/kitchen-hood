#include <Arduino.h>

// ---------------------- ПОДКЛЮЧЕНИЯ ----------------------
// DRV8825:
//   DIR  → 2   (направление вращения)
//   STEP → 3   (шаг)
//   EN   → 4   (разрешение драйвера, LOW = включен)
// Концевик (Home switch):
//   LIMIT → 7  (концевой выключатель, замыкается на GND при срабатывании)
// Сенсорная кнопка 233CH:
//   TOUCH → 8  (лог. HIGH при касании)
const int DIR_PIN    = 2;
const int STEP_PIN   = 3;
const int ENABLE_PIN = 4;
const int LIMIT_PIN  = 7;
const int TOUCH_PIN  = 8;

// ---------------------- НАСТРОЙКИ ----------------------
// Параметры механики и движения
const float MAX_DISTANCE_MM = 125.0;   // длина хода в мм
const long STEPS_PER_MM = 400;        // шагов на мм (200*16 для DRV8825 на 1.8° с микрошагом 1/16)
const long MAX_STEPS = MAX_DISTANCE_MM * STEPS_PER_MM; // 
const int STEP_DELAY_US = 40;         // задержка между шагами (скорость)
const long SAFETY_MARGIN_STEPS = 15*STEPS_PER_MM; // запас, если концевик не найден
// NEW: Таймаут для парковки
const long PARK_THRESHOLD_STEPS = 5 * STEPS_PER_MM; // порог "близко к max" (5 мм)
const unsigned long TIMEOUT_MS = 60000*30; // 1 минута для закрывания
// ---------------------- ПЕРЕМЕННЫЕ ----------------------
bool direction = true;   // true = от 0 к максимуму, false = к 0
bool moving = false;
bool homed = false;
long position = 0;       // текущая позиция в шагах
bool lastTouchState = true; //false;
// NEW: Для таймаута парковки
bool timeoutActive = false;
unsigned long stopTime = 0;

unsigned long lastSerialTime = 0; // для отладки
// ---------------------- ВСПОМОГАТЕЛЬНЫЕ ----------------------
void stepMotor(bool dir) {
  digitalWrite(DIR_PIN, dir);
  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(STEP_PIN, LOW);
  delayMicroseconds(STEP_DELAY_US);
}

// ---------------------- HOMING ----------------------
void home() {
  Serial.println("Homing start...");
  digitalWrite(ENABLE_PIN, LOW); // активировать драйвер
  digitalWrite(DIR_PIN, LOW);    // направление к концевику
  direction = false;  // установить направление на "назад"
  long safetyCounter = 0;
  while (digitalRead(LIMIT_PIN) == HIGH) { // пока концевик не сработал
    stepMotor(false);
    safetyCounter++;
    if (safetyCounter > (MAX_STEPS + SAFETY_MARGIN_STEPS)) {
      Serial.println("!!! ERROR: Limit switch not found during homing.");
      digitalWrite(ENABLE_PIN, HIGH); // выключить мотор
       while (digitalRead(LIMIT_PIN) == HIGH);
      Serial.println("Retrying homing...");
    }
    
  }

  Serial.println("Home detected!");
  position = 0;
  homed = true;
  direction = true;
  safetyCounter = 0;
  // Отъехать от концевика
  while (safetyCounter < 2*STEPS_PER_MM) {
    stepMotor(true);
    safetyCounter++;
  }
  digitalWrite(ENABLE_PIN, HIGH); // выключить мотор
  Serial.println("Homing complete.");
  delay(300);
}

// ---------------------- НАЧАЛО ДВИЖЕНИЯ ----------------------
void startMove() {
  moving = true;
  Serial.print("Start moving ");
  Serial.println(direction ? "FORWARD" : "BACKWARD");
  digitalWrite(ENABLE_PIN, LOW);
}

// ---------------------- ОСТАНОВКА ----------------------
void stopMove() {
  moving = false;
  digitalWrite(ENABLE_PIN, HIGH);
  // NEW: Запуск таймаута парковки, если близко к max
  if (position >= (MAX_STEPS - PARK_THRESHOLD_STEPS)) {
    timeoutActive = true;
    stopTime = millis();
    Serial.println("Timeout for parking started (near MAX).");
  } else {
    Serial.println("Position in middle - no parking timeout.");
  }
  Serial.println("Movement stopped.");
  delay(200);
}

// ---------------------- СМЕНА НАПРАВЛЕНИЯ ----------------------
void toggleDirection() {
  direction = !direction;
  Serial.print("Direction changed: ");
  Serial.println(direction ? "FORWARD" : "BACKWARD");
}

// ---------------------- СЧИТЫВАНИЕ КАСАНИЯ ----------------------
bool touchPressed() {
  bool touchState = digitalRead(TOUCH_PIN);
  bool pressed = (touchState && !lastTouchState);
  lastTouchState = touchState;
  return pressed;
}

// ---------------------- SETUP ----------------------
void setup() {
  Serial.begin(9600);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(STEP_PIN, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);
  pinMode(LIMIT_PIN, INPUT_PULLUP);
  pinMode(TOUCH_PIN, INPUT);

  digitalWrite(ENABLE_PIN, HIGH); // выключен при старте
  delay(100);
  home();
  stopMove();
  direction = true; // после калибровки двигаться вперёд
  Serial.println("System ready. Waiting for touch...");
  Serial.println(direction ? "FORWARD" : "BACKWARD");
}

void loop() {
  // NEW: Проверка таймаута парковки (если активен и истек)
  if (timeoutActive && !moving && (millis() - stopTime > TIMEOUT_MS)) {
    Serial.println("Timeout expired - parking to HOME.");
    home(); // автоматическая парковка
    timeoutActive = false;
  }
  // Проверяем касание кнопки
  if (touchPressed()) {
    // NEW: Сброс таймаута при любом касании
    timeoutActive = false;
    if (moving) {
      // Касание во время движения -> стоп
      stopMove();
    } else {
      // Касание при остановке -> смена направления и движение
      if (!homed)
        toggleDirection(); // если не откалибровано, просто сменить направление
      delay(50);
      startMove();
      homed = false; // после старта требуется новая калибровка
    }
  }

  // Если мотор в движении — выполняем шаг
  if (moving) {
    stepMotor(direction);

    // Обновляем позицию в зависимости от направления
    if (direction) position++;
    else position--;

    // --- Проверка на достижение границ ---

    // Достигнут максимум
    if (direction && position >= MAX_STEPS) {
      stopMove();
      position = MAX_STEPS;
      Serial.println("Reached MAX, switching direction -> BACKWARD");
      // direction = false;
    }

    // Достигнут HOME
    if (!direction && digitalRead(LIMIT_PIN) == LOW) {
      stopMove();
      position = 0;
      direction = true; // готов к движению вперёд
      Serial.println("Reached HOME, switching direction -> FORWARD");
      home(); // повторная калибровка
    }

    // Безопасность: если концевик не найден за пределами диапазона
    if (!direction && position < -SAFETY_MARGIN_STEPS) {
      stopMove();
      Serial.println("!!! ERROR: Homing limit not detected. Emergency stop!");
      while (digitalRead(LIMIT_PIN) == HIGH) 
        delay(100); // ждём срабатывания концевика
      home();

    }
  }
}
