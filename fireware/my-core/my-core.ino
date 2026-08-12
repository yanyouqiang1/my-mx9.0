#include "USB.h"
#include "USBHIDKeyboard.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <LittleFS.h>

USBHIDKeyboard Keyboard;
Preferences preferences;

// ================= BLE 设定 =================
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
BLECharacteristic *pCharacteristic;
bool deviceConnected = false;
File meFile;

// ================= 引脚与极性 =================
#define DEBOUNCE_DELAY 15
const int rowPins[] = {4, 5, 6, 7, 15, 16, 17, 18, 3}; 
const int colPins[] = {1, 2, 42, 41, 40, 39, 38, 11, 45, 47, 21, 14, 13, 12, 46, 9, 10}; 
const int numRows = 9;
const int numCols = 17;

#define INPUT_MODE INPUT_PULLDOWN
#define DRIVE_LEVEL HIGH
#define IDLE_LEVEL LOW
#define PRESSED_VAL HIGH

// ================= 自定义宏按键标识 (核心修复点) =================
// 必须大于 255 (0xFF)，防止与标准键盘的 HID 键值发生冲突
#define MACRO_BASE 0x0100
#define K_M1  (MACRO_BASE + 1)
#define K_M2  (MACRO_BASE + 2)
#define K_M3  (MACRO_BASE + 3)
#define K_M4  (MACRO_BASE + 4)
#define K_M5  (MACRO_BASE + 5)
#define K_M6  (MACRO_BASE + 6)
#define K_M7  (MACRO_BASE + 7)
#define K_M8  (MACRO_BASE + 8)
#define K_M9  (MACRO_BASE + 9)
#define K_M10 (MACRO_BASE + 10)
#define K_M11 (MACRO_BASE + 11)
#define K_M12 (MACRO_BASE + 12)
#define K_MA  (MACRO_BASE + 13)
#define K_MB  (MACRO_BASE + 14)
#define K_MC  (MACRO_BASE + 15)
#define K_MR  (MACRO_BASE + 16)
#define K_ME  (MACRO_BASE + 17)

// ================= 键盘矩阵映射 =================
// 数据类型改为 uint16_t
const uint16_t keyMatrix[numRows][numCols] = {
  // Row 0
  {K_MR, K_M1, K_M2, K_M3, K_M4, K_M5, K_M6, K_M7, K_M8, K_M9, K_M10, K_M12, K_M11, K_ME, 0, 0, K_MC},
  // Row 1
  {KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, 0xCE, KEY_F12, 0xCF, 0, 0xD0, KEY_ESC},
  // Row 2
  {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', KEY_BACKSPACE, '=', KEY_INSERT, 0, KEY_HOME, '`'},
  // Row 3
  {KEY_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ']', '[', '\\', 0, 0xD4 /*DELETE*/, KEY_PAGE_UP},
  // Row 4
  {KEY_PAGE_DOWN, KEY_CAPS_LOCK, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\'', ';', 0, 0, KEY_RETURN, KEY_END},
  // Row 5
  {0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', KEY_UP_ARROW, KEY_RIGHT_SHIFT, KEY_LEFT_CTRL, 0, KEY_LEFT_GUI, KEY_LEFT_SHIFT},
  // Row 6 (小键盘区1 - 直接使用底层16进制，杜绝报错)
  {0xE4, 0xE5, 0xE6, 0xE1, 0xE2, 0xE3, 0xEA, 0xEB, 0xE0, 0, 0, 0, 0, K_MA, 0, K_MB, 0xDF},
  // Row 7 (扫描未发现)
  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  // Row 8 (小键盘区2)
  {' ', KEY_RIGHT_ALT, KEY_RIGHT_GUI, 0xED, KEY_RIGHT_CTRL, KEY_LEFT_ARROW, KEY_DOWN_ARROW, KEY_RIGHT_ARROW, 0xDB, 0xDC, 0xDD, 0xE7, 0xDE, 0xE8, 0, 0xE9, KEY_LEFT_ALT}
};

bool lastState[numRows][numCols] = {false};
unsigned long lastDebounceTime[numRows][numCols] = {0};

// ================= BLE 服务器回调 =================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; }
    void onDisconnect(BLEServer* pServer) { deviceConnected = false; BLEDevice::startAdvertising(); }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
        String data = pChar->getValue().c_str();
        if(data.length() == 0) return;

        if (data == "ME_START") {
            Serial.println("开始接收ME数据...");
            meFile = LittleFS.open("/me_base64.txt", FILE_WRITE);
        } 
        else if (data == "ME_END") {
            Serial.println("ME数据接收完毕。");
            if(meFile) meFile.close();
        } 
        else if (data.startsWith("ME_DATA:")) {
            String payload = data.substring(8);
            if(meFile) meFile.print(payload);
        }
        else if (data.startsWith("SET:")) {
            int firstColon = data.indexOf(':', 4);
            if(firstColon > 0) {
                String keyName = data.substring(4, firstColon);
                String payload = data.substring(firstColon + 1);
                preferences.putString(keyName.c_str(), payload);
                Serial.printf("已保存 %s -> %s\n", keyName.c_str(), payload.c_str());
            }
        }
    }
};

void setupBLE() {
    BLEDevice::init("ESP32_KeyBoard_Config");
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    pCharacteristic->setCallbacks(new MyCallbacks());
    pService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
}

// 在固件中替换 executeMacro 函数
void executeMacro(String keyName) {
    if (keyName == "ME") {
        if (LittleFS.exists("/me_base64.txt")) {
            File f = LittleFS.open("/me_base64.txt", FILE_READ);
            while (f.available()) {
                char c = f.read();
                Keyboard.print(c);
                delay(2); 
            }
            f.close();
        }
        return;
    }

    String macroData = preferences.getString(keyName.c_str(), "");
    if (macroData.length() == 0) return;

    if (macroData.startsWith("SEQ:")) {
        // 连续输入模式，支持 [ENTER], [TAB], [ESC]
        String seq = macroData.substring(4);
        int i = 0;
        while (i < seq.length()) {
            if (seq[i] == '[' ) {
                int endBracket = seq.indexOf(']', i);
                if (endBracket != -1) {
                    String tag = seq.substring(i + 1, endBracket);
                    if (tag == "ENTER") Keyboard.write(KEY_RETURN);
                    else if (tag == "TAB") Keyboard.write(KEY_TAB);
                    else if (tag == "ESC") Keyboard.write(KEY_ESC);
                    else if (tag == "BACKSPACE") Keyboard.write(KEY_BACKSPACE);
                    else if (tag == "UP") Keyboard.write(KEY_UP_ARROW);
                    else if (tag == "DOWN") Keyboard.write(KEY_DOWN_ARROW);
                    else Keyboard.print("[" + tag + "]"); // 原样输出未知标签
                    i = endBracket + 1;
                    continue;
                }
            }
            Keyboard.print(seq[i]);
            i++;
            delay(5);
        }
    } 
    else if (macroData.startsWith("CMB:")) {
        // 组合键模式：支持无限个按键同时按下
        String cmb = macroData.substring(4);
        int commaIdx = 0;
        while (cmb.length() > 0) {
            commaIdx = cmb.indexOf(',');
            String kCodeStr = (commaIdx == -1) ? cmb : cmb.substring(0, commaIdx);
            int kCode = kCodeStr.toInt();
            if(kCode > 0) Keyboard.press((uint8_t)kCode);
            if (commaIdx == -1) break;
            cmb = cmb.substring(commaIdx + 1);
        }
        delay(50);
        Keyboard.releaseAll();
    }
}

// 核心修复点：将参数转为 uint16_t 以匹配上面的定义
String getMacroNameByCode(uint16_t code) {
    switch(code) {
        case K_M1: return "M1"; case K_M2: return "M2"; case K_M3: return "M3";
        case K_M4: return "M4"; case K_M5: return "M5"; case K_M6: return "M6";
        case K_M7: return "M7"; case K_M8: return "M8"; case K_M9: return "M9";
        case K_M10: return "M10"; case K_M11: return "M11"; case K_M12: return "M12";
        case K_MA: return "MA"; case K_MB: return "MB"; case K_MC: return "MC";
        case K_MR: return "MR"; case K_ME: return "ME";
        default: return "";
    }
}

void setup() {
    Serial.begin(115200);
    Keyboard.begin();
    USB.begin();

    preferences.begin("macros", false);
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
    }

    for (int c = 0; c < numCols; c++) pinMode(colPins[c], INPUT_MODE);
    for (int r = 0; r < numRows; r++) {
        pinMode(rowPins[r], OUTPUT);
        digitalWrite(rowPins[r], IDLE_LEVEL);
    }

    setupBLE();
    Serial.println("键盘初始化完毕，BLE等待连接...");
}

void loop() {
    for (int r = 0; r < numRows; r++) {
        digitalWrite(rowPins[r], DRIVE_LEVEL);
        delayMicroseconds(10); 

        for (int c = 0; c < numCols; c++) {
            bool currentState = (digitalRead(colPins[c]) == PRESSED_VAL);
            
            if (currentState != lastState[r][c]) {
                if (millis() - lastDebounceTime[r][c] > DEBOUNCE_DELAY) {
                    lastState[r][c] = currentState;
                    lastDebounceTime[r][c] = millis();
                    
                    // 核心修复点：使用 uint16_t 读取键值
                    uint16_t keycode = keyMatrix[r][c];
                    if (keycode == 0) continue; 

                    if (currentState) {
                        if (keycode >= MACRO_BASE) {
                            executeMacro(getMacroNameByCode(keycode));
                        } else {
                            // 标准键盘键值向下强转为 uint8_t 发送给电脑
                            Keyboard.press((uint8_t)keycode);
                        }
                    } else {
                        if (keycode < MACRO_BASE) {
                            Keyboard.release((uint8_t)keycode);
                        }
                    }
                }
            }
        }
        digitalWrite(rowPins[r], IDLE_LEVEL);
    }
}