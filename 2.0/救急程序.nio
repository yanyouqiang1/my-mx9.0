void setup() {
  // 开启 CDC 后，Serial 就是内置 USB
  Serial.begin(115200); 

  // 必须加这一行！等待 USB 串口准备就绪
  // 如果不加这一行，你会错过开机前几秒的打印
  while (!Serial) {
    delay(10);
  }

  Serial.println("");
  Serial.println("Hello! ESP32-S3 已经成功启动了！");
}

void loop() {
  Serial.println("程序运行中...");
  delay(1000);
}