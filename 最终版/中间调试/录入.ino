#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>

#define DEBOUNCE_DELAY 30 // 消抖时间（毫秒）

// ================= 1. 引脚及 I2C 定义 =================
#define I2C_SDA 14
#define I2C_SCL 13
#define MCP23017_ADDR 0x20 

// ESP32-S3 行引脚 (现在作为输入端，开启内置上拉)
const int rowPins[] = {1, 2, 42, 41, 40, 39, 38, 47, 21,12}; 
const int numRows = sizeof(rowPins) / sizeof(rowPins[0]);

// MCP23017 列引脚 (现在作为输出驱动端)
const int numCols = 16; 

Adafruit_MCP23X17 mcp;

// ================= 2. Cherry MX 9.0 按键定义 =================
struct KeyMap {
  const char* name;
  int row; 
  int col; 
};

KeyMap keyboardKeys[] = {
  // 左侧宏按键
  {"MA", -1, -1}, {"MB", -1, -1}, {"MC", -1, -1},
  
  // 顶部宏按键
  {"MR", -1, -1}, {"M1", -1, -1}, {"M2", -1, -1}, {"M3", -1, -1}, {"M4", -1, -1},
  {"M5", -1, -1}, {"M6", -1, -1}, {"M7", -1, -1}, {"M8", -1, -1}, {"M9", -1, -1},
  {"M10", -1, -1}, {"M11", -1, -1}, {"M12", -1, -1}, {"ME", -1, -1},

  // 顶部旋钮区及多媒体按键
  {"LIGHT (灯光)", -1, -1}, {"MUTE (静音)", -1, -1}, {"CPG", -1, -1},
  {"PREV (<<)", -1, -1}, {"PLAY/PAUSE (>||)", -1, -1}, {"NEXT (>>)", -1, -1},
  {"DIAL_BTN (旋钮下按)", -1, -1},

  // 第一排
  {"ESC", -1, -1}, {"F1", -1, -1}, {"F2", -1, -1}, {"F3", -1, -1}, {"F4", -1, -1},
  {"F5", -1, -1}, {"F6", -1, -1}, {"F7", -1, -1}, {"F8", -1, -1}, {"F9", -1, -1},
  {"F10", -1, -1}, {"F11", -1, -1}, {"F12", -1, -1},
  {"PRTSC", -1, -1}, {"SCROLL", -1, -1}, {"PAUSE", -1, -1}, {"CHERRY_LOGO", -1, -1},

  // 第二排 (数字排)
  {"GRAVE (~)", -1, -1}, {"1", -1, -1}, {"2", -1, -1}, {"3", -1, -1}, {"4", -1, -1},
  {"5", -1, -1}, {"6", -1, -1}, {"7", -1, -1}, {"8", -1, -1}, {"9", -1, -1},
  {"0", -1, -1}, {"MINUS (-)", -1, -1}, {"EQUAL (=)", -1, -1}, {"BACKSPACE", -1, -1},

  // 第三排
  {"TAB", -1, -1}, {"Q", -1, -1}, {"W", -1, -1}, {"E", -1, -1}, {"R", -1, -1},
  {"T", -1, -1}, {"Y", -1, -1}, {"U", -1, -1}, {"I", -1, -1}, {"O", -1, -1},
  {"P", -1, -1}, {"LBRACKET ([)", -1, -1}, {"RBRACKET (])", -1, -1}, {"BACKSLASH (\\)", -1, -1},

  // 第四排
  {"CAPS", -1, -1}, {"A", -1, -1}, {"S", -1, -1}, {"D", -1, -1}, {"F", -1, -1},
  {"G", -1, -1}, {"H", -1, -1}, {"J", -1, -1}, {"K", -1, -1}, {"L", -1, -1},
  {"SEMICOLON (;)", -1, -1}, {"QUOTE (')", -1, -1}, {"ENTER", -1, -1},

  // 第五排
  {"LSHIFT", -1, -1}, {"Z", -1, -1}, {"X", -1, -1}, {"C", -1, -1}, {"V", -1, -1},
  {"B", -1, -1}, {"N", -1, -1}, {"M", -1, -1}, {"COMMA (,)", -1, -1}, {"PERIOD (.)", -1, -1},
  {"SLASH (/)", -1, -1}, {"RSHIFT", -1, -1},

  // 第六排 (底栏)
  {"LCTRL", -1, -1}, {"LGUI (WIN)", -1, -1}, {"LALT", -1, -1}, {"SPACE", -1, -1},
  {"RALT", -1, -1}, {"RGUI (WIN)", -1, -1}, {"MENU", -1, -1}, {"RCTRL", -1, -1},

  // 功能控制区
  {"INSERT", -1, -1}, {"HOME", -1, -1}, {"PGUP", -1, -1},
  {"DELETE", -1, -1}, {"END", -1, -1}, {"PGDN", -1, -1},
  {"UP", -1, -1}, {"LEFT", -1, -1}, {"DOWN", -1, -1}, {"RIGHT", -1, -1},

  // 小键盘区
  {"NUM_LOCK", -1, -1}, {"KP_SLASH (/)", -1, -1}, {"KP_ASTERISK (*)", -1, -1}, {"KP_MINUS (-)", -1, -1},
  {"KP_7", -1, -1}, {"KP_8", -1, -1}, {"KP_9", -1, -1}, {"KP_PLUS (+)", -1, -1},
  {"KP_4", -1, -1}, {"KP_5", -1, -1}, {"KP_6", -1, -1},
  {"KP_1", -1, -1}, {"KP_2", -1, -1}, {"KP_3", -1, -1}, {"KP_ENTER", -1, -1},
  {"KP_0", -1, -1}, {"KP_DOT (.)", -1, -1}
};

const int totalKeys = sizeof(keyboardKeys) / sizeof(keyboardKeys[0]);
int currentKeyIdx = 0;
bool isWaitingForRelease = false;
int lastPressedRow = -1;
int lastPressedCol = -1;

void printCurrentPrompt();
void printFinalMapping();
void handleSerialCommand();

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);

  Serial.println("\n--- ESP32-S3 + MCP23017 逆向低电平防干扰扫描 ---");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
    Serial.println("【错误】MCP23017 未响应，请检查 I2C 引脚 13/14！");
    while (1) delay(100);
  }

  // 1. MCP23017 配置为输出驱动端，初始置高电平 (3.3V)
  for (int c = 0; c < numCols; c++) {
    mcp.pinMode(c, OUTPUT);
    mcp.digitalWrite(c, HIGH);
  }

  // 2. ESP32-S3 行引脚配置为输入端，开启内置上拉电阻 (强拉 3.3V，防悬空)
  for (int r = 0; r < numRows; r++) {
    pinMode(rowPins[r], INPUT_PULLUP);
  }

  Serial.println(">> 系统初始化完成，硬件悬空噪声已彻底消除！");
  printCurrentPrompt();
}

void loop() {
  handleSerialCommand();

  if (currentKeyIdx >= totalKeys) {
    delay(1000);
    return; 
  }

  // 1. 等待按键释放 (ESP32 行引脚恢复为 HIGH)
  if (isWaitingForRelease) {
    mcp.digitalWrite(lastPressedCol, LOW); // 继续激活该列
    delayMicroseconds(20);
    
    bool isStillPressed = (digitalRead(rowPins[lastPressedRow]) == LOW);
    mcp.digitalWrite(lastPressedCol, HIGH); // 关闭该列

    if (!isStillPressed) { // 已释放
      delay(DEBOUNCE_DELAY);
      isWaitingForRelease = false;
      currentKeyIdx++;
      printCurrentPrompt();
    }
    return;
  }

  // 2. 逆向矩阵扫描：依次将 MCP23017 的“列”拉低
  for (int c = 0; c < numCols; c++) {
    mcp.digitalWrite(c, LOW); // 激活当前列 (拉低至 0V)
    delayMicroseconds(20); 

    // 扫描 ESP32 的各个“行”引脚
    for (int r = 0; r < numRows; r++) {
      if (digitalRead(rowPins[r]) == LOW) { // 读到 LOW 说明按键按下！
        delay(DEBOUNCE_DELAY); // 防抖
        
        if (digitalRead(rowPins[r]) == LOW) {
          keyboardKeys[currentKeyIdx].row = r;
          keyboardKeys[currentKeyIdx].col = c;
          
          char colPinName[8];
          if (c < 8) sprintf(colPinName, "GPA%d", c);
          else sprintf(colPinName, "GPB%d", c - 8);

          Serial.printf("  [录入成功] -> 按键 '%s' -> Row:%d, Col:%d | (ESP32_GPIO:%d -> MCP23017_%s)\n", 
                        keyboardKeys[currentKeyIdx].name, r, c, rowPins[r], colPinName);
          
          lastPressedRow = r;
          lastPressedCol = c;
          isWaitingForRelease = true;
          break;
        }
      }
    }
    
    mcp.digitalWrite(c, HIGH); // 恢复高电平
    if (isWaitingForRelease) break;
  }
}

void printCurrentPrompt() {
  if (currentKeyIdx < totalKeys) {
    Serial.printf("\n========================================\n");
    Serial.printf("[%d/%d] 请按下按键: >>>  %s  <<<\n", currentKeyIdx + 1, totalKeys, keyboardKeys[currentKeyIdx].name);
    Serial.println(" (提示: 串口发送 's' 跳过此键, 'b' 后退上一步, 'p' 打印当前矩阵)");
    Serial.printf("========================================\n");
  } else {
    Serial.println("\n*** 恭喜！所有按键映射完成！ ***");
    printFinalMapping();
  }
}

void printFinalMapping() {
  Serial.println("\n------ 最终矩阵映射结果 ------\n");
  Serial.println("const int keyMatrixMap[][2] = {");
  for (int i = 0; i < totalKeys; i++) {
    Serial.printf("  {%2d, %2d}, // %-15s (ESP32 RowIdx:%2d, MCP ColIdx:%2d)\n", 
                  keyboardKeys[i].row, 
                  keyboardKeys[i].col, 
                  keyboardKeys[i].name,
                  keyboardKeys[i].row,
                  keyboardKeys[i].col);
  }
  Serial.println("};");
}

void handleSerialCommand() {
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == '\r' || cmd == '\n') return; 

    if (cmd == 's' || cmd == 'S') {
      Serial.printf(">> 已跳过: %s\n", keyboardKeys[currentKeyIdx].name);
      keyboardKeys[currentKeyIdx].row = -1;
      keyboardKeys[currentKeyIdx].col = -1;
      currentKeyIdx++;
      isWaitingForRelease = false;
      printCurrentPrompt();
    } 
    else if (cmd == 'b' || cmd == 'B') {
      if (currentKeyIdx > 0) {
        currentKeyIdx--;
        Serial.printf(">> 返回上一步: %s\n", keyboardKeys[currentKeyIdx].name);
        isWaitingForRelease = false;
        printCurrentPrompt();
      }
    } 
    else if (cmd == 'p' || cmd == 'P') {
      printFinalMapping();
      printCurrentPrompt();
    }
  }
}