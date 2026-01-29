#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ThingSpeak.h>
#include <ModbusMaster.h>
#include <DHT.h>
#include <BH1750.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>

// === WiFi 配置 ===
// 默认 WiFi（如果自动配置失败则使用）
const char* default_ssid     = "iPhone 11 CSC";
const char* default_password = "602602602";

// WiFi 配置结构体
struct WiFiConfig {
  String ssid;
  String password;
  bool valid;
};

// 最多保存 5 组 WiFi 配置
#define MAX_WIFI_CONFIGS 5
WiFiConfig wifiConfigs[MAX_WIFI_CONFIGS];
int wifiConfigCount = 0;

// Web 配置服务器
WebServer server(80);
Preferences preferences;
bool configServerRunning = false; // 配置热点/网页是否正在运行

// === ThingSpeak 配置 ===
unsigned long channelNumber = 1880892UL;
const char* writeAPIKey     = "0UWC02XHIMUUKHGK";

// === RS485 配置（统一总线） ===
#define BAUDRATE         9600
#define RS485_RX_PIN     16
#define RS485_TX_PIN     17
#define RS485_RE_DE_PIN  4  // 统一的RE/DE控制引脚

// RS485传感器从站ID
#define SLAVE_ID_NH4     1   // 氨氮传感器
#define SLAVE_ID_PH      2   // pH传感器
#define SLAVE_ID_TURB    3   // 浊度传感器

// === DHT11 ===
#define DHT_PIN          5
#define DHT_TYPE         DHT11

// === BH1750 ===
BH1750 lightMeter;
bool bh1750Ready = false;

// === DS18B20 水温 ===
#define ONE_WIRE_BUS     18
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// === TDS 水质传感器（模拟量） ===
#define TDS_PIN          34  // ADC1_CH6，只能输入
#define TDS_MAX_VALUE    900  // TDS最大值
#define ADC_RESOLUTION   4095  // ESP32 ADC 12位分辨率

// === 采样/上传间隔 ===
static const unsigned long MEASURE_INTERVAL_MS = 300000UL; // 5分钟

// 掉线后多久自动开启配置热点（避免长时间不可配置）
static const unsigned long CONFIG_AP_TRIGGER_MS = 30000UL; // 30秒

DHT dht(DHT_PIN, DHT_TYPE);
ModbusMaster nodeNH4;   // 氨氮传感器
ModbusMaster nodePH;    // pH传感器
ModbusMaster nodeTurb;  // 浊度传感器
WiFiClient client;

// RS485方向控制函数（统一使用同一个RE/DE引脚）
void preTransmission() { 
  digitalWrite(RS485_RE_DE_PIN, HIGH); 
  delayMicroseconds(50); 
}
void postTransmission() { 
  delayMicroseconds(50); 
  digitalWrite(RS485_RE_DE_PIN, LOW); 
}

// 安全讀取 Modbus
uint8_t safeRead(ModbusMaster &node, uint16_t addr, uint16_t qty, unsigned long timeout_ms = 800) {
  unsigned long start = millis();
  uint8_t res = node.readHoldingRegisters(addr, qty);
  while (res == node.ku8MBResponseTimedOut && millis() - start < timeout_ms) {
    delay(10);
    esp_task_wdt_reset();
    yield();
    res = node.readHoldingRegisters(addr, qty);
  }
  return res;
}

// 从 Preferences 读取保存的 WiFi 配置
void loadWiFiConfigs() {
  preferences.begin("wifi", true);  // 只读模式
  
  wifiConfigCount = preferences.getInt("count", 0);
  if (wifiConfigCount > MAX_WIFI_CONFIGS) {
    wifiConfigCount = MAX_WIFI_CONFIGS;
  }
  
  for (int i = 0; i < wifiConfigCount; i++) {
    String key_ssid = "ssid" + String(i);
    String key_pass = "pass" + String(i);
    wifiConfigs[i].ssid = preferences.getString(key_ssid.c_str(), "");
    wifiConfigs[i].password = preferences.getString(key_pass.c_str(), "");
    wifiConfigs[i].valid = (wifiConfigs[i].ssid.length() > 0);
  }
  
  preferences.end();
  
  Serial.printf("讀取到 %d 組 WiFi 配置:\n", wifiConfigCount);
  for (int i = 0; i < wifiConfigCount; i++) {
    if (wifiConfigs[i].valid) {
      Serial.printf("  [%d] %s\n", i + 1, wifiConfigs[i].ssid.c_str());
    }
  }
  
  if (wifiConfigCount == 0) {
    Serial.println("未找到保存的 WiFi 配置");
  }
}

// 保存所有 WiFi 配置到 Preferences
void saveWiFiConfigs() {
  preferences.begin("wifi", false);  // 读写模式
  
  preferences.putInt("count", wifiConfigCount);
  
  for (int i = 0; i < wifiConfigCount; i++) {
    String key_ssid = "ssid" + String(i);
    String key_pass = "pass" + String(i);
    preferences.putString(key_ssid.c_str(), wifiConfigs[i].ssid);
    preferences.putString(key_pass.c_str(), wifiConfigs[i].password);
  }
  
  // 清除多余的配置
  for (int i = wifiConfigCount; i < MAX_WIFI_CONFIGS; i++) {
    String key_ssid = "ssid" + String(i);
    String key_pass = "pass" + String(i);
    preferences.remove(key_ssid.c_str());
    preferences.remove(key_pass.c_str());
  }
  
  preferences.end();
  Serial.printf("已保存 %d 組 WiFi 配置\n", wifiConfigCount);
}

// 添加或更新 WiFi 配置
bool addWiFiConfig(String ssid, String password) {
  if (ssid.length() == 0 || password.length() == 0) {
    return false;
  }
  
  // 检查是否已存在（更新）
  for (int i = 0; i < wifiConfigCount; i++) {
    if (wifiConfigs[i].ssid == ssid) {
      wifiConfigs[i].password = password;
      wifiConfigs[i].valid = true;
      saveWiFiConfigs();
      Serial.printf("更新 WiFi 配置: %s\n", ssid.c_str());
      return true;
    }
  }
  
  // 添加新配置
  if (wifiConfigCount < MAX_WIFI_CONFIGS) {
    wifiConfigs[wifiConfigCount].ssid = ssid;
    wifiConfigs[wifiConfigCount].password = password;
    wifiConfigs[wifiConfigCount].valid = true;
    wifiConfigCount++;
    saveWiFiConfigs();
    Serial.printf("添加 WiFi 配置: %s\n", ssid.c_str());
    return true;
  }
  
  Serial.println("WiFi 配置已達上限（5組）");
  return false;
}

// 删除 WiFi 配置
bool deleteWiFiConfig(int index) {
  if (index < 0 || index >= wifiConfigCount) {
    return false;
  }
  
  String deletedSSID = wifiConfigs[index].ssid;
  
  // 向前移动数组
  for (int i = index; i < wifiConfigCount - 1; i++) {
    wifiConfigs[i] = wifiConfigs[i + 1];
  }
  
  wifiConfigCount--;
  saveWiFiConfigs();
  Serial.printf("刪除 WiFi 配置: %s\n", deletedSSID.c_str());
  return true;
}

// 尝试连接 WiFi
bool connectWiFi(String ssid, String password, unsigned long timeout_ms = 30000) {
  Serial.printf("正在連接 WiFi: %s\n", ssid.c_str());
  // 注意：如果正在运行配置热点（AP+STA），不要把模式切走或做“全断开”，否则会导致手机找不到热点
  if (!configServerRunning) {
    WiFi.disconnect(true);
    delay(200);
  } else {
    WiFi.disconnect(false);
    delay(50);
  }
  esp_task_wdt_reset();
  
  WiFi.begin(ssid.c_str(), password.c_str());
  
  unsigned long startTime = millis();
  int dotCount = 0;
  
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeout_ms) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();
    dotCount++;
    if (dotCount % 20 == 0) {
      Serial.println("");
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi 連接成功！");
    Serial.printf("IP 地址: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    // 启用 ESP32 的自动重连功能（作为备用）
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);  // 允许保存连接信息
    return true;
  }
  
  Serial.println("");
  Serial.println("WiFi 連接失敗");
  return false;
}

// WiFi 网络信息（用于排序）
struct WiFiNetwork {
  String ssid;
  int rssi;
  int configIndex;  // 在wifiConfigs数组中的索引
};

// 按信号强度自动选择并连接最优 WiFi
bool connectBestWiFi() {
  Serial.println("正在掃描 WiFi 網絡...");
  
  // 如果配置热点在运行，保持 AP+STA，不要切换为 STA（否则 AP 会消失）
  if (configServerRunning) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  WiFi.disconnect(false);
  delay(50);
  esp_task_wdt_reset();
  
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("未找到任何 WiFi 網絡");
    return false;
  }
  
  Serial.printf("找到 %d 個 WiFi 網絡\n", n);
  
  // 创建匹配的网络列表
  WiFiNetwork matchedNetworks[MAX_WIFI_CONFIGS];
  int matchedCount = 0;
  
  // 匹配已保存的配置
  for (int i = 0; i < n && matchedCount < MAX_WIFI_CONFIGS; i++) {
    String scannedSSID = WiFi.SSID(i);
    int scannedRSSI = WiFi.RSSI(i);
    
    // 查找是否在已保存的配置中
    for (int j = 0; j < wifiConfigCount; j++) {
      if (wifiConfigs[j].valid && wifiConfigs[j].ssid == scannedSSID) {
        matchedNetworks[matchedCount].ssid = scannedSSID;
        matchedNetworks[matchedCount].rssi = scannedRSSI;
        matchedNetworks[matchedCount].configIndex = j;
        matchedCount++;
        Serial.printf("  ✓ 找到已保存的網絡: %s (RSSI: %d dBm)\n", scannedSSID.c_str(), scannedRSSI);
        break;
      }
    }
  }
  
  if (matchedCount == 0) {
    Serial.println("未找到已保存的 WiFi 網絡");
    return false;
  }
  
  // 按信号强度排序（从强到弱）
  for (int i = 0; i < matchedCount - 1; i++) {
    for (int j = i + 1; j < matchedCount; j++) {
      if (matchedNetworks[i].rssi < matchedNetworks[j].rssi) {
        WiFiNetwork temp = matchedNetworks[i];
        matchedNetworks[i] = matchedNetworks[j];
        matchedNetworks[j] = temp;
      }
    }
  }
  
  // 按信号强度从强到弱尝试连接
  Serial.println("\n按信號強度嘗試連接:");
  for (int i = 0; i < matchedCount; i++) {
    int configIdx = matchedNetworks[i].configIndex;
    Serial.printf("[%d/%d] 嘗試連接: %s (RSSI: %d dBm)...\n", 
                  i + 1, matchedCount, matchedNetworks[i].ssid.c_str(), matchedNetworks[i].rssi);
    
    if (connectWiFi(wifiConfigs[configIdx].ssid, wifiConfigs[configIdx].password, 15000)) {
      Serial.printf("✓ 成功連接到: %s (信號強度: %d dBm)\n", 
                    matchedNetworks[i].ssid.c_str(), matchedNetworks[i].rssi);
      return true;
    } else {
      Serial.printf("✗ 連接失敗: %s\n", matchedNetworks[i].ssid.c_str());
    }
  }
  
  Serial.println("所有已保存的 WiFi 網絡連接失敗");
  return false;
}

// 存储扫描到的 WiFi 网络列表
String wifiListHTML = "";

// 扫描 WiFi 网络并生成列表 HTML
void scanWiFiNetworks() {
  Serial.println("正在掃描 WiFi 網絡...");
  // 如果配置热点在运行，保持 AP+STA，避免扫描时把 AP 关掉
  if (configServerRunning) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  WiFi.disconnect(false);
  delay(50);
  
  int n = WiFi.scanNetworks();
  wifiListHTML = "";
  
  if (n == 0) {
    wifiListHTML = "<option value=''>未找到 WiFi 網絡</option>";
  } else {
    for (int i = 0; i < n; i++) {
      wifiListHTML += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }
  Serial.printf("找到 %d 個 WiFi 網絡\n", n);
}

// Web 配置页面 HTML
String getConfigPageHTML() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>WiFi 配置</title>";
  html += "<style>body{font-family:Arial;max-width:600px;margin:20px auto;padding:20px;background:#f5f5f5;}";
  html += ".container{background:white;padding:30px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
  html += "h2{color:#333;margin-top:0;}";
  html += "input,select{width:100%;padding:12px;margin:10px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:5px;font-size:14px;}";
  html += "button{width:100%;padding:12px;background:#4CAF50;color:white;border:none;cursor:pointer;font-size:16px;border-radius:5px;margin:5px 0;}";
  html += "button:hover{background:#45a049;}";
  html += ".btn-danger{background:#f44336;}";
  html += ".btn-danger:hover{background:#da190b;}";
  html += ".btn-info{background:#2196F3;}";
  html += ".btn-info:hover{background:#0b7dda;}";
  html += ".btn-small{width:auto;padding:8px 15px;font-size:12px;margin:0 5px;}";
  html += ".info{background:#e3f2fd;padding:15px;border-radius:5px;margin:15px 0;border-left:4px solid #2196F3;}";
  html += ".saved-list{background:#f9f9f9;padding:15px;border-radius:5px;margin:15px 0;}";
  html += ".saved-item{background:white;padding:10px;margin:5px 0;border-radius:5px;border:1px solid #ddd;display:flex;justify-content:space-between;align-items:center;}";
  html += ".saved-item strong{flex:1;}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h2>🌐 ESP32 WiFi 配置</h2>";
  
  // 显示已保存的配置列表
  if (wifiConfigCount > 0) {
    html += "<div class='saved-list'>";
    html += "<h3>已保存的 WiFi 配置 (" + String(wifiConfigCount) + "/" + String(MAX_WIFI_CONFIGS) + "):</h3>";
    for (int i = 0; i < wifiConfigCount; i++) {
      if (wifiConfigs[i].valid) {
        html += "<div class='saved-item'>";
        html += "<strong>" + String(i + 1) + ". " + wifiConfigs[i].ssid + "</strong>";
        html += "<a href='/delete?index=" + String(i) + "' onclick='return confirm(\"確定要刪除這個配置嗎？\")'>";
        html += "<button class='btn-danger btn-small'>刪除</button></a>";
        html += "</div>";
      }
    }
    html += "</div>";
  } else {
    html += "<div class='info'>";
    html += "尚未保存任何 WiFi 配置";
    html += "</div>";
  }
  
  html += "<div class='info'>";
  html += "<strong>說明:</strong><br>";
  html += "• 系統會自動按信號強度選擇最優的 WiFi 連接<br>";
  html += "• 最多可保存 " + String(MAX_WIFI_CONFIGS) + " 組 WiFi 配置<br>";
  html += "• 如果已存在相同 SSID，將更新密碼";
  html += "</div>";
  
  html += "<hr style='margin:20px 0;'>";
  html += "<h3>添加新的 WiFi 配置</h3>";
  html += "<form action='/save' method='POST'>";
  html += "<label><strong>選擇 WiFi 網絡:</strong></label>";
  html += "<select name='ssid' id='ssid'>";
  html += wifiListHTML;
  html += "</select>";
  html += "<label><strong>或手動輸入 SSID:</strong></label>";
  html += "<input type='text' name='ssid_manual' placeholder='WiFi 名稱'>";
  html += "<label><strong>密碼:</strong></label>";
  html += "<input type='password' name='password' placeholder='WiFi 密碼' required>";
  html += "<button type='submit'>💾 保存配置</button>";
  html += "</form>";
  
  html += "<hr style='margin:20px 0;'>";
  html += "<a href='/connect'><button class='btn-info'>🔌 立即嘗試連接</button></a>";
  html += "<a href='/status'><button class='btn-info'>ℹ️ 查看系統狀態</button></a>";
  html += "<a href='/rescan'><button class='btn-info'>🔄 重新掃描 WiFi</button></a>";
  if (wifiConfigCount > 0) {
    html += "<a href='/clear'><button class='btn-danger'>🗑️ 清除所有配置</button></a>";
  }
  
  html += "</div></body></html>";
  return html;
}

// 处理根路径
void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", getConfigPageHTML());
}

// 处理 Captive Portal 检测（iOS/Android 自动弹出配置页面）
void handleCaptivePortal() {
  // iOS 和 Android 会请求这些路径来检测 Captive Portal
  // 返回配置页面，让系统自动弹出
  server.send(200, "text/html; charset=UTF-8", getConfigPageHTML());
}

// 处理所有未定义的路径（重定向到配置页面）
void handleNotFound() {
  // 如果是配置模式，重定向到配置页面
  if (configServerRunning) {
    // 检查是否是 Captive Portal 检测请求
    String host = server.hostHeader();
    if (host.indexOf("captive.apple.com") >= 0 || 
        host.indexOf("connectivitycheck.android.com") >= 0 ||
        host.indexOf("msftconnecttest.com") >= 0 ||
        host.indexOf("192.168.4.1") >= 0 ||
        host.length() == 0) {
      // 返回配置页面，触发 Captive Portal 弹出
      handleCaptivePortal();
    } else {
      // 其他请求重定向到配置页面
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "重定向到配置页面...");
    }
  } else {
    server.send(404, "text/plain", "404: Not Found");
  }
}

// 处理保存配置
void handleSave() {
  String ssid = server.arg("ssid");
  String ssid_manual = server.arg("ssid_manual");
  String password = server.arg("password");
  
  // 如果手动输入了 SSID，优先使用手动输入的
  if (ssid_manual.length() > 0) {
    ssid = ssid_manual;
  }
  
  if (ssid.length() > 0 && password.length() > 0) {
    bool success = addWiFiConfig(ssid, password);
    
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta http-equiv='refresh' content='2;url=/'>";
    html += "<title>配置保存</title>";
    html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#f5f5f5;}";
    html += ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:0 auto;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
    html += ".success{color:#4CAF50;font-size:24px;margin:20px 0;}";
    html += ".error{color:#f44336;font-size:24px;margin:20px 0;}";
    html += "</style></head><body>";
    html += "<div class='container'>";
    if (success) {
      html += "<div class='success'>✓ 配置已保存！</div>";
      html += "<p>WiFi: <strong>" + ssid + "</strong></p>";
      html += "<p>頁面將在 2 秒後自動刷新...</p>";
    } else {
      html += "<div class='error'>✗ 保存失敗</div>";
      html += "<p>可能原因：配置已達上限（" + String(MAX_WIFI_CONFIGS) + "組）</p>";
    }
    html += "<a href='/'>立即返回</a>";
    html += "</div></body></html>";
    server.send(200, "text/html; charset=UTF-8", html);
  } else {
    server.send(400, "text/plain", "錯誤：SSID 和密碼不能為空");
  }
}

// 处理删除配置
void handleDelete() {
  String indexStr = server.arg("index");
  int index = indexStr.toInt();
  
  if (deleteWiFiConfig(index)) {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "刪除成功，正在返回...");
  } else {
    server.send(400, "text/plain", "錯誤：無效的索引");
  }
}

// 处理立即连接
void handleConnect() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='5;url=/'>";
  html += "<title>正在連接</title>";
  html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#f5f5f5;}";
  html += ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:0 auto;}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h2>正在嘗試連接...</h2>";
  html += "<p>系統將按信號強度自動選擇最優的 WiFi 連接</p>";
  html += "<p>頁面將在 5 秒後自動刷新</p>";
  html += "</div></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
  
  // 尝试连接
  delay(1000);
  if (connectBestWiFi()) {
    Serial.println("配置模式：WiFi 連接成功，將繼續正常運行");
  } else {
    Serial.println("配置模式：所有 WiFi 連接失敗");
  }
}

// 处理清除配置
void handleClear() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
  
  wifiConfigCount = 0;
  for (int i = 0; i < MAX_WIFI_CONFIGS; i++) {
    wifiConfigs[i].valid = false;
    wifiConfigs[i].ssid = "";
    wifiConfigs[i].password = "";
  }
  
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='2;url=/'>";
  html += "<title>配置已清除</title>";
  html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#f5f5f5;}";
  html += ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:0 auto;}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h2>✓ 所有配置已清除</h2>";
  html += "<p>正在返回配置頁面...</p>";
  html += "</div></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
  
  Serial.println("所有 WiFi 配置已清除");
}

// 处理系统状态
void handleStatus() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<title>系統狀態</title>";
  html += "<style>body{font-family:Arial;max-width:600px;margin:20px auto;padding:20px;background:#f5f5f5;}";
  html += ".container{background:white;padding:30px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
  html += "table{width:100%;border-collapse:collapse;margin:20px 0;}";
  html += "td{padding:10px;border-bottom:1px solid #ddd;}";
  html += "td:first-child{font-weight:bold;width:40%;}";
  html += ".status-connected{color:#4CAF50;}";
  html += ".status-disconnected{color:#f44336;}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h2>📊 系統狀態</h2>";
  html += "<table>";
  
  html += "<tr><td>WiFi 狀態</td><td>";
  if (WiFi.status() == WL_CONNECTED) {
    html += "<span class='status-connected'>✓ 已連接</span>";
  } else {
    html += "<span class='status-disconnected'>✗ 未連接</span>";
  }
  html += "</td></tr>";
  
  if (WiFi.status() == WL_CONNECTED) {
    html += "<tr><td>當前 SSID</td><td>" + WiFi.SSID() + "</td></tr>";
    html += "<tr><td>IP 地址</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    html += "<tr><td>信號強度</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
    html += "<tr><td>MAC 地址</td><td>" + WiFi.macAddress() + "</td></tr>";
  }
  
  html += "<tr><td>已保存配置數</td><td>" + String(wifiConfigCount) + " / " + String(MAX_WIFI_CONFIGS) + "</td></tr>";
  html += "<tr><td>運行時間</td><td>" + String(millis() / 1000) + " 秒</td></tr>";
  html += "<tr><td>可用內存</td><td>" + String(ESP.getFreeHeap()) + " 字節</td></tr>";
  html += "<tr><td>芯片型號</td><td>ESP32</td></tr>";
  
  html += "</table>";
  
  if (wifiConfigCount > 0) {
    html += "<h3>已保存的 WiFi 配置:</h3>";
    html += "<ul>";
    for (int i = 0; i < wifiConfigCount; i++) {
      if (wifiConfigs[i].valid) {
        html += "<li>" + wifiConfigs[i].ssid;
        if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == wifiConfigs[i].ssid) {
          html += " <strong>(當前連接)</strong>";
        }
        html += "</li>";
      }
    }
    html += "</ul>";
  }
  
  html += "<a href='/'><button style='width:100%;padding:12px;background:#2196F3;color:white;border:none;cursor:pointer;border-radius:5px;'>返回配置頁面</button></a>";
  html += "</div></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

// 处理重新扫描
void handleRescan() {
  scanWiFiNetworks();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "重新掃描完成，正在返回...");
}

// 启动配置模式（AP 模式）
void startConfigMode() {
  Serial.println("\n=== 啟動 WiFi 配置模式 ===");
  
  // 标记先置为 true，确保扫描过程中也不会把 AP 切没
  configServerRunning = true;

  // 在切换到 AP 模式前先扫描 WiFi
  scanWiFiNetworks();
  esp_task_wdt_reset();
  
  Serial.println("請連接 WiFi 熱點: WaterQualityMonitor");
  Serial.println("然後在瀏覽器中訪問: http://192.168.4.1");

  // AP+STA 并行：既可开热点配置，也可后台继续自动重连
  WiFi.mode(WIFI_AP_STA);
  // 显式设置不隐藏、最大连接数；避免某些手机扫描不到
  WiFi.softAP("WaterQualityMonitor", "", 1 /*channel*/, 0 /*hidden*/, 4 /*max_conn*/);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("配置服務器 IP: %s\n", IP.toString().c_str());
  
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/delete", handleDelete);
  server.on("/connect", handleConnect);
  server.on("/clear", handleClear);
  server.on("/status", handleStatus);
  server.on("/rescan", handleRescan);
  // Captive Portal 检测路径（iOS/Android 自动检测）
  server.on("/generate_204", handleCaptivePortal);
  server.on("/hotspot-detect.html", handleCaptivePortal);
  server.on("/kindle-wifi/wifiredirect.html", handleCaptivePortal);
  server.on("/success.txt", handleCaptivePortal);
  server.on("/connecttest.txt", handleCaptivePortal);
  server.on("/fwlink", handleCaptivePortal);
  server.onNotFound(handleNotFound);  // 捕获所有未定义的路径
  server.begin();
  Serial.println("配置服務器已啟動");
  Serial.println("可用功能:");
  Serial.println("  - http://192.168.4.1/        : WiFi 配置頁面");
  Serial.println("  - http://192.168.4.1/status  : 查看系統狀態");
  Serial.println("  - http://192.168.4.1/connect : 立即嘗試連接");
  Serial.println("  - http://192.168.4.1/rescan  : 重新掃描 WiFi");
  Serial.println("  - http://192.168.4.1/clear   : 清除所有配置");
  Serial.println("提示: 連接熱點後，在瀏覽器中輸入任何網址都會自動跳轉到配置頁面");
}

void stopConfigModeIfRunning() {
  if (!configServerRunning) return;
  server.stop();
  WiFi.softAPdisconnect(true);
  configServerRunning = false;
  // 回到 STA 模式即可（保持自动重连逻辑）
  WiFi.mode(WIFI_STA);
  Serial.println("已退出配置模式（AP 已关闭）");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32 多感測器系統啟動... Channel ID: 1880892");

  // 先初始化看门狗（60秒超时，给初始化留足够时间）
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(NULL);

  // 初始化 Preferences
  preferences.begin("wifi", false);
  
  // 加载保存的 WiFi 配置
  loadWiFiConfigs();
  
  // WiFi 自动配置
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  
  bool wifiConnected = false;
  
  // 1. 先尝试按信号强度自动选择最优 WiFi 连接
  if (wifiConfigCount > 0) {
    Serial.println("嘗試按信號強度自動選擇最優 WiFi 連接...");
    wifiConnected = connectBestWiFi();
  }
  
  // 2. 如果自动选择失败，尝试默认配置
  if (!wifiConnected && default_ssid != NULL) {
    Serial.println("嘗試連接默認 WiFi 配置...");
    wifiConnected = connectWiFi(String(default_ssid), String(default_password), 20000);
    if (wifiConnected) {
      // 如果默认配置连接成功，保存它
      addWiFiConfig(String(default_ssid), String(default_password));
    }
  }
  
  // 3. 如果都连接失败，启动配置模式
  if (!wifiConnected) {
    Serial.println("\n所有 WiFi 配置連接失敗，啟動配置模式...");
    startConfigMode();
    
    // 在配置模式下运行一段时间，等待用户配置
    unsigned long configStartTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - configStartTime < 300000) {  // 5分钟超时
      server.handleClient();
      esp_task_wdt_reset();
      delay(10);
      
      // 每30秒检查一次是否已连接
      if ((millis() - configStartTime) % 30000 < 100) {
        if (wifiConfigCount > 0) {
          if (connectBestWiFi()) {
            wifiConnected = true;
            break;
          }
        }
      }
    }
    
    if (!wifiConnected) {
      Serial.println("配置模式超時，系統將繼續運行但無法上傳數據");
    }
  }

  // 連上 WiFi 後短暫延遲（讓網絡穩定）
  delay(500);
  esp_task_wdt_reset();

  // 初始化非 RS485 部分
  Serial.println("初始化 ThingSpeak...");
  ThingSpeak.begin(client);
  delay(200);
  esp_task_wdt_reset();

  Serial.println("初始化 DHT11...");
  dht.begin();
  delay(500);  // DHT11 需要一点时间稳定
  esp_task_wdt_reset();

  Serial.println("初始化 DS18B20...");
  sensors.begin();
  delay(200);
  esp_task_wdt_reset();

  Serial.println("初始化 BH1750...");
  // 确保 I2C 总线正确初始化（如果之前已经初始化过，这里会重置）
  Wire.end();  // 先结束之前的I2C连接
  delay(50);
  Wire.begin();  // 重新初始化 I2C
  delay(200);  // 给 I2C 总线足够的稳定时间
  esp_task_wdt_reset();
  
  // 尝试初始化 BH1750，最多重试3次
  bh1750Ready = false;
  for (int retry = 0; retry < 3; retry++) {
    if (retry > 0) {
      Serial.printf("BH1750 初始化重試 %d/3...\n", retry);
      delay(500);
      Wire.end();
      delay(50);
      Wire.begin();  // 重新初始化 I2C
      delay(200);
    }
    bh1750Ready = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    if (bh1750Ready) {
      Serial.println("BH1750 初始化成功");
      break;
    } else {
      Serial.printf("BH1750 初始化失敗 (嘗試 %d/3)\n", retry + 1);
    }
    esp_task_wdt_reset();
  }
  
  if (!bh1750Ready) {
    Serial.println("BH1750 初始化失敗（稍後將自動重試）");
    Serial.println("提示: 請檢查 I2C 接線 (SDA=21, SCL=22) 和供電");
  }
  delay(200);
  esp_task_wdt_reset();

  // TDS 传感器初始化（模拟量）
  Serial.println("初始化 TDS 传感器...");
  pinMode(TDS_PIN, INPUT);
  // ESP32 ADC 配置：12位分辨率，11dB衰减（0-3.3V）
  analogSetAttenuation(ADC_11db);
  analogSetWidth(12);
  delay(200);
  esp_task_wdt_reset();

  // RS485 铵离子初始化
  // RS485 统一总线初始化（氨氮、pH、浊度共用）
  Serial.println("初始化 RS485 总线（氨氮、pH、浊度）...");
  pinMode(RS485_RE_DE_PIN, OUTPUT);
  digitalWrite(RS485_RE_DE_PIN, LOW);
  Serial2.begin(BAUDRATE, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  
  // 初始化氨氮传感器（Slave ID 1）
  nodeNH4.begin(SLAVE_ID_NH4, Serial2);
  nodeNH4.preTransmission(preTransmission);
  nodeNH4.postTransmission(postTransmission);
  
  // 初始化pH传感器（Slave ID 2）
  nodePH.begin(SLAVE_ID_PH, Serial2);
  nodePH.preTransmission(preTransmission);
  nodePH.postTransmission(postTransmission);
  
  // 初始化浊度传感器（Slave ID 3）
  nodeTurb.begin(SLAVE_ID_TURB, Serial2);
  nodeTurb.preTransmission(preTransmission);
  nodeTurb.postTransmission(postTransmission);
  
  delay(1000);  // RS485 需要一点时间稳定
  esp_task_wdt_reset();

  // 重新设置看门狗为正常运行时的时间（30秒）
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  Serial.print("Free Heap: "); Serial.println(ESP.getFreeHeap());
  Serial.println("所有初始化完成，開始正常循環...");
}

void loop() {
  esp_task_wdt_reset();

  // 智能 WiFi 自动重连和连接质量监控机制
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  static unsigned long lastConnectionQualityCheck = 0;
  static int reconnectAttempts = 0;
  static bool wasConnected = false;
  static bool configModeTriggered = false;
  static int lastRSSI = 0;
  static unsigned long lastConnectionTime = 0;
  
  unsigned long now = millis();
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  static unsigned long disconnectedSince = 0;
  
  // 检测连接状态变化
  if (isConnected && !wasConnected) {
    Serial.println("\n✓ WiFi 已重新連接！");
    Serial.printf("IP 地址: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    reconnectAttempts = 0;  // 重置重连计数
    wasConnected = true;
    configModeTriggered = false;  // 重置配置模式标志
    lastConnectionTime = now;
    lastRSSI = WiFi.RSSI();
  } else if (!isConnected && wasConnected) {
    Serial.println("\n✗ WiFi 連接已斷開");
    wasConnected = false;
    lastReconnectAttempt = 0;  // 立即尝试重连
    reconnectAttempts = 0;  // 重置计数，重新开始
  }

  // 记录掉线开始时间；掉线持续一段时间后，自动开启配置热点（便于手机配置/检查）
  if (!isConnected) {
    if (disconnectedSince == 0) disconnectedSince = now;
    if (!configServerRunning && (now - disconnectedSince) >= CONFIG_AP_TRIGGER_MS) {
      Serial.println("\n⚠️ WiFi 已掉線超過 30 秒，自動開啟配置熱點 WaterQualityMonitor");
      startConfigMode();
      configModeTriggered = true;
    }
  } else {
    disconnectedSince = 0;
  }
  
  // 如果已连接，定期检查连接质量（每30秒）
  if (isConnected) {
    if (now - lastConnectionQualityCheck >= 30000) {
      lastConnectionQualityCheck = now;
      int currentRSSI = WiFi.RSSI();
      
      // 检查连接质量
      if (currentRSSI < -85) {
        Serial.printf("⚠️ WiFi 信號較弱: %d dBm，考慮切換到更好的網絡\n", currentRSSI);
      }
      
      // 如果信号明显变弱（下降超过15dB），尝试切换到更好的WiFi
      if (lastRSSI != 0 && (currentRSSI - lastRSSI) < -15 && wifiConfigCount > 1) {
        Serial.println("信號明顯變弱，嘗試切換到更好的 WiFi 網絡...");
        // 不立即切换，只是记录，让用户知道
        lastRSSI = currentRSSI;
      } else {
        lastRSSI = currentRSSI;
      }
      
      // 验证连接是否真的有效（ping网关或检查IP）
      if (WiFi.localIP()[0] == 0) {
        Serial.println("⚠️ 檢測到 IP 地址異常，重新連接...");
        WiFi.disconnect();
        delay(500);
        wasConnected = false;
        lastReconnectAttempt = 0;
      }
    }
  }
  
  // 如果未连接，尝试重连
  if (!isConnected) {
    // 如果重连失败超过10次，自动进入配置模式
    if (reconnectAttempts >= 10 && !configModeTriggered) {
      Serial.println("\n⚠️ 重連失敗次數過多，自動進入配置模式");
      Serial.println("請連接 WiFi 熱點 'WaterQualityMonitor' 並訪問 http://192.168.4.1 進行配置");
      startConfigMode();
      configModeTriggered = true;
      reconnectAttempts = 0;  // 重置计数
    }
    
    // 计算重连间隔（指数退避：首次立即，然后5秒、10秒、20秒，最大30秒）
    unsigned long reconnectInterval = 0;
    if (reconnectAttempts == 0) {
      reconnectInterval = 0;  // 首次立即重连
    } else {
      reconnectInterval = min(5000 * (1 << min(reconnectAttempts - 1, 3)), 30000);
    }
    
    // 首次断开或达到重连间隔时尝试重连
    if (lastReconnectAttempt == 0 || (now - lastReconnectAttempt) >= reconnectInterval) {
      lastReconnectAttempt = now;
      reconnectAttempts++;
      
      Serial.printf("\n[重連嘗試 #%d] WiFi 未連接，嘗試重新連接...\n", reconnectAttempts);
      
      // 使用按信号强度自动选择最优 WiFi 重连
      bool reconnectSuccess = false;
      if (wifiConfigCount > 0) {
        reconnectSuccess = connectBestWiFi();
      } else if (default_ssid != NULL) {
        reconnectSuccess = connectWiFi(String(default_ssid), String(default_password), 10000);
      } else {
        Serial.println("沒有可用的 WiFi 配置");
        // 如果没有配置，直接进入配置模式
        if (!configModeTriggered) {
          Serial.println("自動進入配置模式...");
          startConfigMode();
          configModeTriggered = true;
        }
      }
      
      if (reconnectSuccess) {
        reconnectAttempts = 0;  // 重连成功，重置计数
        wasConnected = true;
        configModeTriggered = false;
        lastConnectionTime = now;
        lastRSSI = WiFi.RSSI();
      } else {
        Serial.printf("重連失敗，將在 %lu 秒後重試 (已嘗試 %d 次)\n", 
                      reconnectInterval > 0 ? reconnectInterval / 1000 : 0, reconnectAttempts);
        if (reconnectAttempts >= 10) {
          Serial.println("提示: 如果持續失敗，系統將自動進入配置模式");
        }
      }
    }
    
    // 每5秒显示一次状态（用于监控）
    if (now - lastWiFiCheck >= 5000) {
      lastWiFiCheck = now;
      wl_status_t status = WiFi.status();
      const char* statusText = "";
      switch(status) {
        case WL_IDLE_STATUS: statusText = "空閒"; break;
        case WL_NO_SSID_AVAIL: statusText = "找不到 SSID"; break;
        case WL_CONNECTED: statusText = "已連接"; break;
        case WL_CONNECT_FAILED: statusText = "連接失敗"; break;
        case WL_CONNECTION_LOST: statusText = "連接丟失"; break;
        case WL_DISCONNECTED: statusText = "已斷開"; break;
        default: statusText = "未知狀態"; break;
      }
      Serial.printf("[WiFi 狀態] %d (%s), 等待重連... (已嘗試 %d 次)\n", status, statusText, reconnectAttempts);
    }
  } else if (isConnected) {
    // 已连接，关闭配置热点并停止Web配置（避免干扰）
    stopConfigModeIfRunning();
    wasConnected = true;
  }
  
  // 如果配置服务在运行，处理 Web 服务器请求（即使处于 AP+STA）
  if (configServerRunning) {
    server.handleClient();
    
    // 在配置模式下，每30秒尝试一次连接（如果用户已配置）
    static unsigned long lastConfigModeCheck = 0;
    if (now - lastConfigModeCheck > 30000) {
      lastConfigModeCheck = now;
      if (wifiConfigCount > 0) {
        Serial.println("配置模式：嘗試按信號強度自動選擇最優 WiFi 連接...");
        if (connectBestWiFi()) {
          Serial.println("配置模式：連接成功，切換到正常模式");
          configModeTriggered = false;
          wasConnected = true;
          lastConnectionTime = now;
          lastRSSI = WiFi.RSSI();
          stopConfigModeIfRunning();
        }
      }
    }
  }

  // === 每5分钟刷新一次参数（非阻塞） ===
  static unsigned long lastMeasureMs = 0;
  if (lastMeasureMs != 0 && (now - lastMeasureMs) < MEASURE_INTERVAL_MS) {
    return;
  }
  lastMeasureMs = now;

  Serial.println("\n=== 感測器讀取 ===");

  float nh4 = 0.0;
  float ph = 0.0;
  float temp = 0.0;
  float hum = 0.0;
  float lux = 0.0;
  float waterTemp = 0.0;
  float turbidity = 0.0;
  float tds = 0.0;

  // 铵离子
  uint8_t resNH4 = safeRead(nodeNH4, 0x0000, 2);
  if (resNH4 == nodeNH4.ku8MBSuccess) {
    uint32_t raw = ((uint32_t)nodeNH4.getResponseBuffer(0) << 16) | nodeNH4.getResponseBuffer(1);
    nh4 = *(float*)&raw;
    // 验证数据有效性
    if (!isnan(nh4) && !isinf(nh4) && nh4 >= 0 && nh4 < 1000) {
      Serial.printf("NH4+: %.3f mg/L\n", nh4);
    } else {
      Serial.printf("NH4+ 數據異常 (值: %.3f) → 0\n", nh4);
      nh4 = 0.0;
    }
  } else {
    const char* errorMsg = "";
    switch(resNH4) {
      case 0x01: errorMsg = "非法功能"; break;
      case 0x02: errorMsg = "非法数据地址"; break;
      case 0x03: errorMsg = "非法数据值"; break;
      case 0x04: errorMsg = "从站设备故障"; break;
      case 0xE1: errorMsg = "校验和错误"; break;
      case 0xE2: errorMsg = "接收超时"; break;
      case 0xE3: errorMsg = "无效响应"; break;
      default: errorMsg = "未知错误"; break;
    }
    Serial.printf("NH4 讀取失敗: 0x%02X (%s) → 0\n", resNH4, errorMsg);
    nh4 = 0.0;  // 确保设置为0，上传到ThingSpeak
  }

  esp_task_wdt_reset();

  // pH值
  uint8_t resPH = safeRead(nodePH, 0x0000, 1);
  if (resPH == nodePH.ku8MBSuccess) {
    uint16_t raw = nodePH.getResponseBuffer(0);
    ph = raw / 100.0;  // 假设pH值以整数形式存储（如700表示7.00）
    // 验证数据有效性
    if (!isnan(ph) && !isinf(ph) && ph >= 0 && ph <= 14) {
      Serial.printf("pH: %.2f\n", ph);
    } else {
      Serial.printf("pH 數據異常 (值: %.2f) → 0\n", ph);
      ph = 0.0;
    }
  } else {
    const char* errorMsg = "";
    switch(resPH) {
      case 0x01: errorMsg = "非法功能"; break;
      case 0x02: errorMsg = "非法数据地址"; break;
      case 0x03: errorMsg = "非法数据值"; break;
      case 0x04: errorMsg = "从站设备故障"; break;
      case 0xE1: errorMsg = "校验和错误"; break;
      case 0xE2: errorMsg = "接收超时"; break;
      case 0xE3: errorMsg = "无效响应"; break;
      default: errorMsg = "未知错误"; break;
    }
    Serial.printf("pH 讀取失敗: 0x%02X (%s) → 0\n", resPH, errorMsg);
    ph = 0.0;  // 确保设置为0，上传到ThingSpeak
  }

  esp_task_wdt_reset();

  // 空氣溫濕度
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    hum = h;
    temp = t;
    Serial.printf("空氣濕度: %.1f%%  溫度: %.1f°C\n", hum, temp);
  } else {
    Serial.println("DHT11 異常 → 0");
  }

  // 光照
  static unsigned long lastBH1750Retry = 0;
  if (!bh1750Ready) {
    // 每30秒尝试恢复一次 BH1750（避免频繁重试）
    unsigned long now = millis();
    if (now - lastBH1750Retry > 30000) {
      lastBH1750Retry = now;
      Serial.println("嘗試恢復 BH1750...");
      Wire.begin();
      delay(100);
      bh1750Ready = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
      if (bh1750Ready) {
        Serial.println("BH1750 已恢復");
      } else {
        Serial.println("BH1750 恢復失敗，將在30秒後重試");
      }
      esp_task_wdt_reset();
    }
  }
  
  if (bh1750Ready) {
    float l = lightMeter.readLightLevel();
    if (l >= 0 && !isnan(l) && !isinf(l)) {
      lux = l;
      Serial.printf("光照: %.1f lx\n", lux);
    } else {
      Serial.printf("BH1750 讀取異常 (值: %.2f) → 0\n", l);
      bh1750Ready = false; // 下次循环重试初始化
      lastBH1750Retry = millis(); // 重置重试计时
    }
  } else {
    // 只在第一次或每30秒输出一次，避免刷屏
    static unsigned long lastErrorMsg = 0;
    unsigned long now = millis();
    if (now - lastErrorMsg > 30000) {
      lastErrorMsg = now;
      Serial.println("BH1750 未配置 → 0（將自動重試）");
    }
  }

  // 水溫
  sensors.requestTemperatures();
  float wt = sensors.getTempCByIndex(0);
  if (wt != DEVICE_DISCONNECTED_C && wt > -50 && wt < 150) {
    waterTemp = wt;
    Serial.printf("水溫: %.2f °C\n", waterTemp);
  } else {
    Serial.println("水溫異常 → 0");
  }

  esp_task_wdt_reset();

  // 浊度
  uint8_t resTurb = safeRead(nodeTurb, 0x0000, 1);
  if (resTurb == nodeTurb.ku8MBSuccess) {
    uint16_t raw = nodeTurb.getResponseBuffer(0);
    turbidity = raw / 10.0;
    Serial.printf("浊度: %.1f NTU\n", turbidity);
  } else {
    Serial.printf("浊度異常 (0x%02X) → 0\n", resTurb);
  }

  esp_task_wdt_reset();

  // TDS 水质传感器（模拟量）
  // 读取多次取平均值，提高稳定性
  int tdsSum = 0;
  int tdsReadings = 10;
  for (int i = 0; i < tdsReadings; i++) {
    tdsSum += analogRead(TDS_PIN);
    delay(10);
  }
  int tdsRaw = tdsSum / tdsReadings;
  
  // 将ADC值（0-4095）映射到TDS值（0-900）
  // 假设ADC值线性对应TDS值
  tds = (float)tdsRaw * TDS_MAX_VALUE / ADC_RESOLUTION;
  
  // 限制在有效范围内
  if (tds < 0) tds = 0;
  if (tds > TDS_MAX_VALUE) tds = TDS_MAX_VALUE;
  
  Serial.printf("TDS: %.1f ppm (ADC: %d)\n", tds, tdsRaw);

  // 上傳
  ThingSpeak.setField(1, temp);
  ThingSpeak.setField(2, hum);
  ThingSpeak.setField(3, lux);
  ThingSpeak.setField(4, waterTemp);
  ThingSpeak.setField(5, turbidity);
  ThingSpeak.setField(6, ph);  // pH 值
  ThingSpeak.setField(7, nh4);  // NH4+ 浓度（氨氮）
  ThingSpeak.setField(8, tds);

  // 只有在 WiFi 连接时才上传数据
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n=== 上傳數據到 ThingSpeak ===");
    Serial.printf("Field 1 (溫度): %.1f°C\n", temp);
    Serial.printf("Field 2 (濕度): %.1f%%\n", hum);
    Serial.printf("Field 3 (光照): %.1f lx\n", lux);
    Serial.printf("Field 4 (水溫): %.2f°C\n", waterTemp);
    Serial.printf("Field 5 (浊度): %.1f NTU\n", turbidity);
    Serial.printf("Field 6 (pH): %.2f\n", ph);
    Serial.printf("Field 7 (NH4+): %.3f mg/L\n", nh4);
    Serial.printf("Field 8 (TDS): %.1f ppm\n", tds);
    
    int status = ThingSpeak.writeFields(channelNumber, writeAPIKey);
    if (status == 200) {
      Serial.println("✓ ThingSpeak 上傳成功");
    } else {
      Serial.printf("✗ ThingSpeak 上傳失敗: %d\n", status);
      if (status == -301) {
        Serial.println("  錯誤: 字段值無效");
      } else if (status == -302) {
        Serial.println("  錯誤: 字段編號無效");
      } else if (status == 0) {
        Serial.println("  錯誤: 網絡連接問題");
      }
    }
  } else {
    Serial.println("WiFi 未連接，跳過數據上傳");
  }

  Serial.println("循環完成");
}

