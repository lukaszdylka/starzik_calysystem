// --- Biblioteki ---
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <esp_now.h>
#include <DFRobotDFPlayerMini.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <Arduino.h>

// --- LCD ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- RFID ---
#define RST_PIN 4
#define SS_PIN 5
MFRC522 rfid(SS_PIN, RST_PIN);

// --- Przekaźnik ---
const int relayPin = 2;

// --- Klawiatura ---
const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
  { '1', '2', '3' },
  { '4', '5', '6' },
  { '7', '8', '9' },
  { '*', '0', '#' }
};
byte rowPins[ROWS] = { 32, 33, 25, 26 };
byte colPins[COLS] = { 27, 14, 13 };
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- DFPlayer ---
HardwareSerial mySoftwareSerial(1);
DFRobotDFPlayerMini myDFPlayer;
const int busyPin = 34;

// --- ESP-NOW Communication ---
// MAC adres ESP32 Master
uint8_t master_mac[] = {0x78, 0x1C, 0x3C, 0xF5, 0x82, 0xD8};

typedef struct {
  String command;
  String data;
  unsigned long timestamp;
} MasterMessage;

// --- Zmienne globalne ---
String correctCode = "81522252839";
String enteredCode = "";
bool tag1Used = false;
bool tag2Allowed = false;
bool tag2Used = false;

// Komunikacja z Master
bool masterConnected = false;
unsigned long lastMasterHeartbeat = 0;
const unsigned long HEARTBEAT_TIMEOUT = 30000;

// Statystyki dla panelu
String codesHistory[20]; // Ostatnie 20 kodów
int codesHistoryCount = 0;
int digitStats[10] = {0}; // Statystyki cyfr 0-9
String currentStage = "WAITING_TAG1";
unsigned long stageStartTime = 0;

// Debouncing klawiatury
unsigned long lastKeyTime = 0;
const unsigned long KEY_DEBOUNCE_DELAY = 200; // 200ms między klawiszami
char lastKey = 0;

// --- Deklaracje funkcji ---
void setupESPNow();
void waitForDFPlayer();
bool checkForTag(String targetUID);
void sendToMaster(String command, String data);
void sendStatusUpdate();
void sendCodeStatistics(String code, bool correct);
void resetPuzzle();
void openLockFromPanel();
void updateStage(String newStage);
void addCodeToHistory(String code, bool correct);
void updateDigitStatistics(String code);
void handleMasterMessage(String command, String data);
void sendHeartbeatToMaster();
void checkMasterConnection();
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len);

void setup() {
  Serial.begin(115200);
  mySoftwareSerial.begin(9600, SERIAL_8N1, 16, 17);
  
  pinMode(relayPin, OUTPUT);
  pinMode(busyPin, INPUT);
  digitalWrite(relayPin, LOW);

  lcd.init();
  lcd.noBacklight();
  lcd.clear();

  SPI.begin();
  rfid.PCD_Init();

  if (!myDFPlayer.begin(mySoftwareSerial)) {
    Serial.println("Nie można połączyć z DFPlayerem");
  } else {
    Serial.println("DFPlayer połączony");
    myDFPlayer.volume(25);
  }

  // Inicjalizacja WiFi dla ESP-NOW
  WiFi.mode(WIFI_STA);
  Serial.print("MAC Address Walizka: ");
  Serial.println(WiFi.macAddress());

  setupESPNow();
  
  // Inicjalizacja statystyk
  stageStartTime = millis();
  
  Serial.println("🧳 Walizka LOTTO gotowa!");
  
  // Wyślij początkowy status
  sendStatusUpdate();
}

void loop() {
  checkMasterConnection();
  
  // === ETAP 1: Oczekiwanie na Tag 1 ===
  if (!tag1Used && checkForTag("F1AAF73")) {
    Serial.println("📍 Tag 1 wykryty!");
    
    myDFPlayer.play(1);
    delay(200); // Bezpieczny delay
    
    // Otwórz zamek
    digitalWrite(relayPin, HIGH);
    delay(5000);
    digitalWrite(relayPin, LOW);
    
    // Przejdź do etapu klawiatury
    tag1Used = true;
    updateStage("KEYPAD_ACTIVE");
    
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Liczby LOTTO + #:");
    
    sendToMaster("tag1_detected", "F1AAF73");
    sendStatusUpdate();
  }

  // === ETAP 2: Wprowadzanie kodu ===
  if (tag1Used && !tag2Allowed) {
    char key = keypad.getKey();
    if (key) {
      myDFPlayer.play(2);
      waitForDFPlayer();
      
      if (key >= '0' && key <= '9' && enteredCode.length() < 11) {
        // Dodaj cyfrę
        enteredCode += key;
        lcd.setCursor(enteredCode.length() - 1, 1);
        lcd.print(key);
        Serial.println("Wprowadzono cyfrę: " + String(key));
        
        // Krótki beep bez blokowania
        myDFPlayer.play(2);
        delay(100); // Krótki delay zamiast waitForDFPlayer
        
      } else if (key == '*') {
        // Usuń ostatnią cyfrę
        if (enteredCode.length() > 0) {
          enteredCode.remove(enteredCode.length() - 1);
          lcd.setCursor(enteredCode.length(), 1);
          lcd.print(" ");
          lcd.setCursor(enteredCode.length(), 1);
        }
        
      } else if (key == '#') {
        // Sprawdź kod
        if (enteredCode.length() > 0) {
          Serial.println("Sprawdzanie kodu: " + enteredCode);
          
          bool isCorrect = (enteredCode == correctCode);
          String codeToSend = enteredCode; // Zapisz kod przed resetem!
          
          // Zapisz kod do statystyk
          addCodeToHistory(enteredCode, isCorrect);
          updateDigitStatistics(enteredCode);
          sendCodeStatistics(enteredCode, isCorrect);
          
          if (isCorrect) {
            // Kod poprawny
            tag2Allowed = true;
            updateStage("WAITING_TAG2");
            
            Serial.println("✅ Kod poprawny!");
            myDFPlayer.play(3);
            delay(200); // Bezpieczny delay zamiast waitForDFPlayer
            
            lcd.clear();
            lcd.print("Przyloz tag 2");
            
            sendToMaster("code_correct", codeToSend);
            sendStatusUpdate();
            
          } else {
            // Kod niepoprawny
            Serial.println("❌ Błędny kod!");
            myDFPlayer.play(4);
            delay(200); // Bezpieczny delay zamiast waitForDFPlayer
            
            lcd.clear();
            lcd.print("Numery niepoprawne");
            delay(2000);
            
            // Wyślij błędny kod PRZED resetem
            sendToMaster("code_incorrect", codeToSend);
            
            // Dopiero teraz reset wprowadzania
            enteredCode = "";
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Liczby LOTTO + #:");
          }
        }
      }
    }
  }

  // === ETAP 3: Oczekiwanie na Tag 2 ===
  if (tag2Allowed && !tag2Used && checkForTag("E3BF25E2")) {
    Serial.println("📍 Tag 2 wykryty!");
    
    tag2Used = true;
    updateStage("COMPLETED");
    
    lcd.clear();
    lcd.print("1 - POLSKI");
    lcd.setCursor(0, 1);
    lcd.print("2 - SLASKI");
    
    sendToMaster("tag2_detected", "E3BF25E2");
    sendStatusUpdate();
  }

  // === ETAP 4: Wybór języka ===
  if (tag2Used) {
    char key = keypad.getKey();
    
    // Debouncing dla wyboru języka
    if (key && (millis() - lastKeyTime > KEY_DEBOUNCE_DELAY || key != lastKey)) {
      lastKeyTime = millis();
      lastKey = key;
      
      if (key == '1') {
        Serial.println("🇵🇱 Wybrano język POLSKI");
        myDFPlayer.play(5);
        delay(200); // Bezpieczny delay
        sendToMaster("language_selected", "POLSKI");
        
      } else if (key == '2') {
        Serial.println("🏴 Wybrano język ŚLĄSKI");
        myDFPlayer.play(6);
        delay(200); // Bezpieczny delay
        sendToMaster("language_selected", "SLASKI");
      }
    }
  }
  
  // Wysyłaj heartbeat co 15 sekund
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 15000) {
    sendHeartbeatToMaster();
    lastHeartbeat = millis();
  }
  
  delay(50);
}

void setupESPNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Błąd inicjalizacji ESP-NOW");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, master_mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Błąd dodawania Master peer");
    return;
  }
  
  Serial.println("ESP-NOW skonfigurowane dla Master");
}

void waitForDFPlayer() {
  while (digitalRead(busyPin) == LOW) {
    delay(10);
  }
}

bool checkForTag(String targetUID) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return false;

  String readUID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    readUID += String(rfid.uid.uidByte[i], HEX);
  }
  readUID.toUpperCase();
  targetUID.toUpperCase();
  
  Serial.print("📡 Odczytano tag: ");
  Serial.println(readUID);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  return readUID == targetUID;
}

void sendToMaster(String command, String data) {
  String serialized = command + "|" + data + "|" + String(millis());
  uint8_t message[250];
  size_t len = serialized.length();
  if (len > 249) len = 249;
  serialized.getBytes(message, len + 1);
  
  esp_err_t result = esp_now_send(master_mac, message, len);
  if (result == ESP_OK) {
    Serial.println("📤 Wysłano do Master: " + command);
  } else {
    Serial.println("❌ Błąd wysyłania do Master");
  }
}

void sendStatusUpdate() {
  // Tworzenie JSON ze statusem
  DynamicJsonDocument doc(1024);
  doc["stage"] = currentStage;
  doc["tag1_used"] = tag1Used;
  doc["tag2_allowed"] = tag2Allowed;
  doc["tag2_used"] = tag2Used;
  doc["relay_state"] = digitalRead(relayPin);
  doc["entered_code_length"] = enteredCode.length();
  doc["stage_time"] = millis() - stageStartTime;
  
  // Historia kodów (ostatnie 5)
  JsonArray codes = doc.createNestedArray("codes_history");
  int start = max(0, codesHistoryCount - 5);
  for (int i = start; i < codesHistoryCount; i++) {
    codes.add(codesHistory[i]);
  }
  
  // Statystyki cyfr
  JsonObject stats = doc.createNestedObject("digit_stats");
  for (int i = 0; i < 10; i++) {
    stats[String(i)] = digitStats[i];
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  sendToMaster("status_update", jsonString);
}

void sendCodeStatistics(String code, bool correct) {
  DynamicJsonDocument doc(256);
  doc["code"] = code;
  doc["correct"] = correct;
  doc["timestamp"] = millis();
  doc["stage_time"] = millis() - stageStartTime;
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  sendToMaster("code_entered", jsonString);
}

void addCodeToHistory(String code, bool correct) {
  if (codesHistoryCount >= 20) {
    // Przesuń historię
    for (int i = 0; i < 19; i++) {
      codesHistory[i] = codesHistory[i + 1];
    }
    codesHistoryCount = 19;
  }
  
  // Format: "1234|1|1634567890" (kod|poprawny|timestamp)
  codesHistory[codesHistoryCount] = code + "|" + (correct ? "1" : "0") + "|" + String(millis());
  codesHistoryCount++;
}

void updateDigitStatistics(String code) {
  for (int i = 0; i < code.length(); i++) {
    char digit = code.charAt(i);
    if (digit >= '0' && digit <= '9') {
      digitStats[digit - '0']++;
    }
  }
}

void updateStage(String newStage) {
  currentStage = newStage;
  stageStartTime = millis();
  Serial.println("🔄 Nowy etap: " + newStage);
}

void resetPuzzle() {
  Serial.println("🔄 Reset zagadki przez Master");
  
  // Reset wszystkich zmiennych
  enteredCode = "";
  tag1Used = false;
  tag2Allowed = false;
  tag2Used = false;
  currentStage = "WAITING_TAG1";
  stageStartTime = millis();
  
  // Reset hardware
  digitalWrite(relayPin, LOW);
  lcd.noBacklight();
  lcd.clear();
  
  // Reset statystyk (opcjonalnie - możesz zachować)
  // for (int i = 0; i < 10; i++) digitStats[i] = 0;
  // codesHistoryCount = 0;
  
  sendStatusUpdate();
  Serial.println("✅ Zagadka zresetowana");
}

void openLockFromPanel() {
  Serial.println("🔓 Otwieranie zamka przez panel");
  digitalWrite(relayPin, HIGH);
  delay(5000);
  digitalWrite(relayPin, LOW);
  sendToMaster("lock_opened", "panel_command");
}

void handleMasterMessage(String command, String data) {
  Serial.println("🎛️ Master komenda: " + command + ", dane: " + data);
  
  if (command == "reset_puzzle") {
    resetPuzzle();
  } else if (command == "open_lock") {
    openLockFromPanel();
  } else if (command == "get_status") {
    sendStatusUpdate();
  } else if (command == "restart") {
    Serial.println("🔄 Restart żądany przez Master");
    delay(1000);
    ESP.restart();
  } else if (command == "heartbeat") {
    Serial.println("💓 Heartbeat od Master");
  } else {
    Serial.println("❓ Nieznana komenda od Master: " + command);
  }
}

void sendHeartbeatToMaster() {
  sendToMaster("heartbeat", "walizka_alive");
}

void checkMasterConnection() {
  if (masterConnected && (millis() - lastMasterHeartbeat > HEARTBEAT_TIMEOUT)) {
    masterConnected = false;
    Serial.println("❌ Utracono połączenie z Master");
  }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("❌ ESP-NOW: Błąd wysyłania do Master");
  }
}

void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  String receivedData = "";
  for (int i = 0; i < len; i++) {
    receivedData += (char)incomingData[i];
  }
  
  Serial.println("📡 Otrzymano od Master: " + receivedData);
  
  int firstPipe = receivedData.indexOf('|');
  int secondPipe = receivedData.indexOf('|', firstPipe + 1);
  
  if (firstPipe > 0 && secondPipe > 0) {
    String command = receivedData.substring(0, firstPipe);
    String data = receivedData.substring(firstPipe + 1, secondPipe);
    handleMasterMessage(command, data);
  }
  
  masterConnected = true;
  lastMasterHeartbeat = millis();
}