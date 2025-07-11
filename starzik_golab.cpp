#include <WiFi.h>
#include <esp_now.h>
#include <DFRobotDFPlayerMini.h>
#include <HardwareSerial.h>
#include <Arduino.h>

// Konfiguracja DFPlayer
HardwareSerial mySoftwareSerial(2); // UART2
DFRobotDFPlayerMini myDFPlayer;

// Konfiguracja przycisku
#define HINT_BUTTON_PIN 4
#define LED_PIN 2

// MAC adres ESP32 Master - ZMIEŃ NA PRAWDZIWY!
uint8_t master_mac[] = {0x78, 0x1C, 0x3C, 0xF5, 0x82, 0xD8};

// Struktura komunikacji
typedef struct {
  String command;
  String data;
  unsigned long timestamp;
} MasterMessage;

// Zmienne globalne
bool masterConnected = false;
unsigned long lastMasterHeartbeat = 0;
const unsigned long HEARTBEAT_TIMEOUT = 30000;

// Przycisk
bool lastButtonState = HIGH;
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
bool hintRequestSent = false;
const unsigned long HINT_PRESS_TIME = 3000;

// Audio
bool isPlayingAudio = false;
String currentAudioFile = "";

// Stan gry
bool gameActive = false;
bool gamePaused = false;
String currentGameGroup = "";

// Deklaracje funkcji
void setupESPNow();
void setupDFPlayer();
void checkHintButton();
void sendHintRequest();
void checkMasterConnection();
void checkAudioStatus();
void handleMasterMessage(String command, String data);
void playAudio(String fileName);
int getFileNumber(String fileName);
void stopAudio();
void setVolume(int volume);
void startGame(String groupName);
void pauseGame();
void resumeGame();
void endGame(String status);
void sendHeartbeatToMaster();
bool sendToMaster(MasterMessage& message);
void blinkLED(int times, int delayMs);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len);

void setup() {
  Serial.begin(115200);
  mySoftwareSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("ESP32 Gołąb - Escape Room Slave");

  pinMode(HINT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  Serial.print("MAC Address Gołąb: ");
  Serial.println(WiFi.macAddress());

  setupESPNow();
  setupDFPlayer();

  Serial.println("Gołąb gotowy!");
  blinkLED(3, 300);
}

void loop() {
  checkHintButton();
  checkMasterConnection();
  checkAudioStatus();
  
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 15000) {
    sendHeartbeatToMaster();
    lastHeartbeat = millis();
  }
  
  delay(50);
}

void setupESPNow() {
  // Upewnij się że WiFi jest w trybie STA
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

void setupDFPlayer() {
  Serial.println("Inicjalizacja DFPlayer Mini...");
  
  if (!myDFPlayer.begin(mySoftwareSerial)) {
    Serial.println("Błąd komunikacji z DFPlayer:");
    Serial.println("1. Sprawdź połączenia RX/TX (16,17)");
    Serial.println("2. Sprawdź kartę SD");
    Serial.println("3. Sprawdź zasilanie DFPlayer");
    while(true){
      delay(0);
    }
  }
  
  Serial.println("DFPlayer Mini online.");
  myDFPlayer.volume(20);
  myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
  delay(1000);
  
  int fileCount = myDFPlayer.readFileCounts();
  Serial.print("Plików na karcie SD: ");
  Serial.println(fileCount);
  
  if (fileCount >= 2) {
    Serial.println("Karta SD wykryta z plikami MP3");
    Serial.println("TESTY: Oczekiwane pliki 001.mp3, 002.mp3");
  } else {
    Serial.println("UWAGA: Brak wystarczającej liczby plików MP3 na karcie SD!");
    Serial.println("TESTY: Potrzebujesz 2 pliki: 001.mp3, 002.mp3");
  }
}

void checkHintButton() {
  bool currentButtonState = digitalRead(HINT_BUTTON_PIN);
  
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonPressStart = millis();
    buttonPressed = true;
    hintRequestSent = false;
    Serial.println("Przycisk naciśnięty - liczenie czasu...");
    digitalWrite(LED_PIN, HIGH);
  }
  
  if (buttonPressed && currentButtonState == LOW) {
    unsigned long pressDuration = millis() - buttonPressStart;
    if (pressDuration >= HINT_PRESS_TIME && !hintRequestSent) {
      sendHintRequest();
      hintRequestSent = true;
      blinkLED(5, 100);
    }
  }
  
  if (currentButtonState == HIGH && lastButtonState == LOW) {
    digitalWrite(LED_PIN, LOW);
    buttonPressed = false;
    unsigned long pressDuration = millis() - buttonPressStart;
    Serial.print("Przycisk puszczony po: ");
    Serial.print(pressDuration);
    Serial.println(" ms");
    
    if (pressDuration < HINT_PRESS_TIME) {
      Serial.println("Za krótkie naciśnięcie - wymagane 3 sekundy");
    }
  }
  
  lastButtonState = currentButtonState;
}

void sendHintRequest() {
  if (!gameActive) {
    Serial.println("Gra nieaktywna - ignoruję żądanie podpowiedzi");
    return;
  }
  
  Serial.println("🔔 Wysyłam żądanie podpowiedzi do Master");
  MasterMessage msg;
  msg.command = "hint_request";
  msg.data = "golab_button_3sec";
  msg.timestamp = millis();
  
  if (sendToMaster(msg)) {
    Serial.println("✅ Żądanie podpowiedzi wysłane!");
  } else {
    Serial.println("❌ Błąd wysyłania żądania podpowiedzi");
  }
}

void checkMasterConnection() {
  if (masterConnected && (millis() - lastMasterHeartbeat > HEARTBEAT_TIMEOUT)) {
    masterConnected = false;
    Serial.println("Utracono połączenie z Master");
    if (isPlayingAudio) {
      myDFPlayer.stop();
      isPlayingAudio = false;
    }
    gameActive = false;
  }
}

void checkAudioStatus() {
  if (isPlayingAudio && myDFPlayer.available()) {
    uint8_t type = myDFPlayer.readType();
    int value = myDFPlayer.read();
    
    if (type == DFPlayerPlayFinished) {
      Serial.println("Zakończono odtwarzanie audio");
      isPlayingAudio = false;
      
      MasterMessage msg;
      msg.command = "audio_finished";
      msg.data = currentAudioFile;
      msg.timestamp = millis();
      sendToMaster(msg);
      
      currentAudioFile = "";
    } else if (type == DFPlayerError) {
      Serial.print("Błąd DFPlayer: ");
      Serial.println(value);
      isPlayingAudio = false;
    }
  }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("ESP-NOW: Błąd wysyłania do Master");
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

void handleMasterMessage(String command, String data) {
  Serial.println("🎛️ Master komenda: " + command + ", dane: " + data);
  
  if (command == "play_audio") {
    playAudio(data);
  } else if (command == "stop_audio") {
    stopAudio();
  } else if (command == "set_volume") {
    setVolume(data.toInt());
  } else if (command == "start_game") {
    startGame(data);
  } else if (command == "pause_game") {
    pauseGame();
  } else if (command == "resume_game") {
    resumeGame();
  } else if (command == "end_game") {
    endGame(data);
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

void playAudio(String fileName) {
  Serial.println("🎵 Próba odtworzenia: " + fileName);
  
  int fileNumber = getFileNumber(fileName);
  if (fileNumber > 0) {
    if (isPlayingAudio) {
      myDFPlayer.stop();
      delay(100);
    }
    
    myDFPlayer.play(fileNumber);
    isPlayingAudio = true;
    currentAudioFile = fileName;
    
    Serial.println("▶️ Odtwarzanie pliku " + String(fileNumber) + ": " + fileName);
    blinkLED(2, 150);
  } else {
    Serial.println("❌ Nieznany plik audio: " + fileName);
    MasterMessage msg;
    msg.command = "error";
    msg.data = "unknown_file:" + fileName;
    msg.timestamp = millis();
    sendToMaster(msg);
  }
}

int getFileNumber(String fileName) {
  if (fileName == "golab") return 1;
  if (fileName == "hint1") return 2;
  if (fileName == "hint2") return 2;
  return 1;
}

void stopAudio() {
  if (isPlayingAudio) {
    myDFPlayer.stop();
    isPlayingAudio = false;
    currentAudioFile = "";
    Serial.println("⏹️ Audio zatrzymane");
    
    MasterMessage msg;
    msg.command = "audio_finished";
    msg.data = "stopped";
    msg.timestamp = millis();
    sendToMaster(msg);
  }
}

void startGame(String groupName) {
  gameActive = true;
  gamePaused = false;
  currentGameGroup = groupName;
  Serial.println("🎮 Gra rozpoczęta: " + groupName);
  blinkLED(3, 500);
  
  MasterMessage msg;
  msg.command = "status";
  msg.data = "game_started:" + groupName;
  msg.timestamp = millis();
  sendToMaster(msg);
}

void pauseGame() {
  if (!gameActive) return;
  gamePaused = true;
  Serial.println("⏸️ Gra wstrzymana");
  if (isPlayingAudio) {
    myDFPlayer.pause();
  }
}

void resumeGame() {
  if (!gameActive) return;
  gamePaused = false;
  Serial.println("▶️ Gra wznowiona");
  if (isPlayingAudio) {
    myDFPlayer.start();
  }
}

void endGame(String status) {
  gameActive = false;
  gamePaused = false;
  currentGameGroup = "";
  Serial.println("🏁 Gra zakończona: " + status);
  
  if (isPlayingAudio) {
    myDFPlayer.stop();
    isPlayingAudio = false;
    currentAudioFile = "";
  }
  
  if (status == "completed") {
    blinkLED(10, 100);
  } else {
    blinkLED(3, 1000);
  }
  
  MasterMessage msg;
  msg.command = "status";
  msg.data = "game_ended:" + status;
  msg.timestamp = millis();
  sendToMaster(msg);
}

void sendHeartbeatToMaster() {
  MasterMessage msg;
  msg.command = "heartbeat";
  msg.data = "golab_alive";
  msg.timestamp = millis();
  sendToMaster(msg);
}

bool sendToMaster(MasterMessage& message) {
  String serialized = message.command + "|" + message.data + "|" + String(message.timestamp);
  uint8_t data[250];
  size_t len = serialized.length();
  if (len > 249) len = 249;
  serialized.getBytes(data, len + 1);
  
  esp_err_t result = esp_now_send(master_mac, data, len);
  if (result == ESP_OK) {
    return true;
  } else {
    Serial.println("❌ Błąd wysyłania do Master");
    return false;
  }
}

void setVolume(int volume) {
  volume = constrain(volume, 0, 30);
  myDFPlayer.volume(volume);
  Serial.println("🔊 Głośność ustawiona na: " + String(volume));
  
  MasterMessage msg;
  msg.command = "volume_set";
  msg.data = String(volume);
  msg.timestamp = millis();
  sendToMaster(msg);
}

void blinkLED(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    delay(delayMs);
  }
}