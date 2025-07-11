#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include <Arduino.h>

// Konfiguracja WiFi Access Point
const char* ap_ssid = "EscapeRoom_Master";
const char* ap_password = "escape123";

// Serwer WWW
WebServer server(80);

// MAC adresy ESP32 Slaves
uint8_t golab_mac[] = {0x14, 0x33, 0x5C, 0x0E, 0xCC, 0x24};
uint8_t walizka_mac[] = {0x14, 0x33, 0x5C, 0x0E, 0x16, 0x30};

// Struktura komunikacji z Gołąb
typedef struct {
  String command;
  String data;
  unsigned long timestamp;
} GolabMessage;

// Zmienne globalne
bool golabConnected = false;
bool walizkaConnected = false;
unsigned long lastGolabHeartbeat = 0;
unsigned long lastWalizkaHeartbeat = 0;
const unsigned long HEARTBEAT_TIMEOUT = 30000;
bool hintRequested = false;
unsigned long hintRequestTime = 0;

// Status Walizka LOTTO
struct WalizkaState {
  String stage;
  bool tag1Used;
  bool tag2Allowed;  
  bool tag2Used;
  bool relayState;
  int enteredCodeLength;
  unsigned long stageTime;
  String codesHistory[10];
  int codesCount;
  int digitStats[10];
  unsigned long lastUpdate;
} walizkaState;

// Status gry
struct GameSession {
  String sessionId;
  String groupName;
  int playerCount;
  bool isTestGame;
  bool isActive;
  bool isPaused;
  unsigned long startTime;
} currentGame;

// Deklaracje funkcji
void setupWiFiAP();
void setupESPNow();
void setupWebServer();
void listSPIFFSFiles();
void resetGameSession();
void resetWalizkaState();
void blinkLED(int times, int delayMs);
void checkGolabConnection();
bool sendAudioToGolab(String fileName);
bool sendCommandToGolab(String command, String data);
bool sendCommandToWalizka(String command, String data);
bool sendToGolab(GolabMessage& message);
bool sendToWalizka(GolabMessage& message);
void handleGolabMessage(String command, String data);
void handleWalizkaMessage(String command, String data);
bool startGame(JsonObject gameData);
bool pauseGame(bool paused);
bool endGame(String status);
String formatTimestamp(unsigned long timestamp);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len);

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Master - Escape Room Gołąb");
  Serial.print("MAC Address Master: ");
  Serial.println(WiFi.macAddress());

  pinMode(2, OUTPUT);

  if (!SPIFFS.begin(true)) {
    Serial.println("Błąd inicjalizacji SPIFFS");
    return;
  }
  
  listSPIFFSFiles();
  setupWiFiAP();
  setupESPNow();
  setupWebServer();
  resetGameSession();
  resetWalizkaState();

  Serial.println("Master gotowy!");
  Serial.print("Access Point IP: ");
  Serial.println(WiFi.softAPIP());
  blinkLED(3, 300);
}

void loop() {
  server.handleClient();
  checkGolabConnection();
  delay(100);
}

void setupWiFiAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  
  Serial.println("WiFi Access Point uruchomiony");
  Serial.print("SSID: ");
  Serial.println(ap_ssid);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupESPNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("Błąd inicjalizacji ESP-NOW");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Dodaj Gołąb
  esp_now_peer_info_t golabPeer;
  memset(&golabPeer, 0, sizeof(golabPeer));
  memcpy(golabPeer.peer_addr, golab_mac, 6);
  golabPeer.channel = 0;
  golabPeer.encrypt = false;
  golabPeer.ifidx = WIFI_IF_STA;

  if (esp_now_add_peer(&golabPeer) != ESP_OK) {
    Serial.println("Błąd dodawania Gołąb peer");
  } else {
    Serial.println("ESP-NOW: Gołąb dodany");
  }
  
  // Dodaj Walizka
  esp_now_peer_info_t walizkaPeer;
  memset(&walizkaPeer, 0, sizeof(walizkaPeer));
  memcpy(walizkaPeer.peer_addr, walizka_mac, 6);
  walizkaPeer.channel = 0;
  walizkaPeer.encrypt = false;
  walizkaPeer.ifidx = WIFI_IF_STA;

  if (esp_now_add_peer(&walizkaPeer) != ESP_OK) {
    Serial.println("Błąd dodawania Walizka peer");
  } else {
    Serial.println("ESP-NOW: Walizka dodana");
  }

  Serial.println("ESP-NOW skonfigurowane");
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    if (SPIFFS.exists("/index.html")) {
      File file = SPIFFS.open("/index.html", "r");
      server.streamFile(file, "text/html");
      file.close();
      Serial.println("Wysłano index.html");
    } else {
      server.send(404, "text/plain", "Panel index.html nie znaleziony na SPIFFS!");
      Serial.println("BŁĄD: index.html nie znaleziony!");
    }
  });

  server.on("/beep.mp3", HTTP_GET, []() {
    if (SPIFFS.exists("/beep.mp3")) {
      File file = SPIFFS.open("/beep.mp3", "r");
      server.streamFile(file, "audio/mpeg");
      file.close();
      Serial.println("Wysłano beep.mp3");
    } else {
      server.send(404, "text/plain", "beep.mp3 nie znaleziony na SPIFFS!");
      Serial.println("BŁĄD: beep.mp3 nie znaleziony!");
    }
  });

  server.on("/hint_status", HTTP_GET, []() {
    DynamicJsonDocument doc(256);
    doc["hint_requested"] = hintRequested;
    doc["hint_time"] = hintRequestTime;
    doc["timestamp"] = millis();
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
    
    hintRequested = false;
  });

  server.on("/status", HTTP_GET, []() {
    DynamicJsonDocument doc(512);
    doc["success"] = true;
    doc["ip"] = WiFi.softAPIP().toString();
    doc["ssid"] = ap_ssid;
    doc["rssi"] = WiFi.RSSI();
    doc["golab_connected"] = golabConnected;
    doc["walizka_connected"] = walizkaConnected;
    doc["game_active"] = currentGame.isActive;
    doc["uptime"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["spiffs_total"] = SPIFFS.totalBytes();
    doc["spiffs_used"] = SPIFFS.usedBytes();
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  server.on("/play_audio", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      DynamicJsonDocument doc(512);
      deserializeJson(doc, server.arg("plain"));
      
      String fileName = doc["fileName"];
      String sessionId = doc["sessionId"] | "";
      
      if (sendAudioToGolab(fileName)) {
        DynamicJsonDocument response(256);
        response["success"] = true;
        response["message"] = "Audio wysłane do Gołąb";
        String responseStr;
        serializeJson(response, responseStr);
        server.send(200, "application/json", responseStr);
        Serial.println("Wysłano audio do Gołąb: " + fileName);
      } else {
        DynamicJsonDocument response(256);
        response["success"] = false;
        response["error"] = "Błąd wysyłania do Gołąb";
        String responseStr;
        serializeJson(response, responseStr);
        server.send(500, "application/json", responseStr);
      }
    } else {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Brak danych\"}");
    }
  });

  server.on("/stop_audio", HTTP_POST, []() {
    if (sendCommandToGolab("stop_audio", "")) {
      server.send(200, "application/json", "{\"success\":true,\"message\":\"Stop wysłane do Gołąb\"}");
    } else {
      server.send(500, "application/json", "{\"success\":false,\"error\":\"Błąd komunikacji z Gołąb\"}");
    }
  });

  server.on("/command", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      DynamicJsonDocument doc(512);
      deserializeJson(doc, server.arg("plain"));
      
      String command = doc["command"];
      bool success = false;
      String message = "";
      
      if (command == "start_game") {
        success = startGame(doc["data"]);
        message = success ? "Gra rozpoczęta" : "Błąd rozpoczynania gry";
      } else if (command == "pause_game") {
        success = pauseGame(doc["data"]["paused"]);
        message = success ? "Pauza toggled" : "Błąd pauzy";
      } else if (command == "end_game") {
        success = endGame(doc["data"]["status"]);
        message = success ? "Gra zakończona" : "Błąd zakończenia gry";
      } else {
        message = "Nieznana komenda: " + command;
      }
      
      DynamicJsonDocument response(256);
      response["success"] = success;
      response["message"] = message;
      String responseStr;
      serializeJson(response, responseStr);
      server.send(200, "application/json", responseStr);
    } else {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Brak danych\"}");
    }
  });

  server.on("/set_volume", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      DynamicJsonDocument doc(256);
      deserializeJson(doc, server.arg("plain"));
      
      int volume = doc["volume"] | 20;
      volume = constrain(volume, 0, 30);
      
      if (sendCommandToGolab("set_volume", String(volume))) {
        DynamicJsonDocument response(256);
        response["success"] = true;
        response["message"] = "Głośność ustawiona: " + String(volume);
        String responseStr;
        serializeJson(response, responseStr);
        server.send(200, "application/json", responseStr);
        Serial.println("Ustawiono głośność Gołąb: " + String(volume));
      } else {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Błąd komunikacji z Gołąb\"}");
      }
    } else {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Brak danych\"}");
    }
  });

  server.on("/restart", HTTP_POST, []() {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Restartowanie Master...\"}");
    delay(1000);
    ESP.restart();
  });

  server.on("/restart_slave", HTTP_POST, []() {
    if (sendCommandToGolab("restart", "slave_restart")) {
      server.send(200, "application/json", "{\"success\":true,\"message\":\"Restart wysłany do Gołąb\"}");
    } else {
      server.send(500, "application/json", "{\"success\":false,\"error\":\"Błąd komunikacji z Gołąb\"}");
    }
  });

  server.on("/restart_all", HTTP_POST, []() {
    sendCommandToGolab("restart", "all_restart");
    sendCommandToWalizka("restart", "all_restart");
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Restartowanie całego systemu...\"}");
    delay(2000);
    ESP.restart();
  });

  // === NOWE ENDPOINTY ZAGADEK ===
  
  server.on("/puzzle_status", HTTP_GET, []() {
    DynamicJsonDocument doc(1024);
    
    // Status Walizka LOTTO
    JsonObject walizka = doc.createNestedObject("walizka");
    walizka["stage"] = walizkaState.stage;
    walizka["connected"] = walizkaConnected;
    walizka["last_update"] = walizkaState.lastUpdate;
    
    // Historia kodów
    JsonArray codes = walizka.createNestedArray("codesHistory");
    for (int i = 0; i < walizkaState.codesCount && i < 10; i++) {
      String entry = walizkaState.codesHistory[i];
      int pipe1 = entry.indexOf('|');
      int pipe2 = entry.indexOf('|', pipe1 + 1);
      
      if (pipe1 > 0 && pipe2 > 0) {
        JsonObject codeEntry = codes.createNestedObject();
        codeEntry["code"] = entry.substring(0, pipe1);
        codeEntry["correct"] = (entry.substring(pipe1 + 1, pipe2) == "1");
        codeEntry["timestamp"] = formatTimestamp(entry.substring(pipe2 + 1).toInt());
      }
    }
    
    // Statystyki cyfr
    JsonObject stats = walizka.createNestedObject("digitStats");
    for (int i = 0; i < 10; i++) {
      stats[String(i)] = walizkaState.digitStats[i];
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  server.on("/puzzle_command", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      DynamicJsonDocument doc(512);
      deserializeJson(doc, server.arg("plain"));
      
      String puzzle = doc["puzzle"];
      String command = doc["command"];
      
      if (puzzle == "walizka") {
        if (command == "open_lock") {
          if (sendCommandToWalizka("open_lock", "")) {
            server.send(200, "application/json", "{\"success\":true,\"message\":\"Komenda wysłana do Walizka\"}");
          } else {
            server.send(500, "application/json", "{\"success\":false,\"error\":\"Błąd komunikacji z Walizka\"}");
          }
        } else if (command == "reset") {
          if (sendCommandToWalizka("reset_puzzle", "")) {
            server.send(200, "application/json", "{\"success\":true,\"message\":\"Reset wysłany do Walizka\"}");
          } else {
            server.send(500, "application/json", "{\"success\":false,\"error\":\"Błąd komunikacji z Walizka\"}");
          }
        } else {
          server.send(400, "application/json", "{\"success\":false,\"error\":\"Nieznana komenda\"}");
        }
      } else {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Nieznana zagadka\"}");
      }
    } else {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Brak danych\"}");
    }
  });

  server.on("/restart_slave3", HTTP_POST, []() {
    if (sendCommandToWalizka("restart", "slave3_restart")) {
      server.send(200, "application/json", "{\"success\":true,\"message\":\"Restart wysłany do Walizka\"}");
    } else {
      server.send(500, "application/json", "{\"success\":false,\"error\":\"Błąd komunikacji z Walizka\"}");
    }
  });

  server.onNotFound([]() {
    String message = "File Not Found\n\n";
    message += "URI: " + server.uri() + "\n";
    message += "Method: " + String((server.method() == HTTP_GET) ? "GET" : "POST") + "\n";
    server.send(404, "text/plain", message);
  });

  server.begin();
  Serial.println("Serwer WWW uruchomiony na porcie 80");
}

void listSPIFFSFiles() {
  Serial.println("=== Pliki w SPIFFS ===");
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.print("Plik: ");
    Serial.print(file.name());
    Serial.print(" (");
    Serial.print(file.size());
    Serial.println(" bajtów)");
    file = root.openNextFile();
  }
  Serial.println("=====================");
}

String formatTimestamp(unsigned long timestamp) {
  unsigned long seconds = timestamp / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  seconds = seconds % 60;
  minutes = minutes % 60;
  hours = hours % 24;
  
  char timeStr[10];
  sprintf(timeStr, "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(timeStr);
}

bool sendAudioToGolab(String fileName) {
  GolabMessage msg;
  msg.command = "play_audio";
  msg.data = fileName;
  msg.timestamp = millis();
  return sendToGolab(msg);
}

bool sendCommandToGolab(String command, String data) {
  GolabMessage msg;
  msg.command = command;
  msg.data = data;
  msg.timestamp = millis();
  return sendToGolab(msg);
}

bool sendCommandToWalizka(String command, String data) {
  GolabMessage msg;
  msg.command = command;
  msg.data = data;
  msg.timestamp = millis();
  return sendToWalizka(msg);
}

bool sendToWalizka(GolabMessage& message) {
  if (!walizkaConnected) {
    Serial.println("Walizka nie jest połączona");
    return false;
  }
  
  String serialized = message.command + "|" + message.data + "|" + String(message.timestamp);
  uint8_t data[250];
  size_t len = serialized.length();
  if (len > 249) len = 249;
  serialized.getBytes(data, len + 1);
  
  esp_err_t result = esp_now_send(walizka_mac, data, len);
  if (result == ESP_OK) {
    Serial.println("Wiadomość wysłana do Walizka: " + message.command);
    return true;
  } else {
    Serial.println("Błąd wysyłania do Walizka");
    return false;
  }
}
bool sendToGolab(GolabMessage& message) {
  if (!golabConnected) {
    Serial.println("Gołąb nie jest połączony");
    return false;
  }
  
  String messageData = message.command + "|" + message.data + "|" + String(message.timestamp);
  uint8_t data[250];
  size_t len = messageData.length();
  if (len > 249) len = 249;
  messageData.getBytes(data, len + 1);
  
  esp_err_t result = esp_now_send(golab_mac, data, len);
  if (result == ESP_OK) {
    Serial.println("Wiadomość wysłana do Gołąb: " + message.command);
    return true;
  } else {
    Serial.println("Błąd wysyłania do Gołąb");
    return false;
  }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("ESP-NOW: Wysłano pomyślnie do Gołąb");
  } else {
    Serial.println("ESP-NOW: Błąd wysyłania do Gołąb");
  }
}

void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  String receivedData = "";
  for (int i = 0; i < len; i++) {
    receivedData += (char)incomingData[i];
  }
  
  // Sprawdź od którego urządzenia przyszła wiadomość
  bool fromGolab = true;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != golab_mac[i]) {
      fromGolab = false;
      break;
    }
  }
  
  bool fromWalizka = false;
  if (!fromGolab) {
    fromWalizka = true;
    for (int i = 0; i < 6; i++) {
      if (mac[i] != walizka_mac[i]) {
        fromWalizka = false;
        break;
      }
    }
  }
  
  if (fromGolab) {
    Serial.println("Otrzymano od Gołąb: " + receivedData);
    golabConnected = true;
    lastGolabHeartbeat = millis();
  } else if (fromWalizka) {
    Serial.println("Otrzymano od Walizka: " + receivedData);
    walizkaConnected = true;
    lastWalizkaHeartbeat = millis();
  } else {
    Serial.println("Otrzymano od nieznanego urządzenia: " + receivedData);
    return;
  }
  
  int firstPipe = receivedData.indexOf('|');
  int secondPipe = receivedData.indexOf('|', firstPipe + 1);
  
  if (firstPipe > 0 && secondPipe > 0) {
    String command = receivedData.substring(0, firstPipe);
    String data = receivedData.substring(firstPipe + 1, secondPipe);
    
    if (fromGolab) {
      handleGolabMessage(command, data);
    } else if (fromWalizka) {
      handleWalizkaMessage(command, data);
    }
  }
}

void handleWalizkaMessage(String command, String data) {
  Serial.println("Walizka komenda: " + command + ", dane: " + data);
  
  if (command == "status_update") {
    // Parsuj JSON ze statusem
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, data);
    
    if (!error) {
      walizkaState.stage = doc["stage"] | "WAITING_TAG1";
      walizkaState.tag1Used = doc["tag1_used"] | false;
      walizkaState.tag2Allowed = doc["tag2_allowed"] | false;
      walizkaState.tag2Used = doc["tag2_used"] | false;
      walizkaState.relayState = doc["relay_state"] | false;
      walizkaState.enteredCodeLength = doc["entered_code_length"] | 0;
      walizkaState.stageTime = doc["stage_time"] | 0;
      walizkaState.lastUpdate = millis();
      
      // Historia kodów
      JsonArray codes = doc["codes_history"];
      walizkaState.codesCount = 0;
      for (JsonVariant code : codes) {
        if (walizkaState.codesCount < 10) {
          walizkaState.codesHistory[walizkaState.codesCount] = code.as<String>();
          walizkaState.codesCount++;
        }
      }
      
      // Statystyki cyfr
      JsonObject stats = doc["digit_stats"];
      for (int i = 0; i < 10; i++) {
        walizkaState.digitStats[i] = stats[String(i)] | 0;
      }
      
      Serial.println("Status Walizka zaktualizowany");
    }
    
  } else if (command == "code_entered") {
    // Parsuj JSON z wprowadzonym kodem
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, data);
    
    if (!error) {
      String code = doc["code"] | "";
      bool correct = doc["correct"] | false;
      unsigned long timestamp = doc["timestamp"] | millis();
      
      // Dodaj do historii
      if (walizkaState.codesCount < 10) {
        String entry = code + "|" + (correct ? "1" : "0") + "|" + String(timestamp);
        walizkaState.codesHistory[walizkaState.codesCount] = entry;
        walizkaState.codesCount++;
      }
      
      // Aktualizuj statystyki cyfr
      for (int i = 0; i < code.length(); i++) {
        char digit = code.charAt(i);
        if (digit >= '0' && digit <= '9') {
          walizkaState.digitStats[digit - '0']++;
        }
      }
      
      walizkaState.lastUpdate = millis();
      Serial.println("Kod zapisany: " + code + " (poprawny: " + (correct ? "TAK" : "NIE") + ")");
    }
    
  } else if (command == "code_correct") {
    Serial.println("Walizka: Kod poprawny - " + data);
    
  } else if (command == "code_incorrect") {
    Serial.println("Walizka: Kod niepoprawny - " + data);
    
  } else if (command == "tag1_detected") {
    Serial.println("Walizka: Tag 1 wykryty");
    
  } else if (command == "tag2_detected") {
    Serial.println("Walizka: Tag 2 wykryty");
    
  } else if (command == "language_selected") {
    Serial.println("Walizka: Wybrano język: " + data);
    
  } else if (command == "lock_opened") {
    Serial.println("Walizka: Zamek otwarty");
    
  } else if (command == "heartbeat") {
    Serial.println("Heartbeat od Walizka");
    
  } else {
    Serial.println("Nieznana komenda od Walizka: " + command);
  }
}

void handleGolabMessage(String command, String data) {
  Serial.println("Gołąb komenda: " + command + ", dane: " + data);
  
  if (command == "hint_request") {
    Serial.println("🔔 HINT REQUEST od Gołąb!");
    hintRequested = true;
    hintRequestTime = millis();
    blinkLED(5, 100);
  } else if (command == "status") {
    Serial.println("Status Gołąb: " + data);
  } else if (command == "audio_finished") {
    Serial.println("Gołąb zakończył odtwarzanie audio");
  } else if (command == "volume_set") {
    Serial.println("Gołąb: głośność ustawiona na " + data);
  } else if (command == "error") {
    Serial.println("Błąd Gołąb: " + data);
  } else if (command == "heartbeat") {
    Serial.println("Heartbeat od Gołąb");
  } else {
    Serial.println("Nieznana komenda od Gołąb: " + command);
  }
}

bool startGame(JsonObject gameData) {
  currentGame.sessionId = gameData["sessionId"] | String(millis());
  currentGame.groupName = gameData["groupName"] | "Unknown";
  currentGame.playerCount = gameData["playerCount"] | 4;
  currentGame.isTestGame = gameData["isTestGame"] | false;
  currentGame.isActive = true;
  currentGame.isPaused = false;
  currentGame.startTime = millis();
  
  Serial.println("Rozpoczynanie gry: " + currentGame.groupName);
  sendCommandToGolab("start_game", currentGame.groupName);
  return true;
}

bool pauseGame(bool paused) {
  if (!currentGame.isActive) return false;
  
  currentGame.isPaused = paused;
  Serial.println(paused ? "Gra wstrzymana" : "Gra wznowiona");
  sendCommandToGolab(paused ? "pause_game" : "resume_game", "");
  return true;
}

bool endGame(String status) {
  if (!currentGame.isActive) return false;
  
  Serial.println("Kończenie gry ze statusem: " + status);
  sendCommandToGolab("end_game", status);
  resetGameSession();
  return true;
}

void checkGolabConnection() {
  // Sprawdź połączenie z Gołąb
  if (golabConnected && (millis() - lastGolabHeartbeat > HEARTBEAT_TIMEOUT)) {
    golabConnected = false;
    Serial.println("Utracono połączenie z Gołąb");
  }
  
  // Sprawdź połączenie z Walizka
  if (walizkaConnected && (millis() - lastWalizkaHeartbeat > HEARTBEAT_TIMEOUT)) {
    walizkaConnected = false;
    Serial.println("Utracono połączenie z Walizka");
  }
  
  // Wysyłaj heartbeaty co 10 sekund
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 10000) {
    if (golabConnected) {
      sendCommandToGolab("heartbeat", "master_ping");
    }
    if (walizkaConnected) {
      sendCommandToWalizka("heartbeat", "master_ping");
    }
    lastHeartbeat = millis();
  }
}

void resetGameSession() {
  currentGame.sessionId = "";
  currentGame.groupName = "";
  currentGame.playerCount = 0;
  currentGame.isTestGame = false;
  currentGame.isActive = false;
  currentGame.isPaused = false;
  currentGame.startTime = 0;
}

void resetWalizkaState() {
  walizkaState.stage = "WAITING_TAG1";
  walizkaState.tag1Used = false;
  walizkaState.tag2Allowed = false;
  walizkaState.tag2Used = false;
  walizkaState.relayState = false;
  walizkaState.enteredCodeLength = 0;
  walizkaState.stageTime = 0;
  walizkaState.codesCount = 0;
  walizkaState.lastUpdate = 0;
  
  for (int i = 0; i < 10; i++) {
    walizkaState.digitStats[i] = 0;
    if (i < 10) {
      walizkaState.codesHistory[i] = "";
    }
  }
}

void blinkLED(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(2, HIGH);
    delay(delayMs);
    digitalWrite(2, LOW);
    delay(delayMs);
  }
}