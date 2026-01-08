/*
The following system was developed to simulate the behavior of a smart study room monitoring system using an ESP32 microcontroller.
It integrates a system that can be implemented in educational buildings to guide their students across study facilities and find 
the best study environment for each.

ESP32 Study Room Monitoring System Features:
  - Temperature and Humidity Sensor (DHT11)
  - Noise Level Sensor (Analog)
  - People Counting using Two PIR Sensors
  - OLED Display (SSD1306)
  - MQTT Communication with Adafruit IO
  - Data published every 30 seconds displayd in Web dashboard (dasboard.html)
*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "DHT.h"
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

#define AIO_SERVER "io.adafruit.com"
#define AIO_SERVERPORT 1883
#define AIO_USERNAME "XXXXXXXXXXXXXXXXX"  // Replace with Adafruit IO Username
#define AIO_KEY "aio_XXXXXXXXXXXXXXXXXXXXXX"  // Replace with Adafruit IO Key

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RST 23   

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define TEMP_SENSOR_PIN 14
#define NOISE_SENSOR_PIN 35
#define MOTION_SENSOR_LEFT_PIN 36
#define MOTION_SENSOR_RIGHT_PIN 39 
#define DHTTYPE DHT11 

// Wifi / Hotspot Credentials
const char* ssid = "";      // network SSID
const char* password = "";  // network password

// Global Objects
WiFiClient wifiClient;
Adafruit_MQTT_Client mqtt(&wifiClient, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// Feed definitions
Adafruit_MQTT_Publish temperatureFeed = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/temperature");
Adafruit_MQTT_Publish humidityFeed = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/humidity");
Adafruit_MQTT_Publish noiseFeed = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/noise");
Adafruit_MQTT_Publish peopleCountFeed = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/people-count");

// People Counting Variables
int peopleCount = 0;
int lastLeftState = LOW;
int lastRightState = LOW;
unsigned long lastLeftTriggerTime = 0;
unsigned long lastRightTriggerTime = 0;
bool leftTriggeredFirst = false;
bool rightTriggeredFirst = false;
bool personCounted = false;

// Debounce and timing constants
const unsigned long TRIGGER_TIMEOUT = 3000;  // 2 seconds max between sensor triggers
const unsigned long DEBOUNCE_DELAY = 300;    // 200ms debounce delay
const unsigned long RESET_COOLDOWN = 1500;   // 1 second cooldown between counts

// Timer for sensor checks
unsigned long lastPeopleCheck = 0;
const unsigned long PEOPLE_CHECK_INTERVAL = 100;  // Check every 100ms

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);
DHT dht(TEMP_SENSOR_PIN, DHTTYPE);

// Timer for subscription check
unsigned long lastSubscriptionCheck = 0;
const unsigned long SUBSCRIPTION_CHECK_INTERVAL = 2000; // Check every 5 seconds

void connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Connecting WiFi...");
  display.display();

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    display.println(".");
    display.display();
  }

  display.clearDisplay();
  Serial.println("\nConnected to WiFi!");
  Serial.print("\nIP: ");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.println("\nConnected!");
  display.print("IP: ");
  display.println(WiFi.localIP());
  delay(1000);
  display.display();
}

void initializeOLED() {
  // Start I2C on the correct pins
  Wire.begin(OLED_SDA, OLED_SCL);

  // Reset OLED
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(20);
  digitalWrite(OLED_RST, HIGH);

  // Initialize SSD1306 OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    while (1);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("LILYGO LoRa3 / T3");
  display.println("---------------------");
  display.println("OLED working!"); 
  delay(2000);
  display.display();
}

void MQTT_connect() {
  int8_t ret;
  
  // Stop if already connected
  if (mqtt.connected()) {
    return;
  }

  Serial.print("Connecting to MQTT... ");
  
  uint8_t retries = 5;
  while ((ret = mqtt.connect()) != 0) {
    Serial.println(mqtt.connectErrorString(ret));
    Serial.println("Retrying MQTT connection in 5 seconds...");
    mqtt.disconnect();
    delay(5000);
    retries--;
    if (retries == 0) {
      Serial.println("MQTT connection failed after retries");
      return;
    }
  }
  
  Serial.println("MQTT connected!");
}

void checkPeopleCount() {
  unsigned long currentMillis = millis();
  
  // Read sensor states
  int leftState = digitalRead(MOTION_SENSOR_LEFT_PIN);   // Outside sensor
  int rightState = digitalRead(MOTION_SENSOR_RIGHT_PIN); // Inside sensor
  
  // Check for LEFT sensor trigger (Outside)
  if (leftState == HIGH && lastLeftState == LOW) {
    if (currentMillis - lastLeftTriggerTime > DEBOUNCE_DELAY) {
      Serial.println("LEFT (Outside) sensor triggered - Person detected OUTSIDE");
      lastLeftTriggerTime = currentMillis;
      
      // Check if this could be an EXIT (RIGHT triggered recently)
      if ((lastRightTriggerTime > 0) && 
          (currentMillis - lastRightTriggerTime < TRIGGER_TIMEOUT) &&
          !personCounted) {
        
        peopleCount--;
        if (peopleCount < 0) peopleCount = 0;
        Serial.print("Person EXITING. Count: ");
        Serial.println(peopleCount);
        publishPeopleCount();
        personCounted = true;
        delay(RESET_COOLDOWN);
      }
      // Otherwise, might be someone just passing outside
    }
  }
  
  // Check for RIGHT sensor trigger (Inside)
  if (rightState == HIGH && lastRightState == LOW) {
    if (currentMillis - lastRightTriggerTime > DEBOUNCE_DELAY) {
      Serial.println("RIGHT (Inside) sensor triggered - Person detected INSIDE");
      lastRightTriggerTime = currentMillis;
      
      // Check if this could be an ENTRY (LEFT triggered recently)
      if ((lastLeftTriggerTime > 0) && 
          (currentMillis - lastLeftTriggerTime < TRIGGER_TIMEOUT) &&
          !personCounted) {
        
        peopleCount++;
        Serial.print("Person ENTERING. Count: ");
        Serial.println(peopleCount);
        publishPeopleCount();
        personCounted = true;
        delay(RESET_COOLDOWN);
      }
      // Otherwise, might be someone just moving inside
    }
  }
  
  // Reset personCounted flag after timeout
  if (personCounted && (currentMillis - lastLeftTriggerTime > TRIGGER_TIMEOUT) &&
                      (currentMillis - lastRightTriggerTime > TRIGGER_TIMEOUT)) {
    personCounted = false;
  }
  
  lastLeftState = leftState;
  lastRightState = rightState;
}

void checkSubscription() {
  if (!mqtt.connected()) {
    Serial.println("MQTT not connected, skipping subscription check");
    return;
  }  
  
  mqtt.processPackets(100);
  
  // Read subscription with a reasonable timeout
  Adafruit_MQTT_Subscribe *subscription = mqtt.readSubscription(2000);
}

void getSensorData() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  
  if (isnan(t) || isnan(h)) {
    Serial.println("DHT error");
    return;
  }

  int tSensVal = (int)round(t);
  int hSensVal = (int)round(h);
  
  int noiseSensVal = analogRead(NOISE_SENSOR_PIN);

  Serial.printf("T = %d °C \nH = %d %% \nN = %d \nPpl= %d", tSensVal, hSensVal, noiseSensVal, peopleCount);
  
  // Display on OLED
  display.clearDisplay(); 
  display.setCursor(0,0); 
  display.print("Temp: "); display.print(tSensVal); display.println(" C"); 
  display.print("Hum:  "); display.print(hSensVal); display.println(" %");
  display.print("Noise: "); display.println(noiseSensVal);
  display.print("People: "); display.println(peopleCount);
  display.display();

  publishSensorData(tSensVal, hSensVal, noiseSensVal);
}

void publishSensorData(int32_t t, int32_t h, int32_t n) {
  if (!mqtt.connected()) {
    Serial.println("Cannot publish - MQTT not connected");
    return;
  }

  Serial.print("\nPublishing temperature... ");
  if (temperatureFeed.publish(t)) {
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }
  
  Serial.print("Publishing humidity... ");
  if (humidityFeed.publish(h)) {
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }
  
  Serial.print("Publishing noise... ");
  if (noiseFeed.publish(n)) {
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }
  
  Serial.println("Published to AIO: T=" + String(t) + " H=" + String(h) + " N=" + String(n));
}

void publishPeopleCount() {
  if (!mqtt.connected()) return;
  
  Serial.print("Publishing people count: ");
  Serial.println(peopleCount);
  
  // FIX: Explicitly cast to int32_t to resolve ambiguous overload
  if (peopleCountFeed.publish((int32_t)peopleCount)) {
    Serial.println("People count published");
  } else {
    Serial.println("Failed to publish people count");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\nStarting ESP32 MQTT Client...");
  
  dht.begin();
  pinMode(NOISE_SENSOR_PIN, INPUT);
  pinMode(MOTION_SENSOR_LEFT_PIN, INPUT);
  pinMode(MOTION_SENSOR_RIGHT_PIN, INPUT);  // Added missing pinMode

  initializeOLED();
  connectToWifi();
  
  // Initial MQTT connection
  MQTT_connect();

  // Publish initial count
  publishPeopleCount();
  
  Serial.println("Setup complete!");
}

void loop() {
  // Get current time once
  unsigned long currentMillis = millis();
  
  // Maintain MQTT connection
  if (!mqtt.connected()) {
    Serial.println("MQTT disconnected, reconnecting...");
    MQTT_connect();
  }
  
  // Check WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectToWifi();
    return;
  }

  // Check people count continuously
  if (currentMillis - lastPeopleCheck >= PEOPLE_CHECK_INTERVAL) {
    lastPeopleCheck = currentMillis;
    checkPeopleCount();
  }
  
  // Check for subscriptions periodically
  if (currentMillis - lastSubscriptionCheck >= SUBSCRIPTION_CHECK_INTERVAL) {
    lastSubscriptionCheck = currentMillis;
    // Serial.println("\n=== Checking subscription ===");
    checkSubscription();
  }
  
  // Get and publish sensor data every 30 seconds
  static unsigned long lastSensorPublish = 0;
  if (currentMillis - lastSensorPublish >= 30000) {
    lastSensorPublish = currentMillis;
    Serial.println("\n=== Getting sensor data ===");
    getSensorData();
  }
  
  // Keep MQTT connection alive
  mqtt.processPackets(100);
  
  // Small delay to prevent watchdog timer issues
  delay(10);
}