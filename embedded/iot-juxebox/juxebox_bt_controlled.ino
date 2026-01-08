/*
  This file extend juxebox_id_controlled.ino to modify the jukebox to be
  controlled via Bluetooth using a control pad from the BluefruitConnect app.

  ESP32 BLE Jukebox with Queue Management
  Code based off of ESP32 example sketch: BLE_uart.ino for BLE initialization and connection operations

  Controls description for the person running the code: UP button --> goes to Next Song, DOWN button --> Play/Pause toggle, LEFT button --> Previous song, RIGHT --> Next Song
*/

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFi.h>

BLEServer* pServer = NULL;
BLECharacteristic* pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool convert = false;
String rxString = "";

WiFiClientSecure client;
const char* host = "iotjukebox.onrender.com";
int port = 443;
const int buzzer = 21;

// WiFi credentials
const char* ssid = "";      // network SSID (name)
const char* password = "";  // network password

// Song playback state
std::vector<String> songQueue;
int currentSongIndex = -1;  // Current position in queue (-1 = no song)
bool isPlaying = false;
bool isPaused = false;
bool skipRequested = false;

unsigned long lastNoteTime = 0;  // Non-blocking song playback variables, gives precise stop and start
int currentNote = 0;
int* currentMelody = nullptr;
int currentMelodySize = 0;
int currentTempo = 0;
bool songActive = false;

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("Client connected...");
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("Client disconnected...");
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();

    if (rxValue.length() > 0) {
      convert = true;
      rxString = String(rxValue.c_str());

      Serial.println("*********");
      Serial.print("Received Value: ");
      Serial.println(rxString);
      Serial.println("*********");
    }
  }
};

String getRandomSong() {  // function to fetch a random song from the server
  client.setInsecure();
  String path = "/song";
  String songName = "";

  Serial.println("Fetching random song name...");

  if (client.connect(host, port)) {
    client.print(String("GET ") + path + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" + "Connection: close\r\n\r\n");

    String response = "";
    while (client.connected()) {
      while (client.available()) {
        response += client.readString();
      }
    }
    client.stop();

    int start = response.indexOf('{');
    int end = response.lastIndexOf('}');
    if (start != -1 && end != -1) {
      String json = response.substring(start, end + 1);
      DynamicJsonDocument doc(512);
      if (!deserializeJson(doc, json)) {
        songName = doc["name"].as<String>();
        Serial.println("Random song: " + songName);
      }
    }
  } else {
    Serial.println("Failed to connect to server");
  }
  return songName;
}

bool fetchSongDetails(String songName, int** melodyOut, int* sizeOut, int* tempoOut) {  // function to get song details and prepare for playback
  client.setInsecure();
  String path = "/song?name=" + songName;

  Serial.println("Fetching song details for: " + songName);

  if (client.connect(host, port)) {
    client.print(String("GET ") + path + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" + "Connection: close\r\n\r\n");

    String response = "";
    while (client.connected()) {
      while (client.available()) {
        response += client.readString();
      }
    }
    client.stop();

    int jsonStart = response.indexOf('{');
    int jsonEnd = response.lastIndexOf('}');
    if (jsonStart == -1 || jsonEnd == -1) {
      Serial.println("JSON not found in response...");
      return false;
    }

    String json = response.substring(jsonStart, jsonEnd + 1);
    Serial.println("Extracted JSON:");
    Serial.println(json);

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
      Serial.println("JSON parsing failed");
      return false;
    }

    *tempoOut = doc["tempo"].as<int>();
    JsonArray melodyArr = doc["melody"].as<JsonArray>();
    *sizeOut = melodyArr.size();
    *melodyOut = new int[*sizeOut];

    for (int i = 0; i < *sizeOut; i++) {
      (*melodyOut)[i] = melodyArr[i].as<int>();
    }

    Serial.println("Song details fetched successfully...");
    return true;
  }

  Serial.println("Failed to connect to server...");
  return false;
}

void startSongPlayback(int* melody, int size, int tempo) {  // Start playing a song (maybe again)
  if (currentMelody != nullptr) {                           // Stop current song if playing
    delete[] currentMelody;
    noTone(buzzer);
  }

  currentMelody = new int[size];
  memcpy(currentMelody, melody, size * sizeof(int));
  currentMelodySize = size;
  currentTempo = tempo;
  currentNote = 0;
  songActive = true;
  isPlaying = true;
  isPaused = false;
  skipRequested = false;
  lastNoteTime = millis();

  Serial.println("Started song playback...");
}

void updateSongPlayback() {                                   // function to update song playback
  if (!songActive || currentMelody == nullptr || isPaused) {  //wait until play button pressed
    return;
  }

  if (skipRequested) {  // Check if skip was requested
    noTone(buzzer);
    songActive = false;
    isPlaying = false;
    if (currentMelody != nullptr) {
      delete[] currentMelody;
      currentMelody = nullptr;
    }
    return;
  }

  if (currentNote >= currentMelodySize) {  // Check if song is finished
    noTone(buzzer);
    songActive = false;
    isPlaying = false;
    if (currentMelody != nullptr) {
      delete[] currentMelody;
      currentMelody = nullptr;
    }
    Serial.println("Song finished");
    return;
  }

  unsigned long currentTime = millis();
  int wholenote = (60000 * 4) / currentTempo;

  int note = currentMelody[currentNote];
  int divider = currentMelody[currentNote + 1];
  int noteDuration = 0;

  if (divider > 0) {  //performs calculations like in the given repo to play sound from exact moment
    noteDuration = wholenote / divider;
  } else if (divider < 0) {
    noteDuration = (wholenote / abs(divider)) * 1.5;
  }

  if (currentTime - lastNoteTime >= noteDuration) {
    noTone(buzzer);
    currentNote += 2;
    lastNoteTime = currentTime;

    if (currentNote < currentMelodySize && currentMelody[currentNote] > 0) {
      tone(buzzer, currentMelody[currentNote], noteDuration * 0.9);
    }
  }
}

void playNextSong() {    // function to play next song in queue
  skipRequested = true;  // Stop current song immediately
  delay(10);             // Brief delay to ensure current song stops

  currentSongIndex++;

  if (currentSongIndex >= songQueue.size()) {  //check if status is at end of queue, if yes fetch new song with method above
    String newSong = getRandomSong();
    if (newSong.length() > 0) {
      songQueue.push_back(newSong);
      Serial.println("Added new random song to queue: " + newSong);
    } else {
      Serial.println("Failed to get random song");
      currentSongIndex--;  // Revert index
      return;
    }
  }

  String songToPlay = songQueue[currentSongIndex];
  Serial.println("Playing song [" + String(currentSongIndex) + "]: " + songToPlay);

  int* melody;
  int size;
  int tempo;

  if (fetchSongDetails(songToPlay, &melody, &size, &tempo)) {
    startSongPlayback(melody, size, tempo);
    delete[] melody;  // startSongPlayback makes its own copy
  } else {
    Serial.println("Failed to fetch song details");
  }
}

void playPreviousSong() {  // Play previous song in queue
  if (currentSongIndex <= 0) {
    Serial.println("No previous song to play (at head of queue)");
    return;
  }
  int* melody; int size; int tempo;

  fetchSongDetails(songQueue[currentSongIndex], &melody, &size, &tempo);
  
  if(currentNote <= currentMelodySize && currentNote > 5) {   //Spotify feature (bonus marks)
    startSongPlayback(melody, size, tempo);
    Serial.println("More than 5 notes played, replay sound...");
    return;
  }


  skipRequested = true;  // Stop current song immediately
  delay(10);

  currentSongIndex--;
  String songToPlay = songQueue[currentSongIndex];
  Serial.println("Playing previous song [" + String(currentSongIndex) + "]: " + songToPlay);

  if (fetchSongDetails(songToPlay, &melody, &size, &tempo)) {
    startSongPlayback(melody, size, tempo);
    delete[] melody;
  } else {
    Serial.println("Failed to fetch song details");
  }
}

void togglePlayPause() {                       // Toggle play/pause
  if (!isPlaying && currentSongIndex == -1) {  //check if it is the first song to be played by jukebox
    Serial.println("Starting first song...");
    playNextSong();
    return;
  }

  if (isPaused) {
    // Resume playback
    isPaused = false;
    Serial.println("Resumed playback");
  } else {
    // Pause playback
    isPaused = true;
    noTone(buzzer);
    Serial.println("Paused playback");
  }
}

void convertControlpad() {  // Convert control pad input to function calls

  convert = false;

  if (rxString == "!B516") {
    Serial.println("********** UP Button - Next Song");
    playNextSong();
  } else if (rxString == "!B615") {
    Serial.println("********** DOWN Button - Play/Pause");
    togglePlayPause();
  } else if (rxString == "!B714") {
    Serial.println("********** LEFT Button - Previous Song");
    playPreviousSong();
  } else if (rxString == "!B813") {
    Serial.println("********** RIGHT Button - Next Song");
    playNextSong();
  }

  rxString = "";
}

void setupWifi() {
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  pinMode(buzzer, OUTPUT);

  setupWifi();

  BLEDevice::init("Irina's ESP32 Service");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  pServer->getAdvertising()->start();
  Serial.println("Waiting for client connection...");

  if (WiFi.status() == WL_CONNECTED) {  //initialize queue
    String firstSong = getRandomSong();
    if (firstSong.length() > 0) {
      songQueue.push_back(firstSong);
      currentSongIndex = 0;
      Serial.println("Queue initialized with: " + firstSong);

      int* melody;
      int size;
      int tempo;
      if (fetchSongDetails(firstSong, &melody, &size, &tempo)) {
        startSongPlayback(melody, size, tempo);
        delete[] melody;  //melody removed each time, otherwise program takes too much space on chip
      }
    }
  }
}

void loop() {
  updateSongPlayback();

  if (convert) {
    convertControlpad();
  }

  if (!isPlaying && !isPaused && currentSongIndex >= 0) {
    Serial.println("Song finished, playing next...");
    playNextSong();
  }

  if (deviceConnected) {
    delay(20);  // Small delay for stability
  }

  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("Start advertising");
    oldDeviceConnected = deviceConnected;
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
}