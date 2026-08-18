const int pA = 41; 
const int pB = 47;

void setup() {
  Serial.begin(115200);
  pinMode(pA, INPUT_PULLUP);
  pinMode(pB, INPUT_PULLUP);
  Serial.println("电平实时监测开始...");
}

void loop() {
  // 每 100ms 打印一次电平，看转动时数值是否从 1 变到 0
  Serial.print("Pin 41: "); Serial.print(digitalRead(pA));
  Serial.print(" | Pin 47: "); Serial.println(digitalRead(pB));
  delay(100);
}