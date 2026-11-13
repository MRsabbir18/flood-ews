/*
 * STEP 1 TEST - run this first when your parts arrive.
 * Only ESP32 + JSN-SR04T. Prints distance in the Serial Monitor (115200 baud).
 * Wiring: VCC->5V(VIN), GND->GND, TRIG->GPIO5, ECHO->1k->GPIO18->2k->GND
 * Point the sensor at a wall 30-200 cm away. If the number follows your hand, the sensor works.
 */
#define PIN_TRIG 5
#define PIN_ECHO 18

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
}

void loop() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(12);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long d = pulseIn(PIN_ECHO, HIGH, 30000);
  if (d == 0) Serial.println("No echo - check wiring / distance (min 20 cm)");
  else { Serial.print("Distance: "); Serial.print(d * 0.0343 / 2.0, 1); Serial.println(" cm"); }
  delay(500);
}
