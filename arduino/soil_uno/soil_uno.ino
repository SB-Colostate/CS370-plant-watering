// Uno R3 as the sensor front-end for the Pi.
// Reads the soil probe (A0) and the HC-SR04 ultrasonic, prints one line per second over USB serial.
//
// Wiring:
//   Soil sensor  S -> A0    V -> D7 (powered only while reading)    G -> GND
//   HC-SR04      VCC -> 5V  GND -> GND  Trig -> D9  Echo -> D10   (Uno is 5V: no divider needed)
//   Uno USB -> Pi USB (/dev/ttyACM0) or desktop (COM3)
//
// Output, one line per second:   SOIL,<0-1023>,DIST,<cm>
//   SOIL: this module reads ~0 dry, rises when wet.
//   DIST: -1 means no echo (out of range / wiring problem).

const int SOIL_PWR = 7;
const int SOIL_AO  = A0;
const int TRIG     = 9;
const int ECHO     = 10;

void setup() {
  pinMode(SOIL_PWR, OUTPUT);
  digitalWrite(SOIL_PWR, LOW);
  pinMode(TRIG, OUTPUT);
  digitalWrite(TRIG, LOW);
  pinMode(ECHO, INPUT);
  Serial.begin(9600);
}

int readSoil() {
  digitalWrite(SOIL_PWR, HIGH);
  delay(100);
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(SOIL_AO);
    delay(5);
  }
  digitalWrite(SOIL_PWR, LOW);
  return sum / 8;
}

float readOnceCm() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  unsigned long us = pulseIn(ECHO, HIGH, 30000UL);  // 30 ms timeout (~5 m)
  if (us == 0) return -1;
  return us / 58.0;
}

float readDistCm() {
  // median of 5 to reject bad echoes
  float v[5];
  for (int i = 0; i < 5; i++) { v[i] = readOnceCm(); delay(60); }
  for (int i = 1; i < 5; i++) {
    float k = v[i]; int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return v[2];
}

void loop() {
  int soil = readSoil();
  float dist = readDistCm();
  Serial.print("SOIL,");
  Serial.print(soil);
  Serial.print(",DIST,");
  Serial.println(dist, 1);
  delay(600);
}
