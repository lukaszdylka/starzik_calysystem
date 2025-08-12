// WALIZKA.ino
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
#define SS_PIN  5
MFRC522 rfid(SS_PIN, RST_PIN);

// --- Przekaźnik lokalny (zamek w walizce) ---
const int relayPin = 2;

// --- KONTAKTRON ---
const int kontaktronPin = 12;
bool lastMagnetState = false;
unsigned long lastMagnetCheck = 0;
const unsigned long MAGNET_DEBOUNCE_DELAY = 100;

// >>> DODANE: opóźnienie uzbrojenia etapu magnesu
unsigned long magnetArmedAt = 0;
const unsigned long MAGNET_ARM_DELAY = 300; // ms

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

// --- ESP-NOW adresy ---
uint8_t master_mac[] = {0x78, 0x1C, 0x3C, 0xF5, 0x82, 0xD8};  // MASTER – panel www
uint8_t device_mac[] = {0x78, 0x1C, 0x3C, 0xF5, 0x88, 0x88};   // SLAVE: podłoga z przekaźnikiem

// --- Tagi startowe ---
const char* START_TAG_1 = "F1AAF703";
const char* START_TAG_2 = "E3BF25E2";

// --- Numer pliku po wybraniu „skrytki” (np. pusty dźwięk) ---
const uint16_t TRIGGER_SOUND_TRACK = 7; // => 0007.mp3

// --- Zmienne logiki ---
String correctCode = "010716363847";
String enteredCode = "";
bool tag1Used = false;

// MAGNES
bool magnetAllowed = false;
bool magnetUsed = false;

// JĘZYK -> po pliku "Podaj nr skrytki" i wpis 53 (wielokrotnie)
bool languageChosen = false;
bool waitingForCompartment = false;  // tryb wprowadzania numeru skrytki
String compartmentInput = "";        // bufor cyfr skrytki

// Master status
bool masterConnected = false;
unsigned long lastMasterHeartbeat = 0;
const unsigned long HEARTBEAT_TIMEOUT = 30000;

// Statystyki
String codesHistory[20];
int codesHistoryCount = 0;
int digitStats[10] = {0};
String currentStage = "WAITING_TAG1";
unsigned long stageStartTime = 0;

// Debounce klawiatury
unsigned long lastKeyTime = 0;
const unsigned long KEY_DEBOUNCE_DELAY = 200;
char lastKey = 0;

// --- Deklaracje ---
void setupESPNow();
void waitForDFPlayer();
bool readUIDIfPresent(String &uidHex);
bool checkMagnet();
void sendToMaster(String command, String data);
bool sendToPeer(const uint8_t mac[6], const String& command, const String& data);
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

// --- Implementacja ---
void setup() {
  Serial.begin(115200);
  mySoftwareSerial.begin(9600, SERIAL_8N1, 16, 17);

  pinMode(relayPin, OUTPUT);
  pinMode(busyPin, INPUT);
  pinMode(kontaktronPin, INPUT_PULLUP);
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
    myDFPlayer.volume(20);
  }

  WiFi.mode(WIFI_STA);
  Serial.print("MAC Address Walizka: ");
  Serial.println(WiFi.macAddress());

  setupESPNow();

  stageStartTime = millis();
  Serial.println("🧳 Walizka gotowa!");
  Serial.println("🧲 Kontaktron pin: " + String(kontaktronPin));

  sendStatusUpdate();
}

void loop() {
  checkMasterConnection();

  // === ETAP 1: Start po TAGU (dowolny z 2 UID) ===
  if (!tag1Used) {
    String uid;
    if (readUIDIfPresent(uid)) {
      Serial.print("📡 Odczytano tag: "); Serial.println(uid);
      if (uid == START_TAG_1 || uid == START_TAG_2) {
        Serial.println("📍 Tag startowy OK");

        myDFPlayer.play(1);
        delay(200);

        // Otwórz lokalną zworę
        digitalWrite(relayPin, HIGH);
        delay(5000);
        digitalWrite(relayPin, LOW);

        tag1Used = true;
        updateStage("KEYPAD_ACTIVE");

        lcd.backlight();
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Liczby LOTTO + #:");

        sendToMaster("tag1_detected", uid);
        sendStatusUpdate();
      }
    }
  }

  // === ETAP 2: Kod ===
  if (tag1Used && !magnetAllowed) {
    char key = keypad.getKey();
    if (key) {
      myDFPlayer.play(2);
      waitForDFPlayer();

      if (key >= '0' && key <= '9' && enteredCode.length() < 12) {
        enteredCode += key;
        lcd.setCursor(enteredCode.length() - 1, 1);
        lcd.print(key);
        Serial.println("Wprowadzono: " + String(key));
        myDFPlayer.play(2);
        delay(100);

      } else if (key == '*') {
        if (enteredCode.length() > 0) {
          enteredCode.remove(enteredCode.length() - 1);
          lcd.setCursor(enteredCode.length(), 1);
          lcd.print(" ");
          lcd.setCursor(enteredCode.length(), 1);
        }

      } else if (key == '#') {
        if (enteredCode.length() > 0) {
          Serial.println("Sprawdzanie: " + enteredCode);

          bool isCorrect = (enteredCode == correctCode);
          String codeToSend = enteredCode;

          addCodeToHistory(enteredCode, isCorrect);
          updateDigitStatistics(enteredCode);
          sendCodeStatistics(enteredCode, isCorrect);

          if (isCorrect) {
            magnetAllowed = true;
            updateStage("WAITING_MAGNET");

            // >>> DODANE: uzbrój detekcję — zapamiętaj stan w chwili wejścia w etap
            lastMagnetState = !digitalRead(kontaktronPin);
            magnetArmedAt = millis();
            Serial.println(String("ARM MAGNET, initial=") + (lastMagnetState ? "MAGNES" : "BRAK"));

            Serial.println("✅ Kod OK – czekam na magnes");
            myDFPlayer.play(3);
            delay(200);

            lcd.clear();
            lcd.print("ZEFLIK");

            sendToMaster("code_correct", codeToSend);
            sendStatusUpdate();

          } else {
            Serial.println("❌ Zly kod");
            myDFPlayer.play(4);
            delay(200);

            lcd.clear();
            lcd.print("Zle numery");
            delay(2000);

            sendToMaster("code_incorrect", codeToSend);

            enteredCode = "";
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Liczby LOTTO + #:");
          }
        }
      }
    }
  }

  // === ETAP 3: Magnes ===
  if (magnetAllowed && !magnetUsed && checkMagnet()) {
    Serial.println("🧲 Magnes wykryty!");
    magnetUsed = true;
    updateStage("LANGUAGE_SELECT");

    lcd.clear();
    lcd.print("1 - POLSKI");
    lcd.setCursor(0, 1);
    lcd.print("2 - SLASKI");

    sendToMaster("magnet_detected", "kontaktron_activated");
    sendStatusUpdate();
  }

  // === ETAP 4: Wybór języka -> po pliku: "Podaj nr skrytki" ===
  if (magnetUsed && !languageChosen) {
    char key = keypad.getKey();

    if (key && (millis() - lastKeyTime > KEY_DEBOUNCE_DELAY || key != lastKey)) {
      lastKeyTime = millis();
      lastKey = key;

      if (key == '1') {
        Serial.println("🇵🇱 Polski");
        myDFPlayer.play(5);
        delay(200);
        sendToMaster("language_selected", "POLSKI");
        languageChosen = true;

      } else if (key == '2') {
        Serial.println("🏴 Śląski");
        myDFPlayer.play(6);
        delay(200);
        sendToMaster("language_selected", "SLASKI");
        languageChosen = true;
      }

      if (languageChosen) {
        // czekaj aż skończy się plik językowy
        waitForDFPlayer();

        // komunikat do gracza
        lcd.clear();
        lcd.setCursor(0,0); lcd.print("Podaj nr skrytki");
        lcd.setCursor(0,1); lcd.print("                ");

        compartmentInput = "";
        waitingForCompartment = true;     // od teraz przyjmujemy cyfry skrytki (wielokrotnie)
        updateStage("WAITING_COMPARTMENT");
        sendStatusUpdate();
      }
    }
  }

  // === ETAP 5: Wprowadzanie numeru skrytki (wymagane 2 cyfry: "53") ===
  if (waitingForCompartment) {
    char k = keypad.getKey();
    if (k && (millis() - lastKeyTime > KEY_DEBOUNCE_DELAY || k != lastKey)) {
      lastKeyTime = millis();
      lastKey = k;

      if (k >= '0' && k <= '9') {
        if (compartmentInput.length() < 2) {
          compartmentInput += k;

          // pokaż wpisywane cyfry (na drugiej linii)
          lcd.setCursor(0,1);
          if (compartmentInput.length() == 1) {
            lcd.print(String(k) + "               ");
          } else {
            lcd.print(compartmentInput + "              ");
          }
        }

        if (compartmentInput.length() == 2) {
          if (compartmentInput == "53") {
            // poprawny numer skrytki -> wyślij do SLAVE + zagraj stały plik
            Serial.println("🔓 Skrytka 53 -> relay_on (SLAVE) + audio");
            bool ok = sendToPeer(device_mac, "relay_on", "latch");
            myDFPlayer.play(TRIGGER_SOUND_TRACK);

            // krótki feedback
            lcd.setCursor(0,1);
            lcd.print(ok ? "OK              " : "Blad wysylki    ");
          } else {
            // zły numer — krótki komunikat
            Serial.println("❌ Zly nr skrytki");
            lcd.setCursor(0,1);
            lcd.print("Zly numer       ");
            myDFPlayer.play(TRIGGER_SOUND_TRACK); // jeśli nie chcesz dźwięku przy błędzie, usuń tę linię
          }

          // po chwili wróć do wpisywania kolejnego numeru (wielokrotnie, aż do resetu)
          delay(600);
          compartmentInput = "";
          lcd.setCursor(0,0); lcd.print("Podaj nr skrytki");
          lcd.setCursor(0,1); lcd.print("                ");
          updateStage("WAITING_COMPARTMENT");
          sendStatusUpdate();
        }
      } else if (k == '*') {
        // kasuj ostatnią cyfrę
        if (compartmentInput.length() > 0) {
          compartmentInput.remove(compartmentInput.length() - 1);
          lcd.setCursor(0,1);
          lcd.print((compartmentInput.length() ? compartmentInput : String(" ")) + "               ");
        }
      } else if (k == '#') {
        // ignorujemy '#' – niepotrzebny tu ENTER
      }
    }
  }

  // Heartbeat do MASTER co 15 s
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 15000) {
    sendHeartbeatToMaster();
    lastHeartbeat = millis();
  }

  delay(50);
}

// --- Pomocnicze ---
// Czyta kartę jeśli jest – zwraca true i wpisuje UID (UPPERCASE, bez spacji)
bool readUIDIfPresent(String &uidHex) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return false;

  uidHex = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidHex += "0";
    uidHex += String(rfid.uid.uidByte[i], HEX);
  }
  uidHex.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return true;
}

bool checkMagnet() {
  // >>> DODANE: krótki czas uzbrojenia etapu magnesu
  if (millis() - magnetArmedAt < MAGNET_ARM_DELAY) return false;

  // Debounce
  if (millis() - lastMagnetCheck < MAGNET_DEBOUNCE_DELAY) return false;
  lastMagnetCheck = millis();

  bool currentMagnetState = !digitalRead(kontaktronPin); // pullup -> LOW = magnes
  // wykryj zbocze: BRAK->MAGNES
  if (currentMagnetState && !lastMagnetState) {
    lastMagnetState = currentMagnetState;
    Serial.println("🧲 Kontaktron: zblizenie!");
    return true;
  }
  lastMagnetState = currentMagnetState;
  return false;
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

  // MASTER (dla statusów)
  {
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, master_mac, 6);
    p.channel = 0; p.encrypt = false; p.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&p) != ESP_OK) Serial.println("Błąd dodawania Master peer");
    else Serial.println("ESP-NOW: dodano MASTER");
  }

  // SLAVE: podłoga z przekaźnikiem
  {
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, device_mac, 6);
    p.channel = 0; p.encrypt = false; p.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&p) != ESP_OK) Serial.println("Błąd dodawania RELAY peer");
    else Serial.println("ESP-NOW: dodano RELAY (podloga)");
  }
}

void waitForDFPlayer() {
  while (digitalRead(busyPin) == LOW) {
    delay(10);
  }
}

bool sendToPeer(const uint8_t mac[6], const String& command, const String& data) {
  String serialized = command + "|" + data + "|" + String(millis());
  uint8_t message[250];
  size_t len = serialized.length();
  if (len > 249) len = 249;
  serialized.getBytes(message, len + 1);

  esp_err_t res = esp_now_send(mac, message, len);
  if (res == ESP_OK) {
    Serial.println("📤 Wysłano (peer): " + command + " | " + data);
    return true;
  } else {
    Serial.println("❌ ESP-NOW send error: " + String(res));
    return false;
  }
}

void sendToMaster(String command, String data) {
  String serialized = command + "|" + data + "|" + String(millis());
  uint8_t message[250];
  size_t len = serialized.length();
  if (len > 249) len = 249;
  serialized.getBytes(message, len + 1);

  esp_err_t result = esp_now_send(master_mac, message, len);
  if (result == ESP_OK) Serial.println("📤 Wysłano do Master: " + command);
  else Serial.println("❌ Błąd wysyłania do Master");
}

void sendStatusUpdate() {
  DynamicJsonDocument doc(1024);
  doc["stage"] = currentStage;
  doc["tag1_used"] = tag1Used;
  doc["magnet_allowed"] = magnetAllowed;
  doc["magnet_used"] = magnetUsed;
  doc["magnet_state"] = !digitalRead(kontaktronPin);
  doc["relay_state"] = (bool)digitalRead(relayPin);
  doc["entered_code_length"] = enteredCode.length();
  doc["stage_time"] = millis() - stageStartTime;
  doc["language_chosen"] = languageChosen;
  doc["waiting_for_compartment"] = waitingForCompartment;

  // historia ostatnich 5 kodów
  JsonArray codes = doc.createNestedArray("codes_history");
  int start = max(0, codesHistoryCount - 5);
  for (int i = start; i < codesHistoryCount; i++) codes.add(codesHistory[i]);

  JsonObject stats = doc.createNestedObject("digit_stats");
  for (int i = 0; i < 10; i++) stats[String(i)] = digitStats[i];

  String jsonString; serializeJson(doc, jsonString);
  sendToMaster("status_update", jsonString);
}

void sendCodeStatistics(String code, bool correct) {
  DynamicJsonDocument doc(256);
  doc["code"] = code;
  doc["correct"] = correct;
  doc["timestamp"] = millis();
  doc["stage_time"] = millis() - stageStartTime;
  String jsonString; serializeJson(doc, jsonString);
  sendToMaster("code_entered", jsonString);
}

void addCodeToHistory(String code, bool correct) {
  if (codesHistoryCount >= 20) {
    for (int i = 0; i < 19; i++) codesHistory[i] = codesHistory[i + 1];
    codesHistoryCount = 19;
  }
  codesHistory[codesHistoryCount] = code + "|" + (correct ? "1" : "0") + "|" + String(millis());
  codesHistoryCount++;
}

void updateDigitStatistics(String code) {
  for (int i = 0; i < code.length(); i++) {
    char d = code.charAt(i);
    if (d >= '0' && d <= '9') digitStats[d - '0']++;
  }
}

void updateStage(String newStage) {
  currentStage = newStage;
  stageStartTime = millis();
  Serial.println("🔄 Nowy etap: " + newStage);
}

void resetPuzzle() {
  Serial.println("🔄 Reset zagadki");
  enteredCode = ""; tag1Used = false;
  magnetAllowed = false; magnetUsed = false;
  languageChosen = false; waitingForCompartment = false; compartmentInput = "";
  currentStage = "WAITING_TAG1"; stageStartTime = millis();
  digitalWrite(relayPin, LOW);
  lcd.noBacklight(); lcd.clear();
  sendStatusUpdate();
  Serial.println("✅ Zresetowano");
}

void openLockFromPanel() {
  Serial.println("🔓 Otwieranie zamka (panel)");
  digitalWrite(relayPin, HIGH); delay(5000); digitalWrite(relayPin, LOW);
  sendToMaster("lock_opened", "panel_command");
}

void handleMasterMessage(String command, String data) {
  Serial.println("🎛️ Master: " + command + " | " + data);
  if (command == "reset_puzzle") resetPuzzle();
  else if (command == "open_lock") openLockFromPanel();
  else if (command == "get_status") sendStatusUpdate();
  else if (command == "restart") { Serial.println("🔄 Restart"); delay(1000); ESP.restart(); }
}

void sendHeartbeatToMaster() { sendToMaster("heartbeat", "walizka_alive"); }

void checkMasterConnection() {
  if (masterConnected && (millis() - lastMasterHeartbeat > HEARTBEAT_TIMEOUT)) {
    masterConnected = false;
    Serial.println("❌ Utracono połączenie z Master");
  }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) Serial.println("❌ ESP-NOW: błąd wysyłania (walizka)");
}

void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  // filtrujemy nadawcę: tylko MASTER jest sterujący
  String payload; payload.reserve(len+1);
  for (int i = 0; i < len; i++) payload += (char)incomingData[i];

  if (memcmp(mac, master_mac, 6) == 0) {
    int p1 = payload.indexOf('|');
    int p2 = payload.indexOf('|', p1 + 1);
    if (p1 > 0 && p2 > 0) {
      String command = payload.substring(0, p1);
      String data    = payload.substring(p1 + 1, p2);
      handleMasterMessage(command, data);
    }
    masterConnected = true;
    lastMasterHeartbeat = millis();
  } else {
    // np. wiadomości od innych ESP — na razie tylko log
    Serial.print("📡 Otrzymano (walizka) od innego MAC: ");
    char macbuf[18];
    snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    Serial.print(macbuf); Serial.print(" : "); Serial.println(payload);
  }
}
