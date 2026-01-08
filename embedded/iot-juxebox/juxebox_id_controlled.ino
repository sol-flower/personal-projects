/*
  This file contains the code for the implementation of a personal Jukebox system using
  an ESP32 microcontroller. The system plays a song based on the
  preference of the user in the room using a song data API.
*/

#include <BluetoothSerial.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` and enable it
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Bluetooth not available or not enabled. It is only available for the ESP32 chip.
#endif

// Global variables
BluetoothSerial SerialBT;
#define BT_DISCOVER_TIME 10000
String btDeviceName;  // stores BT device name
String loggedUser;
String song;
const int buzzer = 21;


const char* ssid = "";      // network SSID (name)
const char* password = "";  // network password
const char* host = "iotjukebox.onrender.com";
int port = 443; 
WiFiClientSecure client;

// Bluetooth Setup 
void setupBluetooth() {
  SerialBT.begin("LILYGO ESP BT");  // Bluetooth device name
  Serial.println("The device started, pair it with Bluetooth!");

  BTScanResults* pResults = SerialBT.discover(BT_DISCOVER_TIME);
  if (pResults) {
    int count = pResults->getCount();
    Serial.print("Number of devices found: ");
    Serial.println(count);

    for (int i = 0; i < count; i++) {
      BTAdvertisedDevice* device = pResults->getDevice(i);
      btDeviceName = String(device->getName().c_str());

      if (btDeviceName.length() > 0) {
        Serial.print("Device: ");
        Serial.println(btDeviceName);
      }
    }

    Serial.print("Registered device: ");
    Serial.println(btDeviceName);
  } else {
    Serial.println("Error on BT Scan, no result!");
  }
}

// WiFi Setup 
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

// Identify User 
void idUser() {
  if (btDeviceName == "Irina’s iPhone (2)") {
    loggedUser = "Irina";
  } else if (btDeviceName == "Irina’s MacBook Pro") {
    loggedUser = "Eric";
  } else {
    Serial.println("Device not reconized");
  }

  Serial.print("Welcome ");
  Serial.println(loggedUser);
}
//Function to fix url paths
//This function was completed with the help of AI
String urlEncode(const String& str) {
  String encoded = "";
  char c;
  char buf[4];
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      sprintf(buf, "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

//  GET Song Request
void idUserSong() {
  client.setInsecure();
  String encodedName = urlEncode(btDeviceName);
  String path = "/preference?id=40248017&key=" + encodedName;

  Serial.println("Fetching user's favorite song...");
  Serial.println("Requesting: " + String(host) + path);

  if (client.connect(host, port)) {
    client.print(String("GET ") + path + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" + "Connection: close\r\n\r\n");

    while (client.connected()) {
      while (client.available()) {
        song = client.readString();
        Serial.println(song);   //not processed output, raw json
      }
    }
    client.stop();

    int jsonStart = song.indexOf('{');    //extract only JSON piece from payload
    int jsonEnd = song.lastIndexOf('}');
    if (jsonStart == -1 || jsonEnd == -1) {
      Serial.println("JSON not found in response!");
      Serial.println(song);
      return;
    }
    String json = song.substring(jsonStart, jsonEnd + 1);
    Serial.println("Extracted JSON:");
    Serial.println(json);

    DynamicJsonDocument doc(256);     //extract song name 
    DeserializationError error = deserializeJson(doc, json);
    if(error) {
      Serial.println("JSON parsing failed");
      return;
    }
    song = doc["name"].as<String>();
    Serial.println(song);   //song contains correctly processed song name

  } else {
    Serial.println("Connection to server failed!");
  }
}

void getSongDetailsAndPlay() {
  String songDetails; 
  client.setInsecure();
  String path = "/song?name=" + song;

  Serial.println("Fetching " + song + " song details...");
  Serial.println("Requesting: " + String(host) + path);

  if (client.connect(host, port)) {
    client.print(String("GET ") + path + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" + "Connection: close\r\n\r\n");

    while (client.connected()) {
      while (client.available()) {
        songDetails = client.readString();
        Serial.println(songDetails);   //not processed output, raw json
      }
    }
    client.stop();

    int jsonStart = songDetails.indexOf('{');    //extract only JSON piece from payload
    int jsonEnd = songDetails.lastIndexOf('}');
    if (jsonStart == -1 || jsonEnd == -1) {
      Serial.println("JSON not found in response!");
      Serial.println(songDetails);
      return;
    }
    String json = songDetails.substring(jsonStart, jsonEnd + 1);
    Serial.println("Extracted JSON:");
    Serial.println(json);

    DynamicJsonDocument doc(1024);     
    DeserializationError error = deserializeJson(doc, json);
    if(error) {
      Serial.println("JSON parsing failed");
      return;
    }
    int tempo = doc["tempo"].as<int>();
    JsonArray melodyArr = doc["melody"].as<JsonArray>();
    int* melody = new int[melodyArr.size()];
    for (int i = 0; i < melodyArr.size(); i++) {
      melody[i] = melodyArr[i].as<int>();
    }
    playSong(melody, melodyArr.size(), tempo);

    delete[] melody;
  }
}

void playSong(int* melody, int size, int tempo) {
  int wholenote = (60000 * 4) / tempo;
  int divider = 0, noteDuration = 0;

  Serial.println("Playing sound...");

  for (int thisNote = 0; thisNote + 1 < size; thisNote = thisNote + 2) {
    int note = melody[thisNote];
    divider = melody[thisNote + 1];
    if (divider > 0) {
      noteDuration = (wholenote) / divider;
    } else if (divider < 0) {
      noteDuration = (wholenote) / abs(divider);
      noteDuration *= 1.5; 
    }
    if (note > 0) { 
      tone(buzzer, melody[thisNote], noteDuration * 0.9);
    }
    delay(noteDuration);
    noTone(buzzer);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(buzzer, OUTPUT);
  setupBluetooth();
  setupWifi();
  idUser();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    idUserSong();
    getSongDetailsAndPlay();
  } else {
    Serial.println("WiFi Disconnected");
  }

  delay(600000); // 10 minutes between requests
}
