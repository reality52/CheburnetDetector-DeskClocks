#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>
#include <TimeLib.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <pitches.h>
// --- НАСТРОЙКИ ПИНОВ ---
#define SPEAKER_PIN 1
#define SDA_PIN 2
#define SCL_PIN 0
#define LCD_I2C_ADDR 0x27
LiquidCrystal_PCF8574 lcd(LCD_I2C_ADDR);

// --- СЕТЕВЫЕ НАСТРОЙКИ ---
const char* ssid = "YOUR-SSID";
const char* password = "YOUR-PASSWORD";
const char* ntpServer = "CHOSE-AVAILABLE-NTP-SERVER(IP OR ADDRESS)";
const long  gmtOffset_sec = 10800; // GMT OFFSET 
const int   daylightOffset_sec = 0; 

// --- MQTT НАСТРОЙКИ ---
const char* mqtt_server = "YOUR-MQTT-SERVER";
const int   mqtt_port = 1883;
const char* mqtt_user = "";
const char* mqtt_password = "";
const char* mqtt_client_id = "network_monitor_esp";

// --- НАСТРОЙКИ ТИШИНЫ (ночью динамик не пищит) ---
const int SILENT_START_HOUR = 21;   // 21:00
const int SILENT_END_HOUR = 7;      // 07:00

// --- СПИСКИ ДЛЯ ПРОВЕРКИ ---
struct CheckItem {
  const char* name;
  bool isIp;
  bool pinged;
  bool httpChecked;
  unsigned long pingTime;
};

CheckItem worldList[] = {
  {"google.com", false, 0, 0, 0},
  {"cloudflare.com", false, 0, 0, 0},
  {"store.steampowered.com", false, 0, 0, 0},
  {"baidu.com", false, 0, 0, 0},
  {"reality.run.place", false, 0, 0, 0}
};

CheckItem whitelistList[] = {
  {"vk.com", false, 0, 0, 0},
  {"ya.ru", false, 0, 0, 0},
  {"max.ru", false, 0, 0, 0}
};

CheckItem localList[] = {
  {"192.168.0.1", true, 0, 0, 0}
};

const char* daysOfWeek[] = {"Su", "Mn", "Tu", "We", "Th", "Fr", "Sa"};

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
int resultCount = 0;
bool whitelistOk = false;
bool worldOk = false;
unsigned long lastCheckTime = 0;
unsigned long networkProblemBeepTime = 0;
bool networkProblemMode = false;
unsigned long lastBacklightState = 0;
bool backlightOn = true;
unsigned long lastClockUpdate = 0;
bool clockAnimation = true;
unsigned long currentStatus = 0;
unsigned long melodyStartTime = 0;
bool melodyEnabled = false;
bool discoverySent = false;

// Для отображения хода теста на веб-странице
bool testingInProgress = false;
String currentTestingHost = "";
String lastTestResult = "";

// История измерений
struct HistoryEntry {
  unsigned long timestamp;
  uint8_t status;
};
const int MAX_HISTORY = 100;
HistoryEntry history[MAX_HISTORY];
int historyCount = 0;
bool historyChanged = false;

// Синхронизация времени
unsigned long lastNtpSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 24UL * 3600 * 1000;

ESP8266WebServer server(80);
WiFiClient espClient;
PubSubClient client(espClient);

#define TOPIC_STATUS        "homeassistant/network/status"
#define TOPIC_RESULT        "homeassistant/network/result"
#define TOPIC_PING          "homeassistant/network/ping"
#define TOPIC_UPTIME        "homeassistant/network/uptime"

// -------------------------------------------------------------------
// ФУНКЦИИ ИСТОРИИ
// -------------------------------------------------------------------
void loadHistory() {
  if (!LittleFS.begin()) return;
  File file = LittleFS.open("/history.dat", "r");
  if (file) {
    historyCount = 0;
    while (file.available() && historyCount < MAX_HISTORY) {
      file.readBytes((char*)&history[historyCount], sizeof(HistoryEntry));
      historyCount++;
    }
    file.close();
    Serial.printf("Loaded %d history entries\n", historyCount);
  } else {
    Serial.println("No history file, will create new");
  }
  LittleFS.end();
}

void saveHistory() {
  if (!historyChanged) return;
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount FS for saving history");
    return;
  }
  File file = LittleFS.open("/history.dat", "w");
  if (file) {
    for (int i = 0; i < historyCount; i++) {
      file.write((uint8_t*)&history[i], sizeof(HistoryEntry));
    }
    file.close();
    Serial.printf("Saved %d history entries\n", historyCount);
    historyChanged = false;
  } else {
    Serial.println("Failed to open history file for writing");
  }
  LittleFS.end();
}

void addHistoryEntry(uint8_t status) {
  if (historyCount >= MAX_HISTORY) {
    for (int i = 0; i < MAX_HISTORY - 1; i++) {
      history[i] = history[i + 1];
    }
    historyCount = MAX_HISTORY - 1;
  }
  history[historyCount].timestamp = time(nullptr);
  history[historyCount].status = status;
  historyCount++;
  historyChanged = true;
  saveHistory();
  Serial.printf("Added history: status=%d at %lu\n", status, history[historyCount-1].timestamp);
}

String getHistoryHTML() {
  String html = "<table border='1' cellpadding='5'><tr><th>Time</th><th>Status</th></tr>";
  for (int i = 0; i < historyCount; i++) {
    time_t t = (time_t)history[i].timestamp;
    struct tm *tm_info = localtime(&t);
    char timeBuf[20];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", tm_info);
    String statusStr;
    switch (history[i].status) {
      case 0: statusStr = "OK"; break;
      case 1: statusStr = "Network Problem"; break;
      case 2: statusStr = "Dead Internet"; break;
      case 3: statusStr = "Whitelisted"; break;
      case 4: statusStr = "Anomaly"; break;
    }
    html += "<tr><td>" + String(timeBuf) + "</td><td>" + statusStr + "</td></tr>";
  }
  html += "</table>";
  return html;
}

String getHistoryJSON() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < historyCount; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["timestamp"] = (unsigned long)history[i].timestamp;
    obj["status"] = history[i].status;
  }
  String output;
  serializeJson(doc, output);
  return output;
}

// -------------------------------------------------------------------
// ФУНКЦИИ ПРОВЕРКИ
// -------------------------------------------------------------------
void saveTestResult(CheckItem* item) {
  if (LittleFS.begin()) {
    File file = LittleFS.open("/test_result.txt", "w");
    if (file) {
      file.print(item->name);
      file.print(":");
      file.print(item->pinged ? "OK" : "FAIL");
      file.print(",");
      file.print(item->httpChecked ? "OK" : "FAIL");
      file.close();
      resultCount++;
    }
    LittleFS.end();
  }
}

void loadFromFile() {
  if (LittleFS.begin()) {
    File file = LittleFS.open("/test_result.txt", "r");
    if (file) {
      Serial.println("Loaded log from FS");
      while (file.available()) {
        Serial.write(file.read());
      }
      file.close();
    }
    LittleFS.end();
  }
}

void saveToFile() {
  if (LittleFS.begin()) {
    File file = LittleFS.open("/status_log.txt", "w");
    if (file) {
      file.print("Status: ");
      file.print(currentStatus);
      file.print(", Count: ");
      file.print(resultCount);
      file.close();
      Serial.println("Status saved to FS");
    }
    LittleFS.end();
  }
}

// -------------------------------------------------------------------
// MQTT
// -------------------------------------------------------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("MQTT Received: ");
  Serial.print(topic);
  Serial.print(" - ");
  Serial.println(message);
  if (String(topic) == "homeassistant/network/command") {
    if (message == "restart") {
      ESP.restart();
    } else if (message == "clear") {
      resultCount = 0;
      saveToFile();
    } else if (message == "test") {
      runTestWithAnimation();
    }
  }
}

void sendMQTTDiscovery() {
  if (!client.connected()) return;
  String deviceJson = "\"device\":{\"identifiers\":[\"network_monitor_esp\"],\"name\":\"Network Monitor\",\"model\":\"ESP8266-01\",\"manufacturer\":\"DIY\"}";
  String statusConfig = "{\"name\":\"Network Status\",\"state_topic\":\"" TOPIC_STATUS "\",\"unique_id\":\"network_monitor_status\",\"icon\":\"mdi:network-strength-4\"," + deviceJson + "}";
  client.publish("homeassistant/sensor/network_monitor_status/config", statusConfig.c_str(), true);
  String uptimeConfig = "{\"name\":\"Network Uptime\",\"state_topic\":\"" TOPIC_UPTIME "\",\"unit_of_measurement\":\"h\",\"unique_id\":\"network_monitor_uptime\",\"icon\":\"mdi:clock-outline\"," + deviceJson + "}";
  client.publish("homeassistant/sensor/network_monitor_uptime/config", uptimeConfig.c_str(), true);
  String resultConfig = "{\"name\":\"Network Result\",\"state_topic\":\"" TOPIC_RESULT "\",\"value_template\":\"{{ value_json.status }}\",\"json_attributes_topic\":\"" TOPIC_RESULT "\",\"unique_id\":\"network_monitor_result\",\"icon\":\"mdi:chart-line\"," + deviceJson + "}";
  client.publish("homeassistant/sensor/network_monitor_result/config", resultConfig.c_str(), true);
  String localConfig = "{\"name\":\"Local Router OK\",\"state_topic\":\"" TOPIC_RESULT "\",\"value_template\":\"{{ value_json.localOk }}\",\"device_class\":\"connectivity\",\"unique_id\":\"network_monitor_local_ok\"," + deviceJson + "}";
  client.publish("homeassistant/binary_sensor/network_monitor_local_ok/config", localConfig.c_str(), true);
  String whitelistConfig = "{\"name\":\"Whitelist Sites OK\",\"state_topic\":\"" TOPIC_RESULT "\",\"value_template\":\"{{ value_json.whitelistOk }}\",\"device_class\":\"connectivity\",\"unique_id\":\"network_monitor_whitelist_ok\"," + deviceJson + "}";
  client.publish("homeassistant/binary_sensor/network_monitor_whitelist_ok/config", whitelistConfig.c_str(), true);
  String worldConfig = "{\"name\":\"World Sites OK\",\"state_topic\":\"" TOPIC_RESULT "\",\"value_template\":\"{{ value_json.worldOk }}\",\"device_class\":\"connectivity\",\"unique_id\":\"network_monitor_world_ok\"," + deviceJson + "}";
  client.publish("homeassistant/binary_sensor/network_monitor_world_ok/config", worldConfig.c_str(), true);
  discoverySent = true;
  Serial.println("MQTT Discovery sent");
}

void connectMQTT() {
  Serial.print("Connecting to MQTT...");
  if (!client.connected()) {
    client.connect(mqtt_client_id, mqtt_user, mqtt_password);
    if (client.connected()) {
      Serial.println(" OK!");
      client.subscribe("homeassistant/network/command");
      if (!discoverySent) sendMQTTDiscovery();
    } else {
      Serial.println(" Failed!");
    }
  } else {
    Serial.println(" Already connected");
  }
}

void sendMQTTStatus() {
  String statusStr;
  switch (currentStatus) {
    case 0: statusStr = "OK"; break;
    case 1: statusStr = "Network Problem"; break;
    case 2: statusStr = "Dead Internet"; break;
    case 3: statusStr = "Whitelisted"; break;
    case 4: statusStr = "Anomaly"; break;
  }
  client.publish(TOPIC_STATUS, statusStr.c_str());
  String json = "{\"status\":\"" + statusStr + "\",\"timestamp\":" + String(time(nullptr)) + ",\"localOk\":" + String(localList[0].pinged ? "true" : "false") + ",\"whitelistOk\":" + String(whitelistOk ? "true" : "false") + ",\"worldOk\":" + String(worldOk ? "true" : "false") + "}";
  client.publish(TOPIC_RESULT, json.c_str());
  String uptimeStr = String((millis() / 1000 / 60 / 60) % 24) + "h";
  client.publish(TOPIC_UPTIME, uptimeStr.c_str());
}

void sendMQTTPingResult(const CheckItem* item) {
  char topic[100];
  snprintf(topic, sizeof(topic), "%s/%s", TOPIC_PING, item->name);
  String json = "{\"pinged\":" + String(item->pinged ? "true" : "false") + ",\"httpChecked\":" + String(item->httpChecked ? "true" : "false") + ",\"pingTime\":" + String(item->pingTime) + ",\"timestamp\":" + String(time(nullptr)) + "}";
  client.publish(topic, json.c_str());
}

// -------------------------------------------------------------------
// WEB HANDLERS (с AJAX, без перезагрузки)
// -------------------------------------------------------------------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Network Monitor</title>";
  html += "<style>body{font-family:Arial;margin:20px;} table{border-collapse:collapse;} th,td{border:1px solid #ccc;padding:8px;} .status-ok{color:green;} .status-problem{color:red;} .status-whitelist{color:orange;}</style>";
  html += "<script>";
  html += "async function call(url) { await fetch(url); location.reload(); }";
  html += "async function clearLogs() { await fetch('/clear'); location.reload(); }";
  html += "async function runTest() { await fetch('/refresh'); location.reload(); }";
  html += "async function tetris() { await fetch('/tetris'); }";
  html += "function updateTime() { fetch('/now').then(r=>r.json()).then(d=>{ document.getElementById('server-time').innerText=d.datetime; }); }";
  html += "function updateTestProgress() { fetch('/testprogress').then(r=>r.json()).then(d=>{ if(d.inProgress) { document.getElementById('test-status').innerHTML='<b>Testing:</b> '+d.currentHost+' → '+d.lastResult; } else { document.getElementById('test-status').innerHTML='<b>Last test:</b> '+d.lastResult; } }); }";
  html += "setInterval(updateTime,1000); setInterval(updateTestProgress,2000); window.onload=function(){ updateTime(); updateTestProgress(); };";
  html += "</script></head><body>";
  html += "<h1>Network Monitor</h1>";
  html += "<p><strong>Server time:</strong> <span id='server-time'>--</span></p>";
  html += "<p><strong>Current status:</strong> <span id='current-status' class='";
  switch (currentStatus) {
    case 0: html += "status-ok"; break;
    case 1: case 2: html += "status-problem"; break;
    case 3: case 4: html += "status-whitelist"; break;
  }
  html += "'>";
  switch (currentStatus) {
    case 0: html += "OK"; break;
    case 1: html += "Network Problem"; break;
    case 2: html += "Dead Internet"; break;
    case 3: html += "Whitelisted"; break;
    case 4: html += "Anomaly"; break;
  }
  html += "</span></p>";
  html += "<div id='test-status' style='background:#f0f0f0;padding:10px;margin:10px 0;'>Loading...</div>";
  html += "<p><button onclick='clearLogs()'>Clear Logs</button> <button onclick='runTest()'>Run test now</button> <button onclick='tetris()'>Tetris Melody</button></p>";
  html += "<h2>History</h2>";
  html += getHistoryHTML();
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleNow() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    String json = "{\"datetime\":\"" + String(buf) + "\"}";
    server.send(200, "application/json", json);
  } else {
    server.send(503, "application/json", "{\"error\":\"time not synced\"}");
  }
}

void handleTestProgress() {
  String json = "{\"inProgress\":" + String(testingInProgress ? "true" : "false") + ",\"currentHost\":\"" + currentTestingHost + "\",\"lastResult\":\"" + lastTestResult + "\",\"lastCheck\":" + String(lastCheckTime / 1000) + "}";
  server.send(200, "application/json", json);
}

void handleStatus() {
  String statusStr;
  switch (currentStatus) { case 0: statusStr = "OK"; break; case 1: statusStr = "Network Problem"; break; case 2: statusStr = "Dead Internet"; break; case 3: statusStr = "Whitelisted"; break; case 4: statusStr = "Anomaly"; break; }
  String json = "{\"status\":\"" + statusStr + "\",\"uptime\":\"" + String((millis() / 1000 / 60 / 60) % 24) + "h\"}";
  server.send(200, "application/json", json);
}

void handleClear() {
  resultCount = 0;
  saveToFile();
  historyCount = 0;
  historyChanged = true;
  saveHistory();
  server.send(200, "text/plain", "Cleared");
}

void handleRefresh() {
  runTestWithAnimation();
  server.send(200, "text/plain", "Test completed");
}

void handleHistory() {
  server.send(200, "application/json", getHistoryJSON());
}

void handleTetris() {
  // Проигрываем мелодию Tetris через динамик (короткая версия)
  // Не блокируем, запускаем в отдельной функции
  playTetrisMelody();
  server.send(200, "text/plain", "Tetris!");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

// -------------------------------------------------------------------
// АНИМАЦИЯ ПОВЕРХ ЧАСОВ (не стирает экран)
// -------------------------------------------------------------------
void showTestAnimation() {
  // Сохраняем область экрана с часами? Нет, часы не стираем, просто рисуем в углах.
  // Но чтобы после анимации убрать символы, мы их затрём пробелами.
  const char* frames[] = {"*", "-", "#", "+"};
  for (int f = 0; f < 8; f++) {
    lcd.setCursor(0, 0); lcd.print(frames[f % 4]);
    lcd.setCursor(15, 0); lcd.print(frames[(f+1) % 4]);
    lcd.setCursor(15, 1); lcd.print(frames[(f+2) % 4]);
    lcd.setCursor(0, 1); lcd.print(frames[(f+3) % 4]);
    delay(200);
  }
  // Стираем символы из углов (ставим пробелы)
  lcd.setCursor(0, 0); lcd.print(' ');
  lcd.setCursor(15, 0); lcd.print(' ');
  lcd.setCursor(15, 1); lcd.print(' ');
  lcd.setCursor(0, 1); lcd.print(' ');
}

// -------------------------------------------------------------------
// ПРОВЕРКИ
// -------------------------------------------------------------------
bool ping(const char* host) {
  WiFiClient client;
  IPAddress ip;
  if (!WiFi.hostByName(host, ip)) return false;
  if (client.connect(ip, 80)) { client.stop(); return true; }
  client.stop();
  if (client.connect(ip, 443)) { client.stop(); return true; }
  return false;
}

bool checkHttp(const char* host) {
  WiFiClient client;
  // Пробуем подключиться только на порт 80 (HTTP)
  if (!client.connect(host, 80)) {
    return false;
  }
  
  // Отправляем HTTP-запрос
  client.print(String("GET / HTTP/1.1\r\n") +
               "Host: " + String(host) + "\r\n" +
               "User-Agent: ESP8266/1.0\r\n" +
               "Connection: close\r\n\r\n");
  
  unsigned long timeout = millis() + 10000; // 10 секунд на ответ
  bool httpSuccess = false;
  
  while (client.connected() && millis() < timeout) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      // Ищем первую строку ответа, например "HTTP/1.1 200 OK"
      if (line.startsWith("HTTP/")) {
        // Проверяем код ответа: 2xx или 3xx считаем успехом
        if (line.indexOf("200") != -1 || line.indexOf("201") != -1 ||
            line.indexOf("301") != -1 || line.indexOf("302") != -1 ||
            line.indexOf("303") != -1 || line.indexOf("304") != -1 ||
            line.indexOf("307") != -1 || line.indexOf("308") != -1) {
          httpSuccess = true;
        }
        break; // первую строку прочитали, дальше можно не читать
      }
    }
    delay(10);
  }
  client.stop();
  return httpSuccess;
}

void resetFlags() {
  for (int i = 0; i < 1; i++) { localList[i].pinged = false; localList[i].httpChecked = false; localList[i].pingTime = 0; }
  for (int i = 0; i < 3; i++) { whitelistList[i].pinged = false; whitelistList[i].httpChecked = false; whitelistList[i].pingTime = 0; }
  for (int i = 0; i < 5; i++) { worldList[i].pinged = false; worldList[i].httpChecked = false; worldList[i].pingTime = 0; }
}

void checkHosts(CheckItem* list, int count, bool isCritical, bool silent = true) {
  for (int i = 0; i < count; i++) {
    currentTestingHost = list[i].name;
    unsigned long pingStart = millis();
    bool pingResult = ping(list[i].name);
    if (pingResult) {
      list[i].pinged = true;
      list[i].pingTime = millis() - pingStart;
      lastTestResult = "OK (ping " + String(list[i].pingTime) + "ms)";
    } else {
      list[i].pinged = false;
      list[i].pingTime = 0;
      lastTestResult = "FAIL (ping)";
    }
    bool httpResult = false;
    if (!list[i].isIp) {
      httpResult = checkHttp(list[i].name);
      list[i].httpChecked = httpResult;
      if (httpResult) lastTestResult += " HTTP OK";
      else lastTestResult += " HTTP FAIL";
    } else {
      list[i].httpChecked = pingResult;
    }
    saveTestResult(&list[i]);
    sendMQTTPingResult(&list[i]);
    delay(100);
  }
}

bool isSilentHour() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  int hour = timeinfo.tm_hour;
  if (SILENT_START_HOUR < SILENT_END_HOUR) {
    return (hour >= SILENT_START_HOUR && hour < SILENT_END_HOUR);
  } else {
    return (hour >= SILENT_START_HOUR || hour < SILENT_END_HOUR);
  }
}

void evaluateStatus() {
  bool localOk = localList[0].pinged;
  whitelistOk = true;
  for (int i = 0; i < 3; i++) if (!whitelistList[i].pinged && !whitelistList[i].httpChecked) { whitelistOk = false; break; }
  worldOk = true;
  for (int i = 0; i < 5; i++) if (!worldList[i].pinged && !worldList[i].httpChecked) { worldOk = false; break; }
  
  unsigned long newStatus;
  if (!localOk) newStatus = 1;
  else if (localOk && !whitelistOk && !worldOk) newStatus = 2;
  else if (localOk && whitelistOk && !worldOk) newStatus = 3;
  else if (!whitelistOk || !worldOk) newStatus = 4;
  else newStatus = 0;
  
  addHistoryEntry(newStatus);
  
  if (newStatus != currentStatus) {
    currentStatus = newStatus;
    if (currentStatus != 0) {
      lcd.clear();
      melodyStartTime = millis();
      melodyEnabled = true;
      networkProblemMode = true;
      switch (currentStatus) {
        case 1: lcd.print("Network Problem!"); break;
        case 2: lcd.print("DEAD INTERNET"); break;
        case 3: lcd.print("! WHITELISTED !"); break;
        case 4: lcd.print("Anomaly !"); break;
      }
      lastBacklightState = millis();
      backlightOn = true;
    } else {
      melodyEnabled = false;
      networkProblemMode = false;
      lcd.setBacklight(255);
      backlightOn = true;
      lastClockUpdate = millis();
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) updateClockDisplay(&timeinfo);
      else lcd.print("Time error");
    }
    sendMQTTStatus();
    saveToFile();
  }
}

void updateClockDisplay(struct tm* timeinfo) {
  char timeStr[10];
  sprintf(timeStr, "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
  if (clockAnimation) { timeStr[2] = ':'; clockAnimation = false; }
  else { timeStr[2] = ' '; clockAnimation = true; }
  lcd.setCursor(5, 0); lcd.print(timeStr);
  char dateStr[20];
  sprintf(dateStr, "%s %02d.%02d.%04d", daysOfWeek[timeinfo->tm_wday], timeinfo->tm_mday, timeinfo->tm_mon+1, timeinfo->tm_year+1900);
  lcd.setCursor(1, 1);
  lcd.print(dateStr);
}

void runTestWithAnimation() {
  testingInProgress = true;
  showTestAnimation();   // анимация поверх часов
  resetFlags();
  checkHosts(localList, 1, true, true);
  checkHosts(whitelistList, 3, false, true);
  checkHosts(worldList, 5, false, true);
  evaluateStatus();
  testingInProgress = false;
  // После теста обновляем экран
  lcd.clear();
  if (currentStatus == 0) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) updateClockDisplay(&timeinfo);
    else lcd.print("Time error");
  } else {
    switch (currentStatus) {
      case 1: lcd.setCursor(2,0);lcd.print("Network");lcd.setCursor(2,1);lcd.print("Problem!"); break;
      case 2: lcd.setCursor(5,0);lcd.print("! DEAD !");lcd.setCursor(2,1);lcd.print("! INTERNET !"); break;
      case 3: lcd.setCursor(2,0);lcd.print("! WHITELISTED !"); break;
      case 4: lcd.setCursor(2,0);lcd.print("Anomaly !"); break;
    }
  }
}

// -------------------------------------------------------------------
// МЕЛОДИИ
// -------------------------------------------------------------------
void beepStartup() { tone(SPEAKER_PIN, 1000, 100); delay(50); tone(SPEAKER_PIN, 1500, 100); }
void beepWifiSuccess() { tone(SPEAKER_PIN, 800, 100); delay(50); tone(SPEAKER_PIN, 1200, 100); delay(50); tone(SPEAKER_PIN, 1600, 100); }
void beepWifiFail() { tone(SPEAKER_PIN, 300, 300); delay(100); tone(SPEAKER_PIN, 300, 300); }

void playStatusMelody() {
  if (!melodyEnabled) return;
  if (isSilentHour()) {
    // В тихий час не пищим
    melodyEnabled = false;
    return;
  }
  if (millis() - melodyStartTime > 10000) { melodyEnabled = false; return; }
  switch (currentStatus) {
    case 1: tone(SPEAKER_PIN, 300, 200); break;
    case 2: tone(SPEAKER_PIN, 200, 500); break;
    case 3: tone(SPEAKER_PIN, 1500, 500); break;
    case 4: tone(SPEAKER_PIN, 800, 200); break;
  }
}

void playTetrisMelody() {
  if (isSilentHour()) return; // ночью не играем
  // Простая версия мелодии Tetris
  int melody[] = {
  NOTE_E5, NOTE_B4, NOTE_C5, NOTE_D5, NOTE_C5, NOTE_B4,
  NOTE_A4, NOTE_A4, NOTE_C5, NOTE_E5, NOTE_D5, NOTE_C5,
  NOTE_B4, NOTE_C5, NOTE_D5, NOTE_E5,
  NOTE_C5, NOTE_A4, NOTE_A4, NOTE_A4, NOTE_B4, NOTE_C5,
  
  NOTE_D5, NOTE_F5, NOTE_A5, NOTE_G5, NOTE_F5,
  NOTE_E5, NOTE_C5, NOTE_E5, NOTE_D5, NOTE_C5,
  NOTE_B4, NOTE_B4, NOTE_C5, NOTE_D5, NOTE_E5,
  NOTE_C5, NOTE_A4, NOTE_A4, REST, 
  
  NOTE_E5, NOTE_B4, NOTE_C5, NOTE_D5, NOTE_C5, NOTE_B4,
  NOTE_A4, NOTE_A4, NOTE_C5, NOTE_E5, NOTE_D5, NOTE_C5,
  NOTE_B4, NOTE_C5, NOTE_D5, NOTE_E5,
  NOTE_C5, NOTE_A4, NOTE_A4, NOTE_A4, NOTE_B4, NOTE_C5,
  
  NOTE_D5, NOTE_F5, NOTE_A5, NOTE_G5, NOTE_F5,
  NOTE_E5, NOTE_C5, NOTE_E5, NOTE_D5, NOTE_C5,
  NOTE_B4, NOTE_B4, NOTE_C5, NOTE_D5, NOTE_E5,
  NOTE_C5, NOTE_A4, NOTE_A4, REST, 
  
  NOTE_E5, NOTE_C5,
  NOTE_D5, NOTE_B4,
  NOTE_C5, NOTE_A4,
  NOTE_GS4, NOTE_B4, REST, 
  NOTE_E5, NOTE_C5,
  NOTE_D5, NOTE_B4,
  NOTE_C5, NOTE_E5, NOTE_A5,
  NOTE_GS5
};

int durations[] = {
  4, 8, 8, 4, 8, 8,
  4, 8, 8, 4, 8, 8,
  4, 8, 4, 4,
  4, 4, 8, 4, 8, 8,
  
  4, 8, 4, 8, 8,
  4, 8, 4, 8, 8,
  4, 8, 8, 4, 4,
  4, 4, 4, 4,
  
  4, 8, 8, 4, 8, 8,
  4, 8, 8, 4, 8, 8,
  4, 8, 4, 4,
  4, 4, 8, 4, 8, 8,
  
  4, 8, 4, 8, 8,
  4, 8, 4, 8, 8,
  4, 8, 8, 4, 4,
  4, 4, 4, 4,
  
  2, 2,
  2, 2,
  2, 2,
  2, 4, 8, 
  2, 2,
  2, 2,
  4, 4, 2,
  2
};

  int size = sizeof(durations) / sizeof(int);

  for (int note = 0; note < size; note++) {
    //to calculate the note duration, take one second divided by the note type.
    //e.g. quarter note = 1000 / 4, eighth note = 1000/8, etc.
    int duration = 1000 / durations[note];
    tone(SPEAKER_PIN, melody[note], duration);

    //to distinguish the notes, set a minimum time between them.
    //the note's duration + 30% seems to work well:
    int pauseBetweenNotes = duration * 1.30;
    delay(pauseBetweenNotes);

  }
  noTone(SPEAKER_PIN);
}

// -------------------------------------------------------------------
// ВРЕМЯ
// -------------------------------------------------------------------
void syncTimeIfNeeded() {
  unsigned long nowMs = millis();
  if (lastNtpSync == 0 || (nowMs - lastNtpSync > NTP_SYNC_INTERVAL)) {
    Serial.println("Syncing time with NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    lastNtpSync = nowMs;
    struct tm timeinfo;
    int attempts = 0;
    while (!getLocalTime(&timeinfo) && attempts < 20) {
      delay(250);
      attempts++;
    }
    if (attempts < 20) Serial.println("Time synced");
    else Serial.println("Time sync failed");
  }
}

// -------------------------------------------------------------------
// SETUP
// -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(SPEAKER_PIN, OUTPUT);
  noTone(SPEAKER_PIN);
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.begin(16, 2);
  lcd.setBacklight(255);
  lcd.print("Initializing...");
  
  if (!LittleFS.begin()) { 
    Serial.println("LittleFS Mount Failed!"); 
    lcd.print("FS Error!"); 
    while(true) delay(1000); 
  }
  loadHistory();
  loadFromFile();
  
  beepStartup();
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 50) {
    delay(500); 
    Serial.print(".");
    lcd.setCursor(0,1); lcd.print("WiFi...");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    lcd.clear(); lcd.print("WiFi: OK");
    beepWifiSuccess();
    delay(1000);
    lcd.clear();
    
    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/clear", handleClear);
    server.on("/refresh", handleRefresh);
    server.on("/history", handleHistory);
    server.on("/now", handleNow);
    server.on("/testprogress", handleTestProgress);
    server.on("/tetris", handleTetris);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("Web Server Started");
    
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(mqttCallback);
    
    syncTimeIfNeeded();
    
    lastCheckTime = millis() - 180000 + 15000;
  } else {
    Serial.println("WiFi Connect Failed!");
    lcd.clear(); lcd.print("No WiFi!");
    beepWifiFail();
    while(true) delay(100);
  }
}

// -------------------------------------------------------------------
// LOOP
// -------------------------------------------------------------------
void loop() {
  if (!client.connected()) connectMQTT();
  client.loop();
  server.handleClient();
  syncTimeIfNeeded();
  
  unsigned long currentMillis = millis();
  if (currentMillis - lastCheckTime > 180000) {
    lastCheckTime = currentMillis;
    runTestWithAnimation();
  }
  
  if (currentStatus == 0) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      if (currentMillis - lastClockUpdate >= 1000) {
        lastClockUpdate = currentMillis;
        updateClockDisplay(&timeinfo);
      }
    }
    if (!backlightOn) { lcd.setBacklight(255); backlightOn = true; }
  } else {
    unsigned long blinkInterval = 100;
    if (currentMillis - lastBacklightState > blinkInterval) {
      lastBacklightState = currentMillis;
      backlightOn = !backlightOn;
      lcd.setBacklight(backlightOn ? 255 : 0);
    }
    if (currentMillis - networkProblemBeepTime > 1000) {
      networkProblemBeepTime = currentMillis;
      playStatusMelody();
    }
  }
}
