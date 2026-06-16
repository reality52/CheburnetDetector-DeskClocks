#include <ESP8266WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <pitches.h>
#include <DNSServer.h>
// --- НАСТРОЙКИ ПИНОВ ---
#define SPEAKER_PIN 1
#define SDA_PIN 2
#define SCL_PIN 0
#define LCD_I2C_ADDR 0x27
LiquidCrystal_PCF8574 lcd(LCD_I2C_ADDR);

// --- СЕТЕВЫЕ НАСТРОЙКИ ---
char wifiSsid[33] = "your-ssid";
char wifiPass[65] = "your-pass";
const char* AP_SSID = "NetworkMonitor";
const char* AP_PASS = "12345678";


// --- НАСТРОЙКИ ТИШИНЫ (ночью динамик не пищит) ---


// --- СПИСКИ ДЛЯ ПРОВЕРКИ ---
// Структура элемента для проверки: имя хоста, флаг IP-адреса, результаты ping и HTTP, время ping
struct CheckItem {
  const char* name;
  bool isIp;
  bool pinged;
  bool httpChecked;
  unsigned long pingTime;
};

// --- КОНФИГУРИРУЕМЫЕ СПИСКИ ХОСТОВ ---
const int MAX_HOSTS = 10;

String cfgLocalHosts[MAX_HOSTS] = {"192.168.0.1"};
int cfgLocalCount = 1;

String cfgWhitelistHosts[MAX_HOSTS] = {"vk.com", "ya.ru", "max.ru"};
int cfgWhitelistCount = 3;

String cfgWorldHosts[MAX_HOSTS] = {"google.com", "cloudflare.com", "store.steampowered.com", "baidu.com", "reality.run.place"};
int cfgWorldCount = 5;

// --- ДИНАМИЧЕСКИЕ МАССИВЫ ПРОВЕРКИ (rebuilt from cfg arrays before each test) ---
CheckItem localList[MAX_HOSTS];
CheckItem whitelistList[MAX_HOSTS];
CheckItem worldList[MAX_HOSTS];

const char* daysOfWeek[] = {"Su", "Mn", "Tu", "We", "Th", "Fr", "Sa"};

// --- КОНФИГУРИРУЕМЫЕ ПАРАМЕТРЫ ---
const char* cfgNtpServer = "0.ru.pool.ntp.org";
int cfgGmtOffset = 10800;
int cfgDstOffset = 0;
int cfgSilentStart = 21;
int cfgSilentEnd = 8;
int checkInterval = 180;

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
int resultCount = 0;
bool whitelistOk = false;
bool worldOk = false;
unsigned long lastCheckTime = 0;
unsigned long networkProblemBeepTime = 0;
unsigned long lastBacklightState = 0;
bool backlightOn = true;
unsigned long lastClockUpdate = 0;
bool clockAnimation = true;
unsigned long currentStatus = 0;
unsigned long melodyStartTime = 0;
bool melodyEnabled = false;

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

bool apMode = false;
DNSServer dnsServer;
unsigned long wifiReconnectStart = 0;

ESP8266WebServer server(80);

// -------------------------------------------------------------------
// ФУНКЦИИ ИСТОРИИ
// -------------------------------------------------------------------
// Загружает историю проверок из файла /history.dat на LittleFS
// Читает до MAX_HISTORY записей типа HistoryEntry
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

// Сохраняет историю проверок в файл /history.dat (только если есть изменения)
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

// Добавляет запись в историю с текущей меткой времени и указанным статусом
// При переполнении вытесняет самую старую запись (сдвигом)
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

// Формирует HTML-таблицу истории для отображения на веб-странице
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

// Формирует JSON-массив истории (timestamp, status) для AJAX-запроса
String getHistoryJSON() {
  String output = "[";
  for (int i = 0; i < historyCount; i++) {
    if (i > 0) output += ",";
    output += "{\"timestamp\":" + String((unsigned long)history[i].timestamp);
    output += ",\"status\":" + String(history[i].status) + "}";
  }
  output += "]";
  return output;
}

// -------------------------------------------------------------------
// ФУНКЦИИ ПРОВЕРКИ
// -------------------------------------------------------------------
// Сохраняет результат проверки одного хоста в файл /test_result.txt
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

// Загружает и выводит в Serial последний сохранённый лог теста
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

// Сохраняет текущий статус и счётчик результатов в /status_log.txt
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
// КОНФИГУРАЦИЯ WIFI (LittleFS)
// -------------------------------------------------------------------
// Загружает SSID и пароль WiFi из файла /wifi.conf
void loadWifiConfig() {
  if (!LittleFS.begin()) return;
  File file = LittleFS.open("/wifi.conf", "r");
  if (file) {
    while (file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.startsWith("ssid=")) {
        strncpy(wifiSsid, line.substring(5).c_str(), 32);
        wifiSsid[32] = '\0';
      } else if (line.startsWith("pass=")) {
        strncpy(wifiPass, line.substring(5).c_str(), 64);
        wifiPass[64] = '\0';
      }
    }
    file.close();
    Serial.printf("WiFi config loaded: SSID=%s\n", wifiSsid);
  }
  LittleFS.end();
}

// Сохраняет SSID и пароль WiFi в файл /wifi.conf
void saveWifiConfig(const char* newSsid, const char* newPass) {
  if (!LittleFS.begin()) return;
  File file = LittleFS.open("/wifi.conf", "w");
  if (file) {
    file.print("ssid=");
    file.println(newSsid);
    file.print("pass=");
    file.println(newPass);
    file.close();
    Serial.println("WiFi config saved");
  }
  LittleFS.end();
}

// -------------------------------------------------------------------
// КОНФИГУРАЦИЯ УСТРОЙСТВА (LittleFS /config.txt)
// -------------------------------------------------------------------
// Загружает все настройки из /config.txt
void loadConfig() {
  if (!LittleFS.begin()) return;
  File file = LittleFS.open("/config.txt", "r");
  if (!file) { LittleFS.end(); return; }
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.startsWith("ntp_server=")) { free((void*)cfgNtpServer); cfgNtpServer = strdup(line.substring(11).c_str()); }
    else if (line.startsWith("gmt_offset=")) cfgGmtOffset = line.substring(11).toInt();
    else if (line.startsWith("dst_offset=")) cfgDstOffset = line.substring(11).toInt();
    else if (line.startsWith("silent_start=")) cfgSilentStart = line.substring(13).toInt();
    else if (line.startsWith("silent_end=")) cfgSilentEnd = line.substring(11).toInt();
    else if (line.startsWith("check_interval=")) checkInterval = line.substring(15).toInt();
    else if (line.startsWith("local_hosts=")) {
      cfgLocalCount = 0;
      String val = line.substring(12);
      int start = 0;
      while (start < (int)val.length() && cfgLocalCount < MAX_HOSTS) {
        int comma = val.indexOf(',', start);
        if (comma == -1) comma = val.length();
        String h = val.substring(start, comma); h.trim();
        if (h.length() > 0) { cfgLocalHosts[cfgLocalCount] = h; cfgLocalCount++; }
        start = comma + 1;
      }
    }
    else if (line.startsWith("whitelist_hosts=")) {
      cfgWhitelistCount = 0;
      String val = line.substring(16);
      int start = 0;
      while (start < (int)val.length() && cfgWhitelistCount < MAX_HOSTS) {
        int comma = val.indexOf(',', start);
        if (comma == -1) comma = val.length();
        String h = val.substring(start, comma); h.trim();
        if (h.length() > 0) { cfgWhitelistHosts[cfgWhitelistCount] = h; cfgWhitelistCount++; }
        start = comma + 1;
      }
    }
    else if (line.startsWith("world_hosts=")) {
      cfgWorldCount = 0;
      String val = line.substring(12);
      int start = 0;
      while (start < (int)val.length() && cfgWorldCount < MAX_HOSTS) {
        int comma = val.indexOf(',', start);
        if (comma == -1) comma = val.length();
        String h = val.substring(start, comma); h.trim();
        if (h.length() > 0) { cfgWorldHosts[cfgWorldCount] = h; cfgWorldCount++; }
        start = comma + 1;
      }
    }
  }
  file.close();
  LittleFS.end();
  Serial.println("Config loaded from /config.txt");
}

// Сохраняет все настройки в /config.txt
void saveConfig() {
  if (!LittleFS.begin()) return;
  File file = LittleFS.open("/config.txt", "w");
  if (!file) { LittleFS.end(); return; }
  file.print("ntp_server="); file.println(cfgNtpServer);
  file.print("gmt_offset="); file.println(cfgGmtOffset);
  file.print("dst_offset="); file.println(cfgDstOffset);
  file.print("silent_start="); file.println(cfgSilentStart);
  file.print("silent_end="); file.println(cfgSilentEnd);
  file.print("check_interval="); file.println(checkInterval);
  file.print("local_hosts=");
  for (int i = 0; i < cfgLocalCount; i++) { if (i > 0) file.print(","); file.print(cfgLocalHosts[i]); }
  file.println();
  file.print("whitelist_hosts=");
  for (int i = 0; i < cfgWhitelistCount; i++) { if (i > 0) file.print(","); file.print(cfgWhitelistHosts[i]); }
  file.println();
  file.print("world_hosts=");
  for (int i = 0; i < cfgWorldCount; i++) { if (i > 0) file.print(","); file.print(cfgWorldHosts[i]); }
  file.println();
  file.close();
  LittleFS.end();
  Serial.println("Config saved to /config.txt");
}

// Вспомогательная: извлечь строковое значение JSON по ключу
String jsonGetString(const String& body, const String& key) {
  int p = body.indexOf("\"" + key + "\":\"");
  if (p == -1) return "";
  p += key.length() + 4;
  int e = body.indexOf("\"", p);
  return body.substring(p, e);
}

// Вспомогательная: извлечь числовое значение JSON по ключу (или defaultVal)
int jsonGetInt(const String& body, const String& key, int defaultVal) {
  int p = body.indexOf("\"" + key + "\":");
  if (p == -1) return defaultVal;
  p += key.length() + 3;
  return body.substring(p).toInt();
}

// Страница настроек (/settings)
void handleSettingsPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1">";
  html += "<title>Settings</title><style>";
  html += "*{box-sizing:border-box;margin:0;padding:0}";
  html += "body{font-family:'Segoe UI',Arial,sans-serif;background:#1a1a2e;color:#eee;padding:20px}";
  html += ".wrap{max-width:700px;margin:0 auto}";
  html += "h1{text-align:center;color:#e94560;margin-bottom:20px}";
  html += ".card{background:#16213e;border-radius:12px;padding:24px;margin-bottom:16px}";
  html += "h2{color:#e94560;font-size:1.1em;margin-bottom:12px;border-bottom:1px solid #253554;padding-bottom:8px}";
  html += ".row{display:flex;gap:12px;margin-bottom:12px;flex-wrap:wrap}";
  html += ".field{flex:1;min-width:140px}";
  html += "label{display:block;font-size:.8em;color:#888;text-transform:uppercase;letter-spacing:1px;margin-bottom:4px}";
  html += "input[type=text],input[type=number],select{width:100%;padding:8px;border:1px solid #333;border-radius:6px;background:#0f3460;color:#eee;font-size:.95em}";
  html += "input:focus,select:focus{outline:none;border-color:#e94560}";
  html += ".host-list{margin-bottom:8px}";
  html += ".host-row{display:flex;align-items:center;gap:8px;margin-bottom:6px;padding:6px 10px;background:#0f3460;border-radius:8px}";
  html += ".host-row input{flex:1;padding:6px;border:1px solid #333;border-radius:6px;background:#16213e;color:#eee}";
  html += ".host-row button{padding:4px 10px;border:none;border-radius:6px;cursor:pointer;font-size:.85em}";
  html += ".del-btn{background:#e94560;color:#fff}.mv-btn{background:#0f3460;color:#ccc}";
  html += ".add-row{display:flex;gap:8px;margin-top:6px}";
  html += ".add-row input{flex:1;padding:6px;border:1px solid #333;border-radius:6px;background:#16213e;color:#eee}";
  html += ".add-btn{padding:6px 14px;background:#0f3460;color:#4fc3f7;border:1px solid #4fc3f7;border-radius:6px;cursor:pointer}";
  html += ".counter{text-align:right;font-size:.8em;color:#555;margin-bottom:6px}";
  html += ".btn-bar{display:flex;gap:12px;margin-top:16px;flex-wrap:wrap}";
  html += ".btn{padding:12px 24px;border:none;border-radius:8px;font-size:1em;font-weight:600;cursor:pointer;transition:background .2s}";
  html += ".btn-primary{background:#e94560;color:#fff;flex:1}.btn-primary:hover{background:#c73553}";
  html += ".btn-secondary{background:#0f3460;color:#ccc;border:1px solid #333}.btn-secondary:hover{background:#1a4a8a}";
  html += ".btn-danger{background:#7f1d1d;color:#fca5a5;border:1px solid #991b1b}.btn-danger:hover{background:#991b1b}";
  html += ".nav{text-align:center;margin-bottom:16px}";
  html += ".nav a{color:#4fc3f7;text-decoration:none;margin:0 10px}.nav a:hover{text-decoration:underline}";
  html += ".msg{text-align:center;padding:10px;border-radius:8px;margin-bottom:12px;display:none}";
  html += ".msg-ok{background:#064e3b;color:#6ee7b7;display:block}.msg-err{background:#7f1d1d;color:#fca5a5;display:block}";
  html += "</style></head><body><div class='wrap'>";
  html += "<h1>&#x2699; Settings</h1>";
  html += "<div class='nav'><a href='/'>Dashboard</a><a href='/settings'>Settings</a><a href='/setup'>WiFi Setup</a></div>";
  html += "<div id='msg' class='msg'></div>";
  html += "<div class='card'><h2>&#x1f4f6; WiFi Configuration</h2>";
  html += "<div class='row'><div class='field'><label>WiFi Network</label><select id='wifi_scan' onchange=\"document.getElementById('wifi_ssid').value=this.value\"><option value=''>-- Scan to list networks --</option></select></div></div>";
  html += "<div class='row'><div class='field"><button class='btn btn-secondary' style='width:auto;padding:8px 16px;margin:0' onclick='scanWifi()'>&#x1f50d; Scan Networks</button></div><div id='scan-spinner' style='display:none;color:#e94560;padding:8px'>Scanning...</div></div>";
  html += "<div class='row'><div class='field'><label>SSID</label><input type='text' id='wifi_ssid' placeholder='Network name'></div>";
  html += "<div class='field'><label>Password</label><input type='password' id='wifi_pass' placeholder='WiFi password'></div></div>";
  html += "<div style='margin-top:12px'><button class='btn btn-danger' onclick='saveWifi()' style='width:auto;padding:10px 20px'>&#x1f4be; Save WiFi &amp; Restart</button>";
  html += "<span style='color:#888;font-size:.8em;margin-left:12px'>Changes require device restart</span></div></div>";
  html += "<div class='card'><h2>NTP Time Sync</h2>";
  html += "<div class='row'><div class='field'><label>NTP Server</label><input type='text' id='ntp_server'></div></div>";
  html += "<div class='row'><div class='field'><label>GMT Offset (sec)</label><input type='number' id='gmt_offset'></div>";
  html += "<div class='field'><label>DST Offset (sec)</label><input type='number' id='dst_offset'></div></div></div>";
  html += "<div class='card'><h2>Silent Hours</h2>";
  html += "<div class='row"><div class='field'><label>Start (0-23)</label><input type='number' id='silent_start' min='0' max='23'></div>";
  html += "<div class='field'><label>End (0-23)</label><input type='number' id='silent_end' min='0' max='23'></div></div></div>";
  html += "<div class='card'><h2>Network Check</h2>";
  html += "<div class='row"><div class='field"><label>Interval (sec, min 10)</label><input type='number' id='check_interval' min='10'></div></div></div>";
  html += "<div class='card' id='hosts-section'></div>";
  html += "<div class='btn-bar">";
  html += "<button class='btn btn-primary' onclick='saveConfig()'>&#x1f4be; Save Settings</button>";
  html += "<button class='btn btn-secondary' onclick="location.href='/'">&#x2302; Dashboard</button>";
  html += "<button class='btn btn-danger' onclick='restart()'>&#x1f504; Restart</button>";
  html += "</div></div><script>";
  html += "var cfg;";
  html += "async function load(){try{var r=await fetch('/api/config');cfg=await r.json();";
  html += "document.getElementById('ntp_server').value=cfg.ntp_server||'';";
  html += "document.getElementById('gmt_offset').value=cfg.gmt_offset||0;";
  html += "document.getElementById('dst_offset').value=cfg.dst_offset||0;";
  html += "document.getElementById('silent_start').value=cfg.silent_start||0;";
  html += "document.getElementById('silent_end').value=cfg.silent_end||0;";
  html += "document.getElementById('check_interval').value=cfg.check_interval||180;";
  html += "document.getElementById('wifi_ssid').value=cfg.wifi_ssid||'';";
  html += "renderHosts(cfg);}catch(e){msg('Load failed: '+e,true);}";
  html += "}";
  html += "function renderHosts(c){var s=document.getElementById('hosts-section');";
  html += "s.innerHTML='<h2>Host Lists</h2>'+buildHosts('Local',c.local_hosts||[],'local')+buildHosts('Whitelist',c.whitelist_hosts||[],'whitelist')+buildHosts('World',c.world_hosts||[],'world');}";
  html += "function buildHosts(title,arr,key){";
  html += "var h='<h3 style=\"color:#4fc3f7;margin:12px 0 8px;font-size:.95em\">'+title+'</h3><div class=\"counter\" id=\"cnt-'+key+'\">'+arr.length+'/10 hosts</div><div class=\"host-list\" id=\"hl-'+key+'\">';";
  html += "arr.forEach(function(name,i){h+='<div class=\"host-row\"><input type=\"text\" value=\"'+name+'\" id=\"h-'+key+'-'+i+'\"><button class=\"mv-btn\" onclick=\"moveHost(\''+key+'\','+i+',-1)\">\u25b2</button><button class=\"mv-btn\" onclick=\"moveHost(\''+key+'\','+i+',1)\">\u25bc</button><button class=\"del-btn\" onclick=\"delHost(\''+key+'\','+i+')\">\u2715</button></div>';});";
  html += "h+='</div><div class=\"add-row\"><input type=\"text\" id=\"new-'+key+'\" placeholder=\"Add host...\" onkeydown=\"if(event.key===\"Enter\")addHost(\''+key+'\')\"><button class=\"add-btn\" onclick=\"addHost(\''+key+'\')\">+ Add</button></div>';";
  html += "return h;}";
  html += "function getHosts(key){var a=[];var el=document.getElementById('hl-'+key);var inputs=el.querySelectorAll('input');for(var i=0;i<inputs.length;i++){var v=inputs[i].value.trim();if(v)a.push(v);}return a;}";
  html += "function addHost(key){var hosts=getHosts(key);if(hosts.length>=10){alert('Max 10 hosts');return;}";
  html += "var inp=document.getElementById('new-'+key);var v=inp.value.trim();if(!v)return;";
  html += "if(cfg)cfg[key+'_hosts']=hosts.concat([v]);inp.value='';renderHosts(cfg);}";
  html += "function delHost(key,idx){var hosts=getHosts(key);hosts.splice(idx,1);if(cfg)cfg[key+'_hosts']=hosts;renderHosts(cfg);}";
  html += "function moveHost(key,idx,dir){var hosts=getHosts(key);var ni=idx+dir;if(ni<0||ni>=hosts.length)return;";
  html += "var t=hosts[idx];hosts[idx]=hosts[ni];hosts[ni]=t;if(cfg)cfg[key+'_hosts']=hosts;renderHosts(cfg);}";
  html += "function msg(text,err){var el=document.getElementById('msg');el.textContent=text;el.className='msg '+(err?'msg-err':'msg-ok');setTimeout(function(){el.style.display='none';},3000);el.style.display='block';}";
  html += "async function saveConfig(){";
  html += "var hosts={local:getHosts('local'),whitelist:getHosts('whitelist'),world:getHosts('world')};";
  html += "var body={";
  html += "ntp_server:document.getElementById('ntp_server').value,";
  html += "gmt_offset:parseInt(document.getElementById('gmt_offset').value)||0,";
  html += "dst_offset:parseInt(document.getElementById('dst_offset').value)||0,";
  html += "silent_start:parseInt(document.getElementById('silent_start').value)||0,";
  html += "silent_end:parseInt(document.getElementById('silent_end').value)||0,";
  html += "check_interval:Math.max(10,parseInt(document.getElementById('check_interval').value)||180),";
  html += "local_hosts:hosts.local,whitelist_hosts:hosts.whitelist,world_hosts:hosts.world";
  html += "};";
  html += "try{var r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});";
  html += "var d=await r.json();if(d.ok)msg('Settings saved!');else msg('Error: '+(d.error||'unknown'),true);}";
  html += "catch(e){msg('Save failed: '+e,true);}}";
  html += "async function scanWifi(){";
  html += "document.getElementById('scan-spinner').style.display='inline-block';";
  html += "var sel=document.getElementById('wifi_scan');sel.innerHTML='<option>Scanning...</option>';";
  html += "try{var r=await fetch('/scan');var n=await r.json();";
  html += "sel.innerHTML='<option value=\"\">-- Select network --</option>';";
  html += "n.forEach(function(x){var o=document.createElement('option');o.value=x.ssid;";
  html += "o.textContent=x.ssid+' ('+x.rssi+' dBm)'+(x.enc?' \u{1f512}':'');sel.appendChild(o);});}";
  html += "catch(e){sel.innerHTML='<option>Scan failed</option>';}";
  html += "document.getElementById('scan-spinner').style.display='none';}";
  html += "async function saveWifi(){";
  html += "var s=document.getElementById('wifi_ssid').value.trim();";
  html += "var p=document.getElementById('wifi_pass').value;";
  html += "if(!s){alert('Enter SSID!');return;}";
  html += "if(!confirm('Save WiFi and restart? Device will reboot.'))return;";
  html += "try{await fetch('/savewifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,pass:p})});";
  html += "msg('WiFi saved! Restarting...');}catch(e){msg('WiFi save sent, restarting...');}}";
  html += "async function restart(){if(!confirm('Restart device?'))return;";
  html += "try{await fetch('/restart');msg('Restarting...');}catch(e){msg('Restart sent');}}";
  html += "load();</script></body></html>";
  server.send(200, "text/html", html);
}

// GET /api/config — возвращает текущую конфигурацию в JSON
void handleApiConfigGet() {
  String json = "{";
  json += "\"wifi_ssid\":\"" + String(wifiSsid) + "\",";
  json += "\"ntp_server\":\"" + String(cfgNtpServer) + "\",";
  json += "\"gmt_offset\":" + String(cfgGmtOffset) + ",";
  json += "\"dst_offset\":" + String(cfgDstOffset) + ",";
  json += "\"silent_start\":" + String(cfgSilentStart) + ",";
  json += "\"silent_end\":" + String(cfgSilentEnd) + ",";
  json += "\"check_interval\":" + String(checkInterval) + ",";
  json += "\"local_hosts\":[";
  for (int i = 0; i < cfgLocalCount; i++) { if (i > 0) json += ","; json += "\"" + cfgLocalHosts[i] + "\""; }
  json += "],\"whitelist_hosts\":[";
  for (int i = 0; i < cfgWhitelistCount; i++) { if (i > 0) json += ","; json += "\"" + cfgWhitelistHosts[i] + "\""; }
  json += "],\"world_hosts\":[";
  for (int i = 0; i < cfgWorldCount; i++) { if (i > 0) json += ","; json += "\"" + cfgWorldHosts[i] + "\""; }
  json += "]}";
  server.send(200, "application/json", json);
}

// POST /api/config — сохраняет конфигурацию из JSON-тела
void handleApiConfigPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}");
    return;
  }
  String body = server.arg("plain");
  String val;

  val = jsonGetString(body, "ntp_server");
  if (val.length() > 0) { free((void*)cfgNtpServer); cfgNtpServer = strdup(val.c_str()); }

  int n = jsonGetInt(body, "gmt_offset", cfgGmtOffset);
  cfgGmtOffset = n;

  n = jsonGetInt(body, "dst_offset", cfgDstOffset);
  cfgDstOffset = n;

  n = jsonGetInt(body, "silent_start", cfgSilentStart);
  cfgSilentStart = n;

  n = jsonGetInt(body, "silent_end", cfgSilentEnd);
  cfgSilentEnd = n;

  n = jsonGetInt(body, "check_interval", checkInterval);
  if (n >= 10) checkInterval = n;

  // Парсим массивы хостов
  int arrStart = body.indexOf("\"local_hosts\":[");
  if (arrStart != -1) {
    arrStart += 14;
    int arrEnd = body.indexOf("]", arrStart);
    String arr = body.substring(arrStart, arrEnd);
    cfgLocalCount = 0;
    int pos = 0;
    while (pos < (int)arr.length() && cfgLocalCount < MAX_HOSTS) {
      int q1 = arr.indexOf('"', pos);
      if (q1 == -1) break;
      int q2 = arr.indexOf('"', q1 + 1);
      if (q2 == -1) break;
      cfgLocalHosts[cfgLocalCount] = arr.substring(q1 + 1, q2);
      cfgLocalCount++;
      pos = q2 + 1;
    }
  }

  arrStart = body.indexOf("\"whitelist_hosts\":[");
  if (arrStart != -1) {
    arrStart += 19;
    int arrEnd = body.indexOf("]", arrStart);
    String arr = body.substring(arrStart, arrEnd);
    cfgWhitelistCount = 0;
    int pos = 0;
    while (pos < (int)arr.length() && cfgWhitelistCount < MAX_HOSTS) {
      int q1 = arr.indexOf('"', pos);
      if (q1 == -1) break;
      int q2 = arr.indexOf('"', q1 + 1);
      if (q2 == -1) break;
      cfgWhitelistHosts[cfgWhitelistCount] = arr.substring(q1 + 1, q2);
      cfgWhitelistCount++;
      pos = q2 + 1;
    }
  }

  arrStart = body.indexOf("\"world_hosts\":[");
  if (arrStart != -1) {
    arrStart += 15;
    int arrEnd = body.indexOf("]", arrStart);
    String arr = body.substring(arrStart, arrEnd);
    cfgWorldCount = 0;
    int pos = 0;
    while (pos < (int)arr.length() && cfgWorldCount < MAX_HOSTS) {
      int q1 = arr.indexOf('"', pos);
      if (q1 == -1) break;
      int q2 = arr.indexOf('"', q1 + 1);
      if (q2 == -1) break;
      cfgWorldHosts[cfgWorldCount] = arr.substring(q1 + 1, q2);
      cfgWorldCount++;
      pos = q2 + 1;
    }
  }

  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

// GET /restart — перезагрузка устройства
void handleRestart() {
  server.send(200, "application/json", "{\"ok\":true,\"message\":\"restarting\"}");
  delay(500);
  ESP.restart();
}

// Страница настройки WiFi (режим точки доступа)
void handleSetupPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>WiFi Setup</title><style>";
  html += "body{font-family:'Segoe UI',Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:20px;display:flex;justify-content:center;align-items:center;min-height:100vh}";
  html += ".card{background:#16213e;border-radius:12px;padding:30px;max-width:420px;width:100%;box-shadow:0 8px 32px rgba(0,0,0,.3)}";
  html += "h1{text-align:center;color:#e94560;margin-bottom:5px;font-size:1.4em}";
  html += ".sub{text-align:center;color:#888;font-size:.85em;margin-bottom:20px}";
  html += "label{display:block;margin-top:15px;font-weight:600;color:#aaa;font-size:.85em;text-transform:uppercase;letter-spacing:1px}";
  html += "select,input[type=text],input[type=password]{width:100%;padding:10px;margin-top:5px;border:1px solid #333;border-radius:8px;background:#0f3460;color:#eee;font-size:1em;box-sizing:border-box}";
  html += "select:focus,input:focus{outline:none;border-color:#e94560}";
  html += "button{width:100%;padding:12px;margin-top:20px;border:none;border-radius:8px;background:#e94560;color:#fff;font-size:1.1em;font-weight:600;cursor:pointer;transition:background .2s}";
  html += "button:hover{background:#c73553}";
  html += ".scan-btn{background:#0f3460;margin-top:10px}.scan-btn:hover{background:#1a4a8a}";
  html += ".info{text-align:center;color:#555;font-size:.75em;margin-top:15px}";
  html += ".spin{display:none;text-align:center;margin-top:10px;color:#e94560}";
  html += "</style></head><body><div class='card'>";
  html += "<h1>&#x1f4f6; WiFi Setup</h1><p class='sub'>Network Monitor &mdash; настройка подключения</p>";
  html += "<label>Доступные сети</label>";
  html += "<select id='ssidSel' onchange=\"document.getElementById('ssid').value=this.value\">";
  html += "<option value=''>-- Нажмите Scan --</option></select>";
  html += "<button class='scan-btn' onclick='scanWifi()'>&#x1f50d; Scan</button>";
  html += "<div id='spinner' class='spin'>Сканирование...</div>";
  html += "<label>SSID</label><input type='text' id='ssid' placeholder='Имя сети'>";
  html += "<label>Пароль</label><input type='password' id='pass' placeholder='Пароль WiFi'>";
  html += "<button onclick='saveWifi()'>&#x1f4be; Сохранить и перезагрузить</button>";
  html += "<p class='info'>После сохранения устройство перезагрузится<br>и попытается подключиться к указанной сети.</p>";
  html += "</div><script>";
  html += "async function scanWifi(){";
  html += "document.getElementById('spinner').style.display='block';";
  html += "document.getElementById('ssidSel').innerHTML='<option>Сканирование...</option>';";
  html += "try{const r=await fetch('/scan');const n=await r.json();";
  html += "const s=document.getElementById('ssidSel');s.innerHTML='<option value=\"\">-- Выберите сеть --</option>';";
  html += "n.forEach(x=>{const o=document.createElement('option');o.value=x.ssid;";
  html += "o.textContent=x.ssid+' ('+x.rssi+' dBm)'+(x.enc?' &#x1f512;':'');s.appendChild(o);});}";
  html += "catch(e){document.getElementById('ssidSel').innerHTML='<option>Ошибка</option>';}";
  html += "document.getElementById('spinner').style.display='none';}";
  html += "async function saveWifi(){";
  html += "const s=document.getElementById('ssid').value.trim();";
  html += "const p=document.getElementById('pass').value;";
  html += "if(!s){alert('Введите SSID!');return;}";
  html += "try{await fetch('/savewifi',{method:'POST',headers:{'Content-Type':'application/json'},";
  html += "body:JSON.stringify({ssid:s,pass:p})});alert('Сохранено! Перезагрузка...');}";
  html += "catch(e){alert('Ошибка сохранения');}}";
  html += "</script></body></html>";
  server.send(200, "text/html", html);
}

// Сканирование доступных WiFi-сетей (JSON)
void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"enc\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? 1 : 0) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

// Сохранение новых WiFi-настроек и перезагрузка устройства
void handleSaveWifi() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    // Парсим JSON вручную (без ArduinoJson)
    int ssidStart = body.indexOf("\"ssid\":\"") + 8;
    int ssidEnd = body.indexOf("\"", ssidStart);
    int passStart = body.indexOf("\"pass\":\"") + 8;
    int passEnd = body.indexOf("\"", passStart);

    if (ssidStart > 7 && ssidEnd > ssidStart && passStart > 7 && passEnd > passStart) {
      String newSsid = body.substring(ssidStart, ssidEnd);
      String newPass = body.substring(passStart, passEnd);

      char ssidBuf[33];
      char passBuf[65];
      strncpy(ssidBuf, newSsid.c_str(), 32);
      ssidBuf[32] = '\0';
      strncpy(passBuf, newPass.c_str(), 64);
      passBuf[64] = '\0';

      saveWifiConfig(ssidBuf, passBuf);
      server.send(200, "application/json", "{\"ok\":true}");

      delay(1000);
      ESP.restart();
      return;
    }
  }
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid request\"}");
}

// Запуск режима точки доступа (AP) для настройки WiFi
void startAPMode() {
  apMode = true;
  Serial.println("Starting AP mode...");
  beepWifiFail();
  lcd.clear();
  lcd.print("AP: NetworkMon");
  lcd.setCursor(0, 1);
  lcd.print("192.168.4.1");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(500);
  Serial.printf("AP SSID: %s\n", AP_SSID);
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleSetupPage);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.onNotFound(handleSetupPage);
  server.begin();
  Serial.println("AP Web Server Started");
}

// -------------------------------------------------------------------
// WEB HANDLERS (с AJAX, без перезагрузки)
// -------------------------------------------------------------------
// Корневая страница веб-интерфейса: статус, время, кнопки управления, история
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
  html += "<p><button onclick='clearLogs()'>Clear Logs</button> <button onclick='runTest()'>Run test now</button> <button onclick='tetris()'>Tetris Melody</button> <button onclick=\"location.href='/settings'\">Settings</button> <button onclick=\"location.href='/setup'\">WiFi Setup</button></p>";
  html += "<h2>History</h2>";
  html += getHistoryHTML();
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// Эндпоинт /now — возвращает текущее серверное время в JSON
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

// Эндпоинт /testprogress — возвращает прогресс текущего теста (inProgress, currentHost, lastResult)
void handleTestProgress() {
  String json = "{\"inProgress\":" + String(testingInProgress ? "true" : "false") + ",\"currentHost\":\"" + currentTestingHost + "\",\"lastResult\":\"" + lastTestResult + "\",\"lastCheck\":" + String(lastCheckTime / 1000) + "}";
  server.send(200, "application/json", json);
}

// Эндпоинт /status — возвращает текущий статус сети и аптайм в JSON
void handleStatus() {
  String statusStr;
  switch (currentStatus) { case 0: statusStr = "OK"; break; case 1: statusStr = "Network Problem"; break; case 2: statusStr = "Dead Internet"; break; case 3: statusStr = "Whitelisted"; break; case 4: statusStr = "Anomaly"; break; }
  String json = "{\"status\":\"" + statusStr + "\",\"uptime\":\"" + String((millis() / 1000 / 60 / 60) % 24) + "h\"}";
  server.send(200, "application/json", json);
}

// Эндпоинт /clear — сбрасывает счётчик результатов, очищает историю
void handleClear() {
  resultCount = 0;
  saveToFile();
  historyCount = 0;
  historyChanged = true;
  saveHistory();
  server.send(200, "text/plain", "Cleared");
}

// Эндпоинт /refresh — запускает внеочередную проверку сети с анимацией
void handleRefresh() {
  runTestWithAnimation();
  server.send(200, "text/plain", "Test completed");
}

// Эндпоинт /history — возвращает историю проверок в формате JSON
void handleHistory() {
  server.send(200, "application/json", getHistoryJSON());
}

// Эндпоинт /tetris — проигрывает мелодию Tetris через динамик
void handleTetris() {
  // Проигрываем мелодию Tetris через динамик (короткая версия)
  // Не блокируем, запускаем в отдельной функции
  playTetrisMelody();
  server.send(200, "text/plain", "Tetris!");
}

// Обработчик 404 — возвращает "Not Found" для всех неизвестных маршрутов
void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

// -------------------------------------------------------------------
// АНИМАЦИЯ ПОВЕРХ ЧАСОВ (не стирает экран)
// -------------------------------------------------------------------
// Показывает анимацию теста в углах дисплея (не стирает часы)
// Бегающие символы * - # + в течение ~1.6 с, затем стираются
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
// Пытается подключиться к хосту по портам 80 (HTTP) и 443 (HTTPS)
// Возвращает true, если хотя бы один порт открыт
bool ping(const char* host) {
  WiFiClient client;
  IPAddress ip;
  if (!WiFi.hostByName(host, ip)) return false;
  if (client.connect(ip, 80)) { client.stop(); return true; }
  client.stop();
  if (client.connect(ip, 443)) { client.stop(); return true; }
  return false;
}

// Отправляет HTTP GET-запрос к хосту и проверяет код ответа (2xx или 3xx)
// Возвращает true при успешном HTTP-ответе
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

// Сбрасывает флаги pinged, httpChecked и pingTime для всех списков хостов
// Восстанавливает массивы CheckItem из конфигурируемых строковых массивов
void rebuildCheckItems() {
  for (int i = 0; i < cfgLocalCount; i++) {
    localList[i].name = cfgLocalHosts[i].c_str();
    localList[i].isIp = true;
    localList[i].pinged = false;
    localList[i].httpChecked = false;
    localList[i].pingTime = 0;
  }
  for (int i = 0; i < cfgWhitelistCount; i++) {
    whitelistList[i].name = cfgWhitelistHosts[i].c_str();
    whitelistList[i].isIp = false;
    whitelistList[i].pinged = false;
    whitelistList[i].httpChecked = false;
    whitelistList[i].pingTime = 0;
  }
  for (int i = 0; i < cfgWorldCount; i++) {
    worldList[i].name = cfgWorldHosts[i].c_str();
    worldList[i].isIp = false;
    worldList[i].pinged = false;
    worldList[i].httpChecked = false;
    worldList[i].pingTime = 0;
  }
}

void resetFlags() {
  for (int i = 0; i < cfgLocalCount; i++) { localList[i].pinged = false; localList[i].httpChecked = false; localList[i].pingTime = 0; }
  for (int i = 0; i < cfgWhitelistCount; i++) { whitelistList[i].pinged = false; whitelistList[i].httpChecked = false; whitelistList[i].pingTime = 0; }
  for (int i = 0; i < cfgWorldCount; i++) { worldList[i].pinged = false; worldList[i].httpChecked = false; worldList[i].pingTime = 0; }
}

// Проверяет список хостов: для каждого выполняет ping и (если не IP) HTTP-проверку
// Сохраняет результаты через saveTestResult, обновляет currentTestingHost/lastTestResult
void checkHosts(CheckItem* list, int count) {
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
    delay(100);
  }
}

// Проверяет, наступил ли "тихий час" (промежуток между SILENT_START_HOUR и SILENT_END_HOUR)
// В тихий час динамик не пищит
bool isSilentHour() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  int hour = timeinfo.tm_hour;    if (cfgSilentStart < cfgSilentEnd) {
    return (hour >= cfgSilentStart && hour < cfgSilentEnd);
  } else {
    return (hour >= cfgSilentStart || hour < cfgSilentEnd);
  }
}

// Оценивает общий статус сети на основе результатов проверки всех списков хостов
// Определяет статусы: 0=OK, 1=Network Problem, 2=Dead Internet, 3=Whitelisted, 4=Anomaly
// При изменении статуса включает мелодию, мигание подсветки, обновляет дисплей
void evaluateStatus() {
  bool localOk = cfgLocalCount > 0 && localList[0].pinged;
  whitelistOk = true;
  for (int i = 0; i < cfgWhitelistCount; i++) if (!whitelistList[i].pinged && !whitelistList[i].httpChecked) { whitelistOk = false; break; }
  worldOk = true;
  for (int i = 0; i < cfgWorldCount; i++) if (!worldList[i].pinged && !worldList[i].httpChecked) { worldOk = false; break; }
  
  unsigned long newStatus;
  if (!localOk) newStatus = 1;
  else if (localOk && !whitelistOk && !worldOk) newStatus = 2;
  else if (localOk && whitelistOk && !worldOk) newStatus = 3;
  else if (!whitelistOk || !worldOk) newStatus = 4;
  else newStatus = 0;
  
  if (newStatus != 0) addHistoryEntry(newStatus);

  if (newStatus != currentStatus) {
    currentStatus = newStatus;
    if (currentStatus != 0) {
      lcd.clear();
      melodyStartTime = millis();
      melodyEnabled = true;
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
      lcd.setBacklight(255);
      backlightOn = true;
      lastClockUpdate = millis();
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) updateClockDisplay(&timeinfo);
      else lcd.print("Time error");
    }
    saveToFile();
  }
}

// Обновляет дисплей: часы в формате ЧЧ:ММ (с мигающим двоеточием) и дату
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

// Запускает полный цикл проверки: анимация → сброс флагов → проверка всех списков → оценка статуса
// После теста обновляет дисплей в зависимости от результата
void runTestWithAnimation() {
  testingInProgress = true;
  showTestAnimation();
  rebuildCheckItems();
  checkHosts(localList, cfgLocalCount);
  checkHosts(whitelistList, cfgWhitelistCount);
  checkHosts(worldList, cfgWorldCount);
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
// Короткий звуковой сигнал при запуске (две ноты восходящие)
void beepStartup() { tone(SPEAKER_PIN, 1000, 100); delay(50); tone(SPEAKER_PIN, 1500, 100); }
// Сигнал успешного подключения к WiFi (три восходящие ноты)
void beepWifiSuccess() { tone(SPEAKER_PIN, 800, 100); delay(50); tone(SPEAKER_PIN, 1200, 100); delay(50); tone(SPEAKER_PIN, 1600, 100); }
// Сигнал ошибки подключения к WiFi (два низких гудка)
void beepWifiFail() { tone(SPEAKER_PIN, 300, 300); delay(100); tone(SPEAKER_PIN, 300, 300); }

// Проигрывает мелодию, соответствующую текущему статусу ошибки
// Работает только 10 секунд после смены статуса, не играет в тихий час
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

// Проигрывает мелодию Tetris через динамик (блокирующая, ~30 секунд)
// Не играет в тихий час
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
// Синхронизирует время по NTP, если прошло больше NTP_SYNC_INTERVAL с последней синхронизации
// Ждёт ответа до 5 секунд (20 попыток по 250 мс)
void syncTimeIfNeeded() {
  unsigned long nowMs = millis();
  if (lastNtpSync == 0 || (nowMs - lastNtpSync > NTP_SYNC_INTERVAL)) {
    Serial.println("Syncing time with NTP...");
    configTime(cfgGmtOffset, cfgDstOffset, cfgNtpServer);
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
// Инициализация: Serial, динамик, дисплей (I2C), LittleFS, загрузка истории, WiFi, веб-сервер, NTP
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
  
  loadWifiConfig();
  beepStartup();
  WiFi.begin(wifiSsid, wifiPass);
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
    WiFi.setAutoReconnect(true);
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
    server.on("/setup", handleSetupPage);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/savewifi", HTTP_POST, handleSaveWifi);
    server.on("/settings", handleSettingsPage);
    server.on("/api/config", HTTP_GET, handleApiConfigGet);
    server.on("/api/config", HTTP_POST, handleApiConfigPost);
    server.on("/restart", handleRestart);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("Web Server Started");
    
    loadConfig();
    syncTimeIfNeeded();
    
    lastCheckTime = millis() - (unsigned long)checkInterval * 1000 + 15000;
  } else {
    Serial.println("WiFi Connect Failed!");
    lcd.clear(); lcd.print("No WiFi!");
    startAPMode();
  }
}

// -------------------------------------------------------------------
// LOOP
// -------------------------------------------------------------------
// Главный цикл: обработка веб-запросов, синхронизация времени,
// периодическая проверка сети (каждые 3 минуты), обновление часов/дисплея
void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }
  
  server.handleClient();
  syncTimeIfNeeded();
  
  unsigned long currentMillis = millis();
  if (currentMillis - lastCheckTime > (unsigned long)checkInterval * 1000) {
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

  // Проверка WiFi-соединения — если пропало на 30 секунд, переходим в AP-режим
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiReconnectStart == 0) {
      wifiReconnectStart = currentMillis;
      Serial.println("WiFi lost, waiting to reconnect...");
    } else if (currentMillis - wifiReconnectStart > 30000) {
      Serial.println("WiFi reconnect timeout, entering AP mode...");
      startAPMode();
      return;
    }
  } else {
    wifiReconnectStart = 0;
  }
}
