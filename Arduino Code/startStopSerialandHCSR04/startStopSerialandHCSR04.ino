#include <Wire.h>
#include <AS5600.h>
#include <Servo.h>

Servo sortingServo;
AMS_5600 ams5600;

// ── Encoder Variables ─────────────────────────────────────────────────────────
volatile int32_t encoderCount = 0;
int16_t previousRaw = -1;
const int16_t TICKS_PER_REV = 4096;
const int16_t HALF_TICKS = 2048;

// ── Timing & RPM Variables ────────────────────────────────────────────────────
unsigned long lastCalcTime = 0;
const uint16_t CALC_INTERVAL_MS = 50;
int32_t lastCount = 0;

// ── Motor Pins ────────────────────────────────────────────────────────────────
const int in1 = 4;
const int in2 = 5;
const int enA = 6;

// ── State Tracking ────────────────────────────────────────────────────────────
unsigned long motorStartTime = 0;
bool motorRunning = false;
bool taskCompleted = false; 
bool isRunning = false; 

// ── HC-SR04 Ultrasonic Sensor ─────────────────────────────────────────────────
const int trigPin = 9;
const int echoPin = 10;
unsigned long lastDistanceCalcTime = 0;
const uint16_t DISTANCE_INTERVAL_MS = 150; // Measured slightly faster for responsiveness
float currentDistance_cm = 0.0;

// ── Spawn Trigger Variables ───────────────────────────────────────────────────
unsigned long outOfRangeStartTime = 0;
bool outOfRangeActive = false;
bool spawnSent = false; // Prevents repeated spawning per event

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); // Higher baud rate strongly recommended
  Wire.begin();
  sortingServo.attach(11);
  sortingServo.write(0); // Start at 0 degrees

  while (ams5600.detectMagnet() == 0) {
    Serial.println("Waiting for magnet...");
    delay(300);
  }
  Serial.println("Magnet OK!");

  previousRaw = (int16_t)ams5600.getRawAngle();
  
  pinMode(in1, OUTPUT);
  pinMode(in2, OUTPUT);
  pinMode(enA, OUTPUT);
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-blocking distance measurement
// ─────────────────────────────────────────────────────────────────────────────
void measureDistance() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // 26000us timeout prevents the code from freezing if no object is detected
  long duration = pulseIn(echoPin, HIGH, 26000); 
  
  if (duration == 0) {
    currentDistance_cm = -1.0; // Indicates out of range
  } else {
    currentDistance_cm = (duration * 0.0343) / 2;
  }
}

// ─────────────────────────────────────────────────────────────────────────────

void CounterClockwise(int speed){
  if (!motorRunning) {
    digitalWrite(in1, HIGH);
    digitalWrite(in2, LOW);
    analogWrite(enA, speed);
    motorStartTime = millis(); 
    motorRunning = true;
    Serial.println(">>> Motor Started...");
  }
}

void CounterClockwiseCalibration(int speed, int32_t counter) {
  if (taskCompleted) return;

  if (!motorRunning) {
    digitalWrite(in1, HIGH);
    digitalWrite(in2, LOW);
    analogWrite(enA, speed);
    motorStartTime = millis();
    motorRunning = true;
    Serial.println(">>> Calibration Started...");
  }

  if (motorRunning && (millis() - motorStartTime >= 18500)) {
    digitalWrite(in1, LOW);
    digitalWrite(in2, LOW);
    analogWrite(enA, 0);

    float durationSec = (millis() - motorStartTime) / 1000.0;
    float speedTicksPerSec = (float)encoderCount / durationSec;

    motorRunning = false;
    taskCompleted = true;

    Serial.println(">>> Calibration Done. Motor Stopped.");
    Serial.print(">>> Avg Speed: ");
    Serial.print(speedTicksPerSec);
    Serial.println(" counts/sec");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void updateEncoder() {
  int16_t currentRaw = (int16_t)ams5600.getRawAngle();
  if (previousRaw == -1) { previousRaw = currentRaw; return; }

  int16_t delta = currentRaw - previousRaw;
  if (delta >  HALF_TICKS) delta -= TICKS_PER_REV;
  if (delta < -HALF_TICKS) delta += TICKS_PER_REV;

  encoderCount += delta;
  previousRaw = currentRaw;
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  updateEncoder();
  unsigned long now = millis();

  // 1. Serial Command Handling
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input == "start") {
      isRunning = true;
      taskCompleted = false;
      motorRunning = false;
      encoderCount = 0;
      Serial.println("--- System Active ---");
    } 
    else if (input == "stop") {
      isRunning = false;
      digitalWrite(in1, LOW);
      digitalWrite(in2, LOW);
      analogWrite(enA, 0);
      motorRunning = false;      // ← reset so motor can restart next time
      taskCompleted = false;     // ← reset so calibration can run again
      Serial.println("--- System Manual Stop ---");
    }

    else if (input.startsWith("servo:")) {
    int angle = input.substring(6).toInt();
    angle = constrain(angle, 0, 180); // Safety clamp
    sortingServo.write(angle);
    Serial.print("Servo moved to: ");
    Serial.println(angle);
    }
  }

  // 2. Main Logic — only runs when isRunning = true
  if (isRunning) {

    // ── Motor Control for calibration ──────────────────────────────────────────
    //CounterClockwiseCalibration(127, encoderCount);  // ← THIS WAS MISSING

    // for moving
    CounterClockwise(127);

    // ── Distance Sensing ───────────────────────────────────────
    if (now - lastDistanceCalcTime >= DISTANCE_INTERVAL_MS) {
      measureDistance();
      lastDistanceCalcTime = now;

      if (currentDistance_cm < 0) {
        Serial.println("Distance: OUT OF RANGE");

        if (!outOfRangeActive) {
          outOfRangeActive = true;
          outOfRangeStartTime = now;
          spawnSent = false; //to spawn in simulation
          // you have to calibrate this time in(ms)
        } else if (!spawnSent && (now - outOfRangeStartTime >= 150)) {
          Serial.println("SPAWN");
          spawnSent = true;
        }

      } else {
        Serial.print("Distance: ");
        Serial.print(currentDistance_cm);
        Serial.println(" cm");
        outOfRangeActive = false;
        spawnSent = false;
      }
    }
  }

  // 3. RPM Background Calculation (always runs)
  if (now - lastCalcTime >= CALC_INTERVAL_MS) {
    lastCount = encoderCount;
    lastCalcTime = now;
  }
}