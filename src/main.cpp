#include <Arduino.h>
/*
   WiFi Modem для ESP32-S3
   Упрощенная версия без аппаратных сигналов модема
   Только RX/TX через USB Serial
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// тач пины
#define TOUCH1 8
#define TOUCH2 35

// Определение пинов для дисплея
#define TFT_SCL 19
#define TFT_SDA 9
#define TFT_RS  20
#define TFT_CS  47
#define TFT_RES 48
#define TFT_BLK 35

#define TFT_H 80
#define TFT_W 160

// Определение пинов для I2S
#define I2S_WS 16
#define I2S_SD 15
#define I2S_SCK 37
#define I2S_PORT I2S_NUM_0

// Настройка пинов для ESP32-S3
#define LORA_NSS 38
#define LORA_DIO0 42
#define LORA_RST 36
#define LORA_SCK 39
#define LORA_MISO 40
#define LORA_MOSI 1
#define LORA_DIO1 2

#define LED_PIN 6

// Версия прошивки
#define FIRMWARE_VERSION "1.0-ESP32S3"
#define BUILD_DATE __DATE__ " " __TIME__

// Настройки по умолчанию
#define DEFAULT_BAUD 115200
#define LISTEN_PORT 6400
#define MAX_CMD_LENGTH 256
#define TX_BUF_SIZE 256


// Глобальные переменные
WebServer webServer(80);
WiFiClient tcpClient;
WiFiServer tcpServer(LISTEN_PORT);
Preferences preferences;

String cmd = "";
bool cmdMode = true;
bool callConnected = false;
bool telnet = false;
bool verboseResults = true;
bool echo = true;
bool autoAnswer = false;
bool petTranslate = false;
String ssid = "*******";
String password = "******";
String busyMsg = "SORRY, SYSTEM IS BUSY. PLEASE TRY AGAIN LATER.";
String speedDials[10];
int currentBaudRate = DEFAULT_BAUD;
unsigned long connectTime = 0;

// Доступные скорости
const long baudRates[] = {300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};

// Результаты AT команд
const String resultCodes[] = {"OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "", "NO DIALTONE", "BUSY", "NO ANSWER"};
enum ResultCode {A_OK,A_CONNECT, A_RING, A_NOCARRIER, A_ERROR, A_NONE, A_NODIALTONE, A_BUSY, A_NOANSWER};

String connectTimeString() {
  if (connectTime == 0) return "00:00:00";
  unsigned long now = millis();
  int secs = (now - connectTime) / 1000;
  int mins = secs / 60;
  int hours = mins / 60;
  char buffer[10];
  sprintf(buffer, "%02d:%02d:%02d", hours % 100, mins % 60, secs % 60);
  return String(buffer);
}

void sendResult(ResultCode result) {
  Serial.print("\r\n");
  if (result == A_CONNECT) {
    Serial.print("CONNECT ");
    Serial.println(currentBaudRate);
  } else if (result == A_NOCARRIER) {
    Serial.print("NO CARRIER (");
    Serial.print(connectTimeString());
    Serial.println(")");
  } else {
    Serial.println(resultCodes[result]);
  }
  Serial.print("\r\n");
}

void sendString(const String& msg) {
  Serial.print("\r\n");
  Serial.println(msg);
  Serial.print("\r\n");
}

void updateLed() {
  if (callConnected) {
    digitalWrite(LED_PIN, LOW);   // Горит при соединении
  } else if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, LOW);   // Горит при подключении к WiFi
  } else {
    digitalWrite(LED_PIN, HIGH);  // Не горит
  }
}

void loadSettings() {
  preferences.begin("wifi-modem", true); // true = read-only
  
  ssid = preferences.getString("ssid", "******");
  password = preferences.getString("pass", "******");
  busyMsg = preferences.getString("busymsg", "SORRY, SYSTEM IS BUSY. PLEASE TRY AGAIN LATER.");
  currentBaudRate = preferences.getInt("baud", DEFAULT_BAUD);
  echo = preferences.getBool("echo", true);
  autoAnswer = preferences.getBool("autoanswer", false);
  telnet = preferences.getBool("telnet", false);
  verboseResults = preferences.getBool("verbose", true);
  petTranslate = preferences.getBool("petscii", false);
  
  // Загрузка быстрых номеров
  for (int i = 0; i < 10; i++) {
    char key[10];
    sprintf(key, "speed%d", i);
    speedDials[i] = preferences.getString(key, "");
  }
  
  preferences.end();
}

void saveSettings() {
  preferences.begin("wifi-modem", false); // false = read-write
  
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.putString("busymsg", busyMsg);
  preferences.putInt("baud", currentBaudRate);
  preferences.putBool("echo", echo);
  preferences.putBool("autoanswer", autoAnswer);
  preferences.putBool("telnet", telnet);
  preferences.putBool("verbose", verboseResults);
  preferences.putBool("petscii", petTranslate);
  
  // Сохранение быстрых номеров
  for (int i = 0; i < 10; i++) {
    char key[10];
    sprintf(key, "speed%d", i);
    preferences.putString(key, speedDials[i]);
  }
  
  preferences.end();
  Serial.println("Settings saved to NVRAM");
}

void factoryReset() {
  preferences.begin("wifi-modem", false);
  preferences.clear();
  preferences.end();
  
  ssid = "";
  password = "";
  busyMsg = "SORRY, SYSTEM IS BUSY. PLEASE TRY AGAIN LATER.";
  currentBaudRate = DEFAULT_BAUD;
  echo = true;
  autoAnswer = false;
  telnet = false;
  verboseResults = true;
  petTranslate = false;
  
  for (int i = 0; i < 10; i++) {
    speedDials[i] = "";
  }
  speedDials[0] = "bbs.fozztexx.com:23";
  speedDials[1] = "cottonwoodbbs.dyndns.org:6502";
  
  Serial.println("Factory defaults restored");
}

void connectWiFi() {
  if (ssid.length() == 0) {
    Serial.println("ERROR: SSID not configured. Use AT$SSID=your_ssid");
    return;
  }
  
  Serial.print("CONNECTING TO ");
  Serial.println(ssid);
  
  WiFi.begin(ssid.c_str(), password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Мигаем при подключении
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("CONNECTED TO ");
    Serial.println(WiFi.SSID());
    Serial.print("IP ADDRESS: ");
    Serial.println(WiFi.localIP());
    updateLed();
    sendResult(A_OK);
  } else {
    Serial.println("CONNECTION FAILED");
    updateLed();
    sendResult(A_ERROR);
  }
}

void disconnectWiFi() {
  WiFi.disconnect();
  Serial.println("WIFI DISCONNECTED");
  updateLed();
  sendResult(A_OK);
}

void showNetworkInfo() {
  Serial.println("=== NETWORK STATUS ===");
  
  Serial.print("WIFI: ");
  switch (WiFi.status()) {
    case WL_CONNECTED: Serial.println("CONNECTED"); break;
    case WL_NO_SSID_AVAIL: Serial.println("SSID NOT FOUND"); break;
    case WL_CONNECT_FAILED: Serial.println("CONNECTION FAILED"); break;
    case WL_IDLE_STATUS: Serial.println("IDLE"); break;
    case WL_DISCONNECTED: Serial.println("DISCONNECTED"); break;
    default: Serial.println("UNKNOWN"); break;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("SSID: "); Serial.println(WiFi.SSID());
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    Serial.print("RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  }
  
  Serial.print("CALL STATUS: ");
  if (callConnected) {
    Serial.print("CONNECTED TO ");
    Serial.println(tcpClient.remoteIP());
    Serial.print("DURATION: ");
    Serial.println(connectTimeString());
  } else {
    Serial.println("NOT CONNECTED");
  }
  
  Serial.println("=====================");
}

void showSettings() {
  Serial.println("=== CURRENT SETTINGS ===");
  Serial.print("BAUD: "); Serial.println(currentBaudRate);
  Serial.print("SSID: "); Serial.println(ssid);
  Serial.print("PASS: "); Serial.println("********"); // Не показываем пароль
  Serial.print("BUSY MSG: "); Serial.println(busyMsg);
  Serial.print("ECHO: "); Serial.println(echo ? "ON" : "OFF");
  Serial.print("VERBOSE: "); Serial.println(verboseResults ? "ON" : "OFF");
  Serial.print("TELNET: "); Serial.println(telnet ? "ON" : "OFF");
  Serial.print("PETSCII: "); Serial.println(petTranslate ? "ON" : "OFF");
  Serial.print("AUTO ANSWER: "); Serial.println(autoAnswer ? "ON" : "OFF");
  
  Serial.println("SPEED DIAL:");
  for (int i = 0; i < 10; i++) {
    if (speedDials[i].length() > 0) {
      Serial.printf("%d: %s\r\n", i, speedDials[i].c_str());
    }
  }
  Serial.println("=====================");
}

void showHelp() {
  Serial.println("=== AT COMMANDS ===");
  Serial.println("AT              - Test command");
  Serial.println("ATDT host:port  - Dial host (ATDT google.com:80)");
  Serial.println("ATDS n          - Speed dial (n=0-9)");
  Serial.println("ATH             - Hang up");
  Serial.println("ATO             - Go online");
  Serial.println("ATZ             - Reload settings");
  Serial.println("AT&W            - Save settings");
  Serial.println("AT&F            - Factory reset");
  Serial.println("AT&V            - View settings");
  Serial.println("AT?             - This help");
  Serial.println("ATI             - Network info");
  Serial.println("ATE0/ATE1       - Echo off/on");
  Serial.println("ATV0/ATV1       - Verbose off/on");
  Serial.println("ATS0=0/ATS0=1   - Auto answer off/on");
  Serial.println("ATNET0/ATNET1   - Telnet off/on");
  Serial.println("ATPET0/ATPET1   - PETSCII translate off/on");
  Serial.println("ATC0/ATC1       - WiFi off/on");
  Serial.println("AT$SSID=xxx     - Set WiFi SSID");
  Serial.println("AT$PASS=xxx     - Set WiFi password");
  Serial.println("AT$SB=nnnn      - Set baud rate");
  Serial.println("AT$BM=message   - Set busy message");
  Serial.println("AT&Zn=host:port - Set speed dial (n=0-9)");
  Serial.println("AT$RB           - Reboot ESP32");
  Serial.println("=================");
}

void hangUp() {
  if (tcpClient.connected()) {
    tcpClient.stop();
  }
  callConnected = false;
  connectTime = 0;
  updateLed();
  sendResult(A_NOCARRIER);
}

void answerCall() {
  if (!tcpServer.hasClient()) return;
  
  tcpClient = tcpServer.available();
  tcpClient.setNoDelay(true);
  
  callConnected = true;
  connectTime = millis();
  cmdMode = false;
  updateLed();
  sendResult(A_CONNECT);
  Serial.flush();
}

void handleIncomingCall() {
  if (!tcpServer.hasClient()) return;
  
  if (callConnected) {
    // Уже в разговоре - сообщаем "занято"
    WiFiClient busyClient = tcpServer.available();
    busyClient.println(busyMsg);
    busyClient.println("CURRENT CALL: " + connectTimeString());
    busyClient.stop();
    return;
  }
  
  if (autoAnswer) {
    answerCall();
  } else {
    // Звоним
    static unsigned long lastRing = 0;
    if (millis() - lastRing > 3000) {
      sendResult(A_RING);
      lastRing = millis();
    }
  }
}

void dialOut(String upCmd) {
  // Can't place a call while in a call
  if (callConnected) {
    sendResult(A_ERROR);
    return;
  }
  String host, port;
  int portIndex;
  // Dialing a stored number
  if (upCmd.indexOf("ATDS") == 0) {
    byte speedNum = upCmd.substring(4, 5).toInt();
    portIndex = speedDials[speedNum].indexOf(':');
    if (portIndex != -1) {
      host = speedDials[speedNum].substring(0, portIndex);
      port = speedDials[speedNum].substring(portIndex + 1);
    } else {
      port = "23";
    }
  } else {
    // Dialing an ad-hoc number
    int portIndex = cmd.indexOf(":");
    if (portIndex != -1)
    {
      host = cmd.substring(4, portIndex);
      port = cmd.substring(portIndex + 1, cmd.length());
    }
    else
    {
      host = cmd.substring(4, cmd.length());
      port = "23"; // Telnet default
    }
  }
  host.trim(); // remove leading or trailing spaces
  port.trim();
  Serial.print("DIALING "); Serial.print(host); Serial.print(":"); Serial.println(port);
  char *hostChr = new char[host.length() + 1];
  host.toCharArray(hostChr, host.length() + 1);
  int portInt = port.toInt();
  tcpClient.setNoDelay(true); // Try to disable naggle
  if (tcpClient.connect(hostChr, portInt))
  {
    tcpClient.setNoDelay(true); // Try to disable naggle
    sendResult(A_CONNECT);
    connectTime = millis();
    cmdMode = false;
    Serial.flush();
    callConnected = true;
   // setCarrier(callConnected);
    //if (tcpServerPort > 0) tcpServer.stop();
  }
  else
  {
    sendResult(A_NOANSWER);
    callConnected = false;
   // setCarrier(callConnected);
  }
  delete hostChr;
}

void processCommand() {
  cmd.trim();
  if (cmd.length() == 0) return;
  
  String upCmd = cmd;
  upCmd.toUpperCase();
  Serial.println();
  
  // === БАЗОВЫЕ КОМАНДЫ ===
  if (upCmd == "AT") sendResult(A_OK);

  // === DIAL ===
  else if ((upCmd.indexOf("ATDT") == 0) || (upCmd.indexOf("ATDP") == 0) || (upCmd.indexOf("ATDI") == 0) || (upCmd.indexOf("ATDS") == 0))
  {
    dialOut(upCmd);
  }
  
  // === HANG UP ===
  else if (upCmd == "ATH") {
    hangUp();
  }
  // === TELNET ===
  else if (upCmd == "ATNET0") {
    telnet = false;
    sendResult(A_OK);
  }
  else if (upCmd == "ATNET1") {
    telnet = true;
    sendResult(A_OK);
  }
  else if (upCmd == "ATNET?") {
    Serial.println(telnet ? "1" : "0");
    sendResult(A_OK);
  }
  // === ANSWER CALL ===
  else if (upCmd == "ATA") {
    answerCall();
  }
  // === HELP ===
  else if (upCmd == "AT?" || upCmd == "ATHELP") {
    showHelp();
    sendResult(A_OK);
  }   
  // === RELOAD SETTINGS ===
  else if (upCmd == "ATZ") {
    loadSettings();
    sendResult(A_OK);
  }
  // === WIFI CONTROL ===
  else if (upCmd == "ATC0") {
    disconnectWiFi();
  }  
  else if (upCmd == "ATC1") {
    connectWiFi();
  }
  /**** Control local echo in command mode ****/
  else if (upCmd.indexOf("ATE") == 0) {
    if (upCmd.substring(3, 4) == "?") {
      sendString(String(echo));
      sendResult(A_OK);
    }
    else if (upCmd.substring(3, 4) == "0") {
      echo = 0;
      sendResult(A_OK);
    }
    else if (upCmd.substring(3, 4) == "1") {
      echo = 1;
      sendResult(A_OK);
    }
    else {
      sendResult(A_ERROR);
    }
  }
  /**** Control verbosity ****/
  else if (upCmd.indexOf("ATV") == 0) {
    if (upCmd.substring(3, 4) == "?") {
      sendString(String(verboseResults));
      sendResult(A_OK);
    }
    else if (upCmd.substring(3, 4) == "0") {
      verboseResults = 0;
      sendResult(A_OK);
    }
    else if (upCmd.substring(3, 4) == "1") {
      verboseResults = 1;
      sendResult(A_OK);
    }
    else {
      sendResult(A_ERROR);
    }
  }
 // === SET BAUD ===
  else if (upCmd.indexOf("AT$SB=") == 0) {
    long newBaud = cmd.substring(6).toInt();
    bool found = false;
    
    for (long rate : baudRates) {
      if (rate == newBaud) {
        found = true;
        break;
      }
    }
    
    if (found) {
      currentBaudRate = newBaud;
      Serial.print("BAUD RATE WILL CHANGE TO ");
      Serial.print(newBaud);
      Serial.println(" AFTER REBOOT");
      Serial.println("USE AT$RB TO REBOOT");
      sendResult(A_OK);
    } else {
      sendResult(A_ERROR);
    }
  }
  else if (upCmd == "AT$SB?") {
    Serial.println(currentBaudRate);
    sendResult(A_OK);
  }
  /**** Display busy message ****/
  else if (upCmd.indexOf("AT$BM?") == 0) {
    sendString(busyMsg);
    sendResult(A_OK);
  }
  // === NETWORK INFO ===
  else if (upCmd == "ATI") {
    showNetworkInfo();
    sendResult(A_OK);
  }  
  // === VIEW SETTINGS ===
  else if (upCmd == "AT&V") {
    showSettings();
    sendResult(A_OK);
  }
  // === SAVE SETTINGS ===
  else if (upCmd == "AT&W") {
    saveSettings();
    sendResult(A_OK);
  }  
  // === FACTORY RESET ===
  else if (upCmd == "AT&F") {
    factoryReset();
    sendResult(A_OK);
  }
 else if (upCmd.indexOf("AT&Z") == 0) {
    if (upCmd.length() >= 5 && upCmd.charAt(4) == '=') {
      int num = upCmd.charAt(3) - '0';
      if (num >= 0 && num <= 9) {
        speedDials[num] = cmd.substring(5);
        Serial.print("SPEED DIAL ");
        Serial.print(num);
        Serial.print(" SET: ");
        Serial.println(speedDials[num]);
        sendResult(A_OK);
      } else {
        sendResult(A_ERROR);
      }
    } else if (upCmd.length() == 5 && upCmd.charAt(4) == '?') {
      int num = upCmd.charAt(3) - '0';
      if (num >= 0 && num <= 9) {
        Serial.println(speedDials[num]);
        sendResult(A_OK);
      } else {
        sendResult(A_ERROR);
      }
    } else {
      sendResult(A_ERROR);
    }
  }
    
  // === SET SSID ===
  else if (upCmd.indexOf("AT$SSID=") == 0) {
    ssid = cmd.substring(8);
    Serial.print("SSID SET TO: ");
    Serial.println(ssid);
    sendResult(A_OK);
  }
  else if (upCmd == "AT$SSID?") {
    Serial.println(ssid);
    sendResult(A_OK);
  }
  
  // === SET PASSWORD ===
  else if (upCmd.indexOf("AT$PASS=") == 0) {
    password = cmd.substring(8);
    Serial.println("PASSWORD SET");
    sendResult(A_OK);
  }
  else if (upCmd == "AT$PASS?") {
    Serial.println("********");
    sendResult(A_OK);
  }
  // === AUTO ANSWER ===
  else if (upCmd == "ATS0=0") {
    autoAnswer = false;
    sendResult(A_OK);
  }
  else if (upCmd == "ATS0=1") {
    autoAnswer = true;
    sendResult(A_OK);
  }
  else if (upCmd == "ATS0?") {
    Serial.println(autoAnswer ? "1" : "0");
    sendResult(A_OK);
  }
  // === PETSCII ===
  else if (upCmd == "ATPET0") {
    petTranslate = false;
    sendResult(A_OK);
  }
  else if (upCmd == "ATPET1") {
    petTranslate = true;
    sendResult(A_OK);
  }
  else if (upCmd == "ATPET?") {
    Serial.println(petTranslate ? "1" : "0");
    sendResult(A_OK);
  }
  else if (upCmd == "ATHEX=1") {
  //  hex = true;
    sendResult(A_OK);
  }

  /**** Set HEX Translate Off ****/
  else if (upCmd == "ATHEX=0") {
 //   hex = false;
    sendResult(A_OK);
  }  
  // === GO ONLINE ===
  else if (upCmd == "ATO") {
    if (callConnected) {
      cmdMode = false;
      sendResult(A_CONNECT);
    } else {
      sendResult(A_ERROR);
    }
  }
  // === SET BUSY MESSAGE ===
  else if (upCmd.indexOf("AT$BM?") == 0) {
    busyMsg = cmd.substring(47);
    Serial.print("BUSY MESSAGE SET: ");
    Serial.println(busyMsg);
    sendResult(A_OK);
  }

  // === REBOOT ===
  else if (upCmd == "AT$RB") {
    Serial.println("REBOOTING...");
    delay(100);
    ESP.restart();
  }
  

  // === UNKNOWN COMMAND ===
  else {
    sendResult(A_ERROR);
  }
  
  cmd = "";
}

void handleWebRoot() {
  String page = "<html><head><title>ESP32 WiFi Modem</title></head><body>";
  page += "<h1>ESP32-S3 WiFi Modem</h1>";
  page += "<p>Firmware: " + String(FIRMWARE_VERSION) + "</p>";
  page += "<p>Build: " + String(BUILD_DATE) + "</p>";
  page += "<hr>";
  
  page += "<h2>WiFi Status</h2>";
  if (WiFi.status() == WL_CONNECTED) {
    page += "<p>Connected to: " + String(WiFi.SSID()) + "</p>";
    page += "<p>IP Address: " + WiFi.localIP().toString() + "</p>";
    page += "<p>Signal: " + String(WiFi.RSSI()) + " dBm</p>";
  } else {
    page += "<p>Not connected</p>";
  }
  
  page += "<h2>Call Status</h2>";
  if (callConnected) {
    page += "<p>Connected to: " + tcpClient.remoteIP().toString() + "</p>";
    page += "<p>Duration: " + connectTimeString() + "</p>";
    page += "<p><a href='/ath'>Hang Up</a></p>";
  } else {
    page += "<p>Not in a call</p>";
  }
  
  page += "<hr><p><a href='/reboot'>Reboot Modem</a></p>";
  page += "</body></html>";
  
  webServer.send(200, "text/html", page);
}

void handleWebHangup() {
  hangUp();
  webServer.send(200, "text/plain", "Call disconnected");
}

void handleWebReboot() {
  webServer.send(200, "text/plain", "Rebooting...");
  delay(100);
  ESP.restart();
}

void setup() {
 // Настройка пинов
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Выключить
  
  // Serial для USB
  Serial.begin(115200);
  delay(1000); // Дать время для инициализации USB
  
  // Загрузка настроек
  loadSettings();
  
  // Настройка WiFi
  WiFi.mode(WIFI_STA);
  
  // Web сервер
  webServer.on("/", handleWebRoot);
  webServer.on("/ath", handleWebHangup);
  webServer.on("/reboot", handleWebReboot);
  webServer.begin();
  
  // TCP сервер для входящих вызовов
  tcpServer.begin();
  
  // mDNS (если нужно)
  if (!MDNS.begin("esp32-modem")) {
    Serial.println("mDNS failed");
  }
  
  // Приветствие
  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32-S3 WiFi Modem " FIRMWARE_VERSION);
  Serial.println("Build: " BUILD_DATE);
  Serial.println("Type AT? for help");
  Serial.println("========================================");
  Serial.println();
  
  // Попытка подключиться к WiFi если настроено
  if (ssid.length() > 0) {
    Serial.println("Auto-connecting to WiFi...");
    connectWiFi();
  }
  
  sendResult(A_OK);
 
}


void loop() {
  // Обработка веб-запросов
  webServer.handleClient();
 // MDNS.update();
  
  // Проверка входящих вызовов
  handleIncomingCall();
  
  // +++ Переход в командный режим
  static int plusCount = 0;
  static unsigned long plusTime = 0;
  
  // Командный режим
  if (cmdMode) {
    if (Serial.available()) {
      char c = Serial.read();
      
      // PETSCII преобразование
      if (petTranslate && c > 127) c -= 128;
      
      // Enter = выполнить команду
      if (c == '\r' || c == '\n') {
        processCommand();
      }
      // Backspace
      else if (c == 8 || c == 127) {
        if (cmd.length() > 0) {
          cmd.remove(cmd.length() - 1);
          if (echo) {
            Serial.write(8);
            Serial.write(' ');
            Serial.write(8);
          }
        }
      }
      // Обычный символ
      else {
        if (cmd.length() < MAX_CMD_LENGTH) {
          cmd += c;
          if (echo) Serial.write(c);
        }
      }
    }
  }
  // Режим передачи данных
  else {
    // Данные от компьютера -> в сеть
    if (Serial.available()) {
      // Проверка на +++
      while (Serial.available()) {
        char c = Serial.read();
        
        // PETSCII преобразование
        if (petTranslate && c > 127) c -= 128;
        
        if (c == '+') {
          plusCount++;
          if (plusCount >= 3) plusTime = millis();
        } else {
          plusCount = 0;
        }
        
        // Отправка в сеть (если не +++)
        if (plusCount < 3 && tcpClient.connected()) {
          // Telnet escaping для 0xFF
          if (telnet && c == 0xFF) {
            tcpClient.write(0xFF);
          }
          tcpClient.write(c);
        }
      }
    }
    
    // Данные из сети -> в компьютер
    if (tcpClient.available()) {
      while (tcpClient.available() && Serial.availableForWrite() > 0) {
        uint8_t c = tcpClient.read();
        
        // Обработка Telnet кодов
        if (telnet && c == 0xFF) {
          uint8_t cmd1 = tcpClient.read();
          uint8_t cmd2 = tcpClient.read();
          
          // Отвечаем на запросы
          if (cmd1 == 0xFD) { // DO
            tcpClient.write(0xFF);
            tcpClient.write(0xFC); // WONT
            tcpClient.write(cmd2);
          } else if (cmd1 == 0xFB) { // WILL
            tcpClient.write(0xFF);
            tcpClient.write(0xFD); // DO
            tcpClient.write(cmd2);
          }
          continue;
        }
        
        Serial.write(c);
      }
    }
    
    // Проверка на разрыв соединения
    if (!tcpClient.connected() && callConnected) {
      hangUp();
      cmdMode = true;
    }
    
    // +++ таймаут (1 секунда)
    if (plusCount >= 3 && millis() - plusTime > 1000) {
      cmdMode = true;
      plusCount = 0;
      sendResult(A_OK);
    }
  }
  
  // Обновление индикатора
  static unsigned long lastLedUpdate = 0;
  if (millis() - lastLedUpdate > 100) {
    updateLed();
    lastLedUpdate = millis();
  }
}
