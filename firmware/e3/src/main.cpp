#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <vector>

// ================= 引脚定义 =================
const int LED_STATUS_PIN = 10;

// ================= 全局状态 =================
bool btConnected = false;
bool isStreaming = false;
String btDeviceName = "";
String btMacAddress = "";

// ================= 蓝牙扫描结果 =================
struct BTScanResult {
    String name;
    String address;
    int rssi;
    bool connected;
};
std::vector<BTScanResult> btDevices;

// ================= LED状态 =================
unsigned long lastLedUpdate = 0;

void handleBluetoothState() {
    unsigned long now = millis();
    if (!btConnected) {
        if (now - lastLedUpdate > 500) {
            lastLedUpdate = now;
            digitalWrite(LED_STATUS_PIN, !digitalRead(LED_STATUS_PIN));
        }
    } else {
        digitalWrite(LED_STATUS_PIN, HIGH);
    }
}

// ================= 蓝牙回调 =================
static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch(event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char bda_str[18];
            snprintf(bda_str, sizeof(bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                     param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
                     param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);

            // 检查是否已存在
            for (auto& r : btDevices) {
                if (r.address == String(bda_str)) return;
            }

            BTScanResult result;
            result.address = String(bda_str);
            result.name = "";
            result.rssi = 0;
            result.connected = false;
            btDevices.push_back(result);

            Serial.printf("[BT] Found: %s\n", bda_str);
            break;
        }
        default:
            break;
    }
}

static void a2d_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch(event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t state = param->conn_stat.state;
            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                btConnected = true;
                char bda_str[18];
                snprintf(bda_str, sizeof(bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                         param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1], param->conn_stat.remote_bda[2],
                         param->conn_stat.remote_bda[3], param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);
                btMacAddress = String(bda_str);
                Serial.println("[BT] Connected: " + btMacAddress);
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                btConnected = false;
                isStreaming = false;
                Serial.println("[BT] Disconnected");
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                isStreaming = true;
                Serial.println("[BT] Audio Started");
            } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                isStreaming = false;
                Serial.println("[BT] Audio Stopped");
            }
            break;
        }
        default:
            break;
    }
}

// ================= Web服务器 =================
AsyncWebServer server(80);

void setupWebServer() {
    // 主页
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta charset='UTF-8'>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<title>E3 Bluetooth Audio Gateway</title>";
        html += "<style>";
        html += "body{font-family:Arial,sans-serif;max-width:700px;margin:0 auto;padding:15px;background:#1a1a2e;color:#eee;}";
        html += "h1{color:#00d4ff;text-align:center;margin-bottom:20px;}";
        html += "h2{color:#00d4ff;font-size:16px;margin:15px 0 10px;}";
        html += ".card{background:#16213e;padding:15px;border-radius:10px;margin:15px 0;}";
        html += ".status{display:flex;justify-content:space-around;text-align:center;flex-wrap:wrap;}";
        html += ".status-item{background:#0f3460;padding:15px;border-radius:8px;min-width:100px;margin:5px;}";
        html += ".value{font-size:20px;font-weight:bold;color:#00d4ff;}";
        html += ".label{color:#888;font-size:12px;margin-top:5px;}";
        html += ".btn{background:#00d4ff;color:#1a1a2e;padding:12px 20px;border:none;border-radius:5px;";
        html += "cursor:pointer;font-size:14px;font-weight:bold;width:100%;margin:8px 0;}";
        html += ".btn:hover{background:#00a8cc;} .btn:disabled{background:#555;color:#888;cursor:not-allowed;}";
        html += ".btn-danger{background:#ff6b6b;}.btn-danger:hover{background:#ff4757;}";
        html += ".btn-success{background:#2ed573;}.btn-success:hover{background:#26de81;}";
        html += ".log{background:#000;padding:12px;border-radius:5px;height:180px;overflow-y:auto;font-family:monospace;font-size:12px;max-height:250px;}";
        html += ".log-line{padding:3px 0;border-bottom:1px solid #222;}";
        html += ".log-time{color:#666;}";
        html += ".log-bt{color:#00d4ff;}";
        html += ".log-sys{color:#2ed573;}";
        html += ".info-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}";
        html += ".info-item{background:#0f3460;padding:10px;border-radius:5px;}";
        html += ".info-label{color:#888;font-size:12px;}";
        html += ".info-value{color:#00d4ff;font-weight:bold;}";
        html += ".device-list{max-height:200px;overflow-y:auto;}";
        html += ".device-item{display:flex;justify-content:space-between;align-items:center;padding:10px;background:#0f3460;margin:8px 0;border-radius:5px;}";
        html += ".device-name{font-weight:bold;}";
        html += ".device-rssi{color:#888;font-size:12px;}";
        html += ".device-btn{padding:8px 15px;font-size:12px;}";
        html += ".connected-badge{background:#2ed573;color:#1a1a2e;padding:5px 10px;border-radius:5px;font-size:12px;}";
        html += "</style></head><body>";

        html += "<h1>E3 Bluetooth Audio Gateway</h1>";

        // 状态
        html += "<div class='card'>";
        html += "<h2>Status</h2>";
        html += "<div class='status'>";
        html += "<div class='status-item'><div class='value' id='btStatus'>Disconnected</div><div class='label'>Bluetooth</div></div>";
        html += "<div class='status-item'><div class='value'>AP</div><div class='label'>Mode</div></div>";
        html += "<div class='status-item'><div class='value'>v1.0.0</div><div class='label'>Firmware</div></div>";
        html += "</div></div>";

        // 操作
        html += "<div class='card'>";
        html += "<h2>Actions</h2>";
        html += "<button class='btn' id='scanBtn' onclick='scanBT()'>Scan Bluetooth Devices</button>";
        html += "<button class='btn btn-danger' onclick='disconnectBT()'>Disconnect</button>";
        html += "<button class='btn' onclick='clearLog()'>Clear Log</button>";
        html += "<button class='btn btn-danger' onclick='restartDevice()'>Restart Device</button>";
        html += "</div>";

        // 设备列表
        html += "<div class='card'>";
        html += "<h2>Bluetooth Devices</h2>";
        html += "<div class='device-list' id='deviceList'><p style='color:#888;'>Click 'Scan' to find devices</p></div>";
        html += "</div>";

        // 日志
        html += "<div class='card'>";
        html += "<h2>System Log</h2>";
        html += "<div class='log' id='logBox'></div>";
        html += "</div>";

        // 信息
        html += "<div class='card'>";
        html += "<h2>Device Info</h2>";
        html += "<div class='info-grid'>";
        html += "<div class='info-item'><div class='info-label'>BT MAC</div><div class='info-value' id='btMac'>-</div></div>";
        html += "<div class='info-item'><div class='info-label'>BT Name</div><div class='info-value'>E3-BT-Audio</div></div>";
        html += "<div class='info-item'><div class='info-label'>Flash</div><div class='info-value'>4MB</div></div>";
        html += "<div class='info-item'><div class='info-label'>RAM</div><div class='info-value'>320KB</div></div>";
        html += "</div></div>";

        html += "<script>";
        html += "function addLog(msg, type='sys'){";
        html += "var box=document.getElementById('logBox');";
        html += "var cls=type==='bt'?'log-bt':'log-sys';";
        html += "var time='<span class=\"log-time\">'+new Date().toLocaleTimeString()+'</span> ';";
        html += "box.innerHTML='<div class=\"log-line\">'+time+'<span class=\"'+cls+'\">'+msg+'</span></div>'+box.innerHTML;";
        html += "}";
        html += "function updateStatus(){";
        html += "fetch('/api/status').then(r=>r.json()).then(d=>{";
        html += "document.getElementById('btStatus').textContent=d.connected?'Connected':'Disconnected';";
        html += "document.getElementById('btStatus').style.color=d.connected?'#2ed573':'#ff6b6b';";
        html += "document.getElementById('btMac').textContent=d.btMac||'-';";
        html += "});}";
        html += "function loadDevices(){";
        html += "fetch('/api/devices').then(r=>r.json()).then(d=>{";
        html += "var html='';";
        html += "if(d.devices.length===0){html='<p style=\"color:#888;\">No devices found. Click Scan.</p>';}";
        html += "d.devices.forEach(dev=>{";
        html += "var btn=dev.connected?'<span class=\"connected-badge\">Connected</span>':'<button class=\"btn btn-success device-btn\" onclick=\"connectBT(\\''+dev.address+'\\')\">Connect</button>';";
        html += "html+='<div class=\"device-item\"><div><div class=\"device-name\">'+dev.name+'</div><div class=\"device-rssi\">'+dev.address+' | '+dev.rssi+' dBm</div></div>'+btn+'</div>';";
        html += "});";
        html += "document.getElementById('deviceList').innerHTML=html;";
        html += "});}";
        html += "function scanBT(){";
        html += "document.getElementById('scanBtn').disabled=true;";
        html += "addLog('[BT] Scanning for devices...','bt');";
        html += "fetch('/api/scan').then(r=>r.text()).then(d=>{";
        html += "addLog('[BT] Scan complete','bt');";
        html += "document.getElementById('scanBtn').disabled=false;";
        html += "loadDevices();";
        html += "});}";
        html += "function connectBT(addr){";
        html += "addLog('[BT] Connecting to '+addr+'...','bt');";
        html += "fetch('/api/connect?addr='+addr).then(r=>r.text()).then(d=>addLog(d,'bt'));";
        html += "}";
        html += "function disconnectBT(){";
        html += "addLog('[BT] Disconnecting...','bt');";
        html += "fetch('/api/disconnect').then(r=>r.text()).then(d=>addLog(d,'bt'));";
        html += "}";
        html += "function clearLog(){document.getElementById('logBox').innerHTML='';}";
        html += "function restartDevice(){if(confirm('Restart device?'))fetch('/api/restart');}";
        html += "addLog('[SYS] Web interface ready','sys');";
        html += "updateStatus();loadDevices();";
        html += "setInterval(updateStatus,3000);";
        html += "setInterval(loadDevices,5000);";
        html += "</script></body></html>";
        request->send(200, "text/html; charset=utf-8", html);
    });

    // API: 状态
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{\"connected\":" + String(btConnected ? "true" : "false") + ",\"btMac\":\"" + btMacAddress + "\"}";
        request->send(200, "application/json", json);
    });

    // API: 设备列表
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{\"devices\":[";
        for (size_t i = 0; i < btDevices.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"name\":\"" + btDevices[i].name + "\",\"address\":\"" + btDevices[i].address + "\",\"rssi\":" + btDevices[i].rssi + ",\"connected\":" + (btDevices[i].connected ? "true" : "false") + "}";
        }
        json += "]}";
        request->send(200, "application/json", json);
    });

    // API: 扫描
    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        btDevices.clear();
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        request->send(200, "text/plain", "[BT] Scan started");
    });

    // API: 连接
    server.on("/api/connect", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("addr")) {
            String addr = request->getParam("addr")->value();
            int bda[6];
            sscanf(addr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]);
            esp_bd_addr_t bda_addr;
            for (int i = 0; i < 6; i++) bda_addr[i] = (uint8_t)bda[i];
            esp_a2d_sink_connect(bda_addr);
            request->send(200, "text/plain", "[BT] Connecting...");
        } else {
            request->send(400, "text/plain", "[ERR] Missing address");
        }
    });

    // API: 断开
    server.on("/api/disconnect", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (btConnected && btMacAddress.length() > 0) {
            int bda[6];
            sscanf(btMacAddress.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]);
            esp_bd_addr_t bda_addr;
            for (int i = 0; i < 6; i++) bda_addr[i] = (uint8_t)bda[i];
            esp_a2d_sink_disconnect(bda_addr);
            request->send(200, "text/plain", "[BT] Disconnecting...");
        } else {
            request->send(200, "text/plain", "[BT] Not connected");
        }
    });

    // API: 重启
    server.on("/api/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Restarting...");
        delay(100);
        ESP.restart();
    });

    server.begin();
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("============================================");
    Serial.println("  E3 Firmware v1.0.0");
    Serial.println("  ESP32 Bluetooth Audio Gateway + WiFi OTA");
    Serial.println("============================================");

    pinMode(LED_STATUS_PIN, OUTPUT);
    digitalWrite(LED_STATUS_PIN, LOW);

    // 初始化蓝牙
    Serial.println("[BT] Initializing...");
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    esp_bt_dev_set_device_name("E3-BT-Audio");

    // 注册回调
    esp_bt_gap_register_callback(bt_gap_callback);
    esp_a2d_register_callback(a2d_callback);
    esp_a2d_sink_init();

    // 获取本地MAC
    const uint8_t* local_bda = esp_bt_dev_get_address();
    char bda_str[18];
    snprintf(bda_str, sizeof(bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             local_bda[0], local_bda[1], local_bda[2],
             local_bda[3], local_bda[4], local_bda[5]);
    Serial.printf("[BT] MAC: %s\n", bda_str);

    // WiFi AP
    Serial.println("[WiFi] Starting AP...");
    String ssid = "E3_Test";
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid.c_str(), "12345678");
    Serial.printf("[WiFi] SSID: %s | IP: %s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());

    // Web服务器
    Serial.println("[Web] Starting server...");
    setupWebServer();

    Serial.println("============================================");
    Serial.println("[OK] System Ready!");
    Serial.println("============================================");
}

void loop() {
    handleBluetoothState();
    delay(10);
}
