// s3-bluetooth-audio/s3-bt-audio.ino
// USB复合设备主程序：键盘 + 蓝牙音频 + USB音频
// 基于 最终版/s3.ino + BTAudioManager (Task1) + USBAudioManager (Task2)

#include "USB.h"
#include "USBAudio.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h" // 多媒体音量及播放控制
#include "USBHIDSystemControl.h"   // 引入休眠、唤醒、关机等系统控制库
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <FFat.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <esp_system.h>

// 引入 ESP32-S3 特有的寄存器头文件，用于控制软件直接复位至 ROM 刷机模式
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "components/bt_audio/bt_audio.h"
#include "components/usb_audio/usb_audio.h"

USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDSystemControl SystemControl; // 系统控制对象实例
Preferences preferences;
Adafruit_MCP23X17 mcp;

// 音频管理器实例
BTAudioManager btAudio;
USBAudioManager usbAudio;

// ================= 引脚与通讯定义 =================
#define I2C_SDA 14
#define I2C_SCL 13
#define MCP23017_ADDR 0x20

#define RX_PIN 10  // 串口 RX 接 C3 的 20
#define TX_PIN 9   // 串口 TX 接 C3 的 10

const int rowPins[] = {1, 2, 42, 41, 40, 39, 38, 47, 21, 12};
const int numRows = 10;
const int numCols = 16;

#define DEBOUNCE_DELAY 20
#define PRESSED_VAL LOW

#define MACRO_BASE 0x1000
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
#define K_LOGO (MACRO_BASE + 18)

// 多媒体宏定义
#define K_NEXT (MACRO_BASE + 19)
#define K_PLAY (MACRO_BASE + 20)
#define K_PREV (MACRO_BASE + 21)

// FN 键定义（固件内部逻辑宏键，不向电脑发送按键值）
#define K_FN   (MACRO_BASE + 22)
bool fnPressed = false; // 记录 FN 键当前状态

#ifndef CONSUMER_CONTROL_SCAN_NEXT_TRACK
#define CONSUMER_CONTROL_SCAN_NEXT_TRACK 0x00B5
#endif
#ifndef CONSUMER_CONTROL_SCAN_PREVIOUS_TRACK
#define CONSUMER_CONTROL_SCAN_PREVIOUS_TRACK 0x00B6
#endif
#ifndef CONSUMER_CONTROL_PLAY_PAUSE
#define CONSUMER_CONTROL_PLAY_PAUSE 0x00CD
#endif

uint16_t keyMatrix[numRows][numCols] = {0};

// CHERRY_LOGO 状态标志（默认为关闭）
bool cherryLogoEnabled = false;

void initKeyMatrix() {
    // Row 0
    keyMatrix[0][0] = KEY_LEFT_ALT; keyMatrix[0][1] = 0xE9; keyMatrix[0][2] = 0xE8; keyMatrix[0][3] = 0xE7;
    keyMatrix[0][4] = 0xDE; keyMatrix[0][5] = 0xDD; keyMatrix[0][6] = 0xDC; keyMatrix[0][7] = 0xDB;
    keyMatrix[0][8] = KEY_RIGHT_ARROW; keyMatrix[0][9] = KEY_DOWN_ARROW; keyMatrix[0][10] = KEY_LEFT_ARROW;
    keyMatrix[0][11] = KEY_RIGHT_CTRL; keyMatrix[0][12] = 0xED; keyMatrix[0][13] = K_FN;
    keyMatrix[0][14] = KEY_RIGHT_ALT; keyMatrix[0][15] = ' ';

    // Row 1
    keyMatrix[1][0] = 0xDF; keyMatrix[1][1] = K_MB; keyMatrix[1][2] = K_MA;
    keyMatrix[1][3] = K_NEXT; // NEXT (>>) 媒体下一首 (FN时为唤醒)
    keyMatrix[1][4] = K_PLAY; // PLAY/PAUSE (>||) 媒体暂停/播放 (FN时为睡眠)
    keyMatrix[1][5] = K_PREV; // PREV (<<) 媒体上一首 (FN时为关机)
    keyMatrix[1][6] = K_LOGO; keyMatrix[1][7] = 0xE0;
    keyMatrix[1][8] = 0xEB; keyMatrix[1][9] = 0xEA; keyMatrix[1][10] = 0xE3; keyMatrix[1][11] = 0xE2;
    keyMatrix[1][12] = 0xE1; keyMatrix[1][13] = 0xE6; keyMatrix[1][14] = 0xE5; keyMatrix[1][15] = 0xE4;

    // Row 2
    keyMatrix[2][0] = KEY_LEFT_SHIFT; keyMatrix[2][1] = KEY_LEFT_GUI; keyMatrix[2][2] = KEY_LEFT_CTRL;
    keyMatrix[2][3] = KEY_UP_ARROW; keyMatrix[2][4] = KEY_RIGHT_SHIFT; keyMatrix[2][5] = '/';
    keyMatrix[2][6] = '.'; keyMatrix[2][7] = ','; keyMatrix[2][8] = 'm'; keyMatrix[2][9] = 'n';
    keyMatrix[2][10] = 'b'; keyMatrix[2][11] = 'v'; keyMatrix[2][12] = 'c'; keyMatrix[2][13] = 'x';
    keyMatrix[2][14] = 'z';

    // Row 4
    keyMatrix[4][0] = KEY_END; keyMatrix[4][1] = KEY_RETURN; keyMatrix[4][3] = '\''; keyMatrix[4][4] = ';';
    keyMatrix[4][5] = 'l'; keyMatrix[4][6] = 'k'; keyMatrix[4][7] = 'j'; keyMatrix[4][8] = 'h';
    keyMatrix[4][9] = 'g'; keyMatrix[4][10] = 'f'; keyMatrix[4][11] = 'd'; keyMatrix[4][12] = 's';
    keyMatrix[4][13] = 'a'; keyMatrix[4][14] = KEY_CAPS_LOCK; keyMatrix[4][15] = 0xD6;

    // Row 5
    keyMatrix[5][0] = KEY_PAGE_UP; keyMatrix[5][1] = KEY_DELETE; keyMatrix[5][2] = '\\'; keyMatrix[5][3] = ']';
    keyMatrix[5][4] = '['; keyMatrix[5][5] = 'p'; keyMatrix[5][6] = 'o'; keyMatrix[5][7] = 'i';
    keyMatrix[5][8] = 'u'; keyMatrix[5][9] = 'y'; keyMatrix[5][10] = 't'; keyMatrix[5][11] = 'r';
    keyMatrix[5][12] = 'e'; keyMatrix[5][13] = 'w'; keyMatrix[5][14] = 'q'; keyMatrix[5][15] = KEY_TAB;

    // Row 6
    keyMatrix[6][0] = '`'; keyMatrix[6][1] = KEY_HOME; keyMatrix[6][2] = KEY_INSERT; keyMatrix[6][3] = KEY_BACKSPACE;
    keyMatrix[6][4] = '='; keyMatrix[6][5] = '-'; keyMatrix[6][6] = '0'; keyMatrix[6][7] = '9';
    keyMatrix[6][8] = '8'; keyMatrix[6][9] = '7'; keyMatrix[6][10] = '6'; keyMatrix[6][11] = '5';
    keyMatrix[6][12] = '4'; keyMatrix[6][13] = '3'; keyMatrix[6][14] = '2'; keyMatrix[6][15] = '1';

    // Row 7
    keyMatrix[7][0] = KEY_ESC; keyMatrix[7][1] = 0xD0; keyMatrix[7][2] = 0xCF; keyMatrix[7][3] = 0xCE;
    keyMatrix[7][4] = KEY_F12; keyMatrix[7][5] = KEY_F11; keyMatrix[7][6] = KEY_F10; keyMatrix[7][7] = KEY_F9;
    keyMatrix[7][8] = KEY_F8; keyMatrix[7][9] = KEY_F7; keyMatrix[7][10] = KEY_F6; keyMatrix[7][11] = KEY_F5;
    keyMatrix[7][12] = KEY_F4; keyMatrix[7][13] = KEY_F3; keyMatrix[7][14] = KEY_F2; keyMatrix[7][15] = KEY_F1;

    // Row 8
    keyMatrix[8][0] = K_MC; keyMatrix[8][2] = K_ME; keyMatrix[8][3] = K_M12; keyMatrix[8][4] = K_M11;
    keyMatrix[8][5] = K_M10; keyMatrix[8][6] = K_M9; keyMatrix[8][7] = K_M8; keyMatrix[8][8] = K_M7;
    keyMatrix[8][9] = K_M6; keyMatrix[8][10] = K_M5; keyMatrix[8][11] = K_M4; keyMatrix[8][12] = K_M3;
    keyMatrix[8][13] = K_M2; keyMatrix[8][14] = K_M1; keyMatrix[8][15] = K_MR;
}

bool lastState[numRows][numCols] = {false};
unsigned long lastDebounceTime[numRows][numCols] = {0};

#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914c"
#define CHARACTERISTIC_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
BLECharacteristic *pCharacteristic;

bool deviceConnected = false;
bool oldDeviceConnected = false;

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; }
    void onDisconnect(BLEServer* pServer) { deviceConnected = false; }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
        String data = pChar->getValue().c_str();
        if(data.length() == 0) return;

        if (data == "ME_START") {
            File f = FFat.open("/me_hex.txt", FILE_WRITE);
            if(f) f.close();
            Serial.println("ME 开始写入，已清空/me_hex.txt");
        }
        else if (data == "ME_END") {
            Serial1.println("N_ME");
            Serial.println("ME 写入完成并关闭，已通知 C3");
        }
        else if (data.startsWith("ME_DATA:")) {
            File f = FFat.open("/me_hex.txt", FILE_APPEND);
            if(f) {
                f.print(data.substring(8));
                f.close();
            }
        }
        else if (data.startsWith("SET:")) {
            int firstColon = data.indexOf(':', 4);
            if(firstColon > 0) {
                String keyName = data.substring(4, firstColon);
                String payload = data.substring(firstColon + 1);
                preferences.putString(keyName.c_str(), payload);

                if (keyName == "MR") {
                    Serial1.println("N_ME");
                }
            }
        }
    }
};

void executeMacro(String keyName) {
    if (keyName == "ME") {
        if (FFat.exists("/me_hex.txt")) {
            File f = FFat.open("/me_hex.txt", FILE_READ);
            if (f) {
                Keyboard.print("[HEXS]");
                delay(20);

                while (f.available()) {
                    Keyboard.print((char)f.read());
                    delay(1);
                }
                f.close();

                delay(20);
                Keyboard.print("[HEXE]");
                return;
            }
        }
    }

    String macroData = preferences.getString(keyName.c_str(), "");
    if (macroData.length() == 0) return;

    if (macroData.startsWith("SEQ:")) {
        String seq = macroData.substring(4);
        int i = 0;
        while (i < seq.length()) {
            if (seq[i] == '[' ) {
                int endBracket = seq.indexOf(']', i);
                if (endBracket != -1) {
                    String tag = seq.substring(i + 1, endBracket);
                    bool matched = true;
                    if (tag == "ENTER") Keyboard.write(KEY_RETURN);
                    else if (tag == "TAB") Keyboard.write(KEY_TAB);
                    else if (tag == "ESC") Keyboard.write(KEY_ESC);
                    else if (tag == "BACKSPACE") Keyboard.write(KEY_BACKSPACE);
                    else matched = false;

                    if (matched) {
                        i = endBracket + 1;
                        continue;
                    }
                }
            }
            Keyboard.print(seq[i++]);
            delay(5);
        }
    }
    else if (macroData.startsWith("CMB:")) {
        String cmb = macroData.substring(4);
        int commaIdx = 0;
        while (cmb.length() > 0) {
            commaIdx = cmb.indexOf(',');
            uint8_t kCode = (commaIdx == -1) ? cmb.toInt() : cmb.substring(0, commaIdx).toInt();
            if(kCode > 0) Keyboard.press(kCode);
            if (commaIdx == -1) break;
            cmb = cmb.substring(commaIdx + 1);
        }
        delay(50); Keyboard.releaseAll();
    }
}

String getMacroNameByCode(uint16_t code) {
    switch(code) {
        case K_M1: return "M1"; case K_M2: return "M2"; case K_M3: return "M3";
        case K_M4: return "M4"; case K_M5: return "M5"; case K_M6: return "M6";
        case K_M7: return "M7"; case K_M8: return "M8"; case K_M9: return "M9";
        case K_M10: return "M10"; case K_M11: return "M11"; case K_M12: return "M12";
        case K_MA: return "MA"; case K_MB: return "MB"; case K_MC: return "MC";
        case K_MR: return "MR"; case K_ME: return "ME"; case K_LOGO: return "LOGO";
        default: return "";
    }
}

// ================= I2C 自主恢复函数 =================
void recoverI2CBus() {
    Serial.println("检测到 I2C 故障，正在尝试自我恢复并重新初始化 MCP23017...");
    Wire.end();
    delay(10);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(25);

    if (mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        for (int c = 0; c < numCols; c++) {
            mcp.pinMode(c, OUTPUT);
            mcp.digitalWrite(c, HIGH);
        }
        Serial.println("I2C 及 MCP23017 恢复成功！");
    } else {
        Serial.println("I2C 恢复失败，MCP23017 未响应...");
    }
}

// 键盘矩阵扫描函数（状态机架构）
void scanKeyboardMatrix() {
    // 周期检测 I2C 状态，防止芯片锁死
    static unsigned long lastI2CCheck = 0;
    if (millis() - lastI2CCheck > 500) {
        lastI2CCheck = millis();
        Wire.beginTransmission(MCP23017_ADDR);
        if (Wire.endTransmission() != 0) {
            recoverI2CBus();
            return;
        }
    }

    // 键盘扫描逻辑
    for (int c = 0; c < numCols; c++) {
        mcp.writeGPIOAB(~(1 << c));
        delayMicroseconds(20);

        for (int r = 0; r < numRows; r++) {
            bool currentState = (digitalRead(rowPins[r]) == PRESSED_VAL);
            if (currentState != lastState[r][c]) {
                if (millis() - lastDebounceTime[r][c] > DEBOUNCE_DELAY) {
                    lastState[r][c] = currentState;
                    lastDebounceTime[r][c] = millis();

                    uint16_t keycode = keyMatrix[r][c];
                    if (keycode == 0) continue;

                    // ================= 按键被按下 =================
                    if (currentState) {
                        if (cherryLogoEnabled) {
                            Serial1.println("N_KEY_PRESS");
                        }

                        // 拦截 FN 键状态
                        if (keycode == K_FN) {
                            fnPressed = true;
                            Serial.println("FN 键被按下");
                        }
                        else if (keycode >= MACRO_BASE) {
                            if (keycode == K_PLAY) {
                                if (fnPressed) {
                                    // FN + PLAY/PAUSE (>||) = 睡眠
                                    Serial.println("触发指令：FN + PLAY -> 睡眠");
                                    SystemControl.press(SYSTEM_CONTROL_STANDBY);
                                    SystemControl.release();
                                } else {
                                    ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
                                }
                            } else if (keycode == K_NEXT) {
                                if (fnPressed) {
                                    // FN + NEXT (>>) = 唤醒电脑
                                    Serial.println("触发指令：FN + NEXT -> 唤醒");
                                    SystemControl.press(SYSTEM_CONTROL_WAKE_HOST);
                                    SystemControl.release();
                                    Keyboard.press(' ');
                                    Keyboard.release(' ');
                                } else {
                                    ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT_TRACK);
                                }
                            } else if (keycode == K_PREV) {
                                if (fnPressed) {
                                    // FN + PREV (<<) = 关机
                                    Serial.println("触发指令：FN + PREV -> 关机");
                                    SystemControl.press(SYSTEM_CONTROL_POWER_OFF);
                                    SystemControl.release();
                                } else {
                                    ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS_TRACK);
                                }
                            } else if (keycode != K_LOGO) {
                                executeMacro(getMacroNameByCode(keycode));
                            }
                        } else {
                            Keyboard.press((uint8_t)keycode);
                        }
                    }
                    // ================= 按键被松开 =================
                    else {
                        if (keycode == K_FN) {
                            fnPressed = false;
                            Serial.println("FN 键已释放");
                        }
                        else if (keycode < MACRO_BASE) {
                            Keyboard.release((uint8_t)keycode);
                        } else {
                            if (keycode == K_PLAY || keycode == K_NEXT || keycode == K_PREV) {
                                ConsumerControl.release();
                            } else if (keycode == K_LOGO) {
                                unsigned long pressedDuration = millis() - lastDebounceTime[r][c];

                                if (pressedDuration >= 2000 && pressedDuration < 8000) {
                                    Serial1.println("N_REBOT");
                                    Serial.println("检测到长按 LOGO 2S~8S 释放，发送 N_REBOT 普通重启...");
                                    delay(200);
                                    esp_restart();
                                }
                                else if (pressedDuration < 2000) {
                                    cherryLogoEnabled = !cherryLogoEnabled;
                                    preferences.putBool("cherryLogo", cherryLogoEnabled);
                                    Serial.printf("CHERRY_LOGO 状态已切换为: %s\n", cherryLogoEnabled ? "开启 (ON)" : "关闭 (OFF)");

                                    executeMacro("LOGO");
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    mcp.writeGPIOAB(0xFFFF);
}

void setup() {
    Serial.begin(115200);
    Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

    // USB 设置
    USB.VID(0x303A);
    USB.PID(0x0020);
    USB.productName("YYQ-MX9.0");
    USB.manufacturerName("YYQ");

    Keyboard.begin();
    ConsumerControl.begin();
    SystemControl.begin();

    // USB Audio 初始化
    usbAudio.begin();

    USB.begin();

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(25);

    if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        Serial.println("MCP23017 初始化失败，总线可能存在故障！尝试进行急救...");
        recoverI2CBus();
    }

    preferences.begin("macros", false);

    cherryLogoEnabled = preferences.getBool("cherryLogo", false);
    Serial.printf("读取到 CHERRY_LOGO 初始状态: %s\n", cherryLogoEnabled ? "开启 (ON)" : "关闭 (OFF)");

    if (!FFat.begin(true)) {
        Serial.println("警告：FFat(FATFS) 挂载失败！请检查分区表设置。");
    } else {
        Serial.println("FFat(FATFS) 挂载成功。");
    }

    initKeyMatrix();

    for (int c = 0; c < numCols; c++) { mcp.pinMode(c, OUTPUT); mcp.digitalWrite(c, HIGH); }
    for (int r = 0; r < numRows; r++) { pinMode(rowPins[r], INPUT_PULLUP); }

    // 蓝牙音频初始化
    btAudio.begin();

    BLEDevice::init("YYQ-MX9.0");
    BLEDevice::setMTU(517);

    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCharacteristic->setCallbacks(new MyCallbacks());
    pService->start();
    BLEDevice::startAdvertising();

    // 创建蓝牙音频任务
    xTaskCreatePinnedToCore(
        [](void* param) { btAudio.task(param); },
        "bt_audio",
        4096,
        NULL,
        2,
        NULL,
        0  // Core 0
    );
}

// 调度器所需的时间变量
unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL = 3;

void loop() {
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        BLEDevice::startAdvertising();
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }

    // 监听来自 C3 旋钮的音量控制指令
    while (Serial1.available()) {
        String cmd = Serial1.readStringUntil('\n');
        cmd.trim();
        if (cmd == "V+") {
            ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
            ConsumerControl.release();
        } else if (cmd == "V-") {
            ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
            ConsumerControl.release();
        }
    }

    // ======== 长按 LOGO (Row 1, Col 6) 8 秒重启至烧录模式 ========
    if (lastState[1][6]) {
        unsigned long pressedDuration = millis() - lastDebounceTime[1][6];
        if (pressedDuration >= 8000) {
            Serial.println("检测到长按 LOGO 达到 8 秒，强行向 C3 发送 N_ROOT 并写入寄存器复位至硬件 ROM 刷机模式...");

            for (int k = 0; k < 15; k++) {
                Serial1.println("N_ROOT");
                delay(100);
            }

            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
            esp_restart();
        }
    }

    // ======== 周期性触发键盘扫描 (非阻塞) ========
    if (millis() - lastScanTime >= SCAN_INTERVAL) {
        lastScanTime = millis();
        scanKeyboardMatrix();
    }

    delay(1);
}
