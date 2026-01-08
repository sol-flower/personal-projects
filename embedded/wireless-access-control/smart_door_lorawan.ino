/*
  This file takes the system from smart_door_wifi.ino and adds LoRaWAN for additional security mechanism.
  Upon previously implemented wireless access control system, the system identifies proximity of a human
  using a distance sensor. Only when both the room credentials are correct and a human is detected
  within a certain range, the door is unlocked. Moreover, all successful and failed login attempts 
  are reported via LoRaWAN to The Things Network.
*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <vector>

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>

#ifdef COMPILE_REGRESSION_TEST
# define FILLMEIN 0
#else
# warning "You must replace the values marked FILLMEIN with real values from the TTN control panel!"
# define FILLMEIN (#dont edit this, edit the lines that use FILLMEIN)
#endif

const char* ssid = "";      // network SSID
const char* password = "";  // network password

//pins numbers
int servoPin = 12;
int greenLedPin = 13;
int redLedPin = 21;
int sensorPin = 34;

int numFailedAttempts = 0;
unsigned long lockoutStart = 0;
bool lockedOut = false;
int sensorValue = 0;

static const u1_t PROGMEM APPEUI[8]={  };   // Update with AppEUI from TTN
void os_getArtEui (u1_t* buf) { memcpy_P(buf, APPEUI, 8);}

static const u1_t PROGMEM DEVEUI[8]={  };   // Update with DevEUI from TTN
void os_getDevEui (u1_t* buf) { memcpy_P(buf, DEVEUI, 8);}  

static const u1_t PROGMEM APPKEY[16] = {  };    // Update with AppKey from TTN
void os_getDevKey (u1_t* buf) {  memcpy_P(buf, APPKEY, 16);}

static uint8_t mydata[] = "Hello, world!";
static osjob_t sendjob;

const unsigned TX_INTERVAL = 60;

const lmic_pinmap lmic_pins = {
 .nss = 18,
 .rxtx = LMIC_UNUSED_PIN,
 .rst = 23,
 .dio = {/*dio0*/ 26, /*dio1*/ 33, /*dio2*/ 32}
};

WebServer server(80);

struct Credential {
  String roomid;
  String password;
};

std::vector<Credential> credentials = {
  {"123", "gogo"},
  {"456", "dalle"},
  {"789", "okay"}
};

Servo myservo;

//Check if header is present and correct
bool is_authentified() {
  Serial.println("Enter is_authentified");
  if (server.hasHeader("Cookie")) {
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
    if (cookie.indexOf("ESPSESSIONID=1") != -1) {
      Serial.println("Authentification Successful");
      return true;
    }
  }
  Serial.println("Authentification Failed");
  return false;
}

//login page, also called for disconnect
void handleLogin() {
  if (lockedOut) {    //if user locked out, redirect
    server.sendHeader("Location", "/bad-auth");
    server.send(303);
    return;
  }

  String msg;

  sensorValue = analogRead(sensorPin); delay(50); //fetch the sensor data
  Serial.print("Sensor value: "); Serial.println(sensorValue);

  if (server.hasHeader("Cookie")) {
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
  }
  if (server.hasArg("DISCONNECT")) {
    Serial.println("Disconnection");
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
    server.send(301);
    return;
  }
  bool goodCredentials = false;
  if (server.hasArg("ROOMID") && server.hasArg("PASSWORD")) {
    for (auto &c : credentials) {
      if (c.roomid == server.arg("ROOMID") && c.password == server.arg("PASSWORD")) { goodCredentials = true; break; }
    }
    
    if (goodCredentials && sensorValue >= 2000) { //case 1 good credentials, and human
      numFailedAttempts = 0;  // reset on success
      server.sendHeader("Location", "/");
      server.sendHeader("Cache-Control", "no-cache");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=1");
      server.send(301);
      Serial.println("Good credentials and human present - Log in Successful");
      digitalWrite(greenLedPin, HIGH); delay(1000); //turn on green led
      digitalWrite(greenLedPin, LOW); delay(1000); 
      myservo.write(0); delay(1000); //activate motor to simulate door knob//activate motor to simulate door knob
      myservo.write(90); delay(1000);
      myservo.write(0); delay(1000); 
      sendLoginEvent("LoginSuccess");
      return;
    }
    if (goodCredentials && sensorValue < 2000) {  //case 2 good credentials, no human
      Serial.println("Good credentials but no human present - Log in Failed");
      msg = "Room credentials are good, but no human was detected...";
      numFailedAttempts++;
      digitalWrite(redLedPin, HIGH); delay(1000); //turn on red led
      digitalWrite(redLedPin, LOW); delay(1000);
      sendLoginEvent("LoginFail_NoHuman");
    }
    if (!goodCredentials && sensorValue >= 2000) {  //case 3 bad credentials, and human
      Serial.println("Wrong credentials but human present - Log in Failed");
      msg = "Wrong room credentials! Try again.";
      numFailedAttempts++;
      digitalWrite(redLedPin, HIGH); delay(1000); //turn on red led
      digitalWrite(redLedPin, LOW); delay(1000);
      sendLoginEvent("LoginFail_BadCreds"); 
    }

    if (!goodCredentials && sensorValue < 2000) { //case 4 bad credentials and no human
      msg = "Wrong room credentials and no human detected! Try again.";
      numFailedAttempts++;
      Serial.println("Log in Failed");
      digitalWrite(redLedPin, HIGH); delay(1000); //turn on red led
      digitalWrite(redLedPin, LOW); delay(1000); 
      sendLoginEvent("LoginFail_BadCreds_NoHuman");
    }

    if (numFailedAttempts >= 3) {
      Serial.println("Login unsuccesful for consecutive 3 times. User suspended for 2 min");
      lockedOut = true; //navigate to error page
      lockoutStart = millis();
      sendLoginEvent("User banned, unsuccessful login attempts > 3");
      server.sendHeader("Location", "/bad-auth");
      server.send(303);
      return;
    }
  }
  String content = "<html><body><form action='/login' method='POST'>Welcome to the super secret room <3<br><br>";
  content += "Room Number:<input type='text' name='ROOMID' placeholder='room_id'><br><br>";
  content += "Password:<input type='password' name='PASSWORD' placeholder='password'><br><br>";
  content += "<input type='submit' name='SUBMIT' value='Submit'></form>" + msg + "<br>";
  server.send(200, "text/html", content);
}

//root page can be accessed only if authentification is ok
void handleRoot() {
  Serial.println("Enter handleRoot");
  String header;
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  String content = "<html><body><H2>Hello:) You successfully entered the room!</H2><br>";
  content += "<a href=\"/login?DISCONNECT=YES\">Leave the room</a></body></html>";
  server.send(200, "text/html", content);
}

//no need authentification
void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void handleErrorPage() {
  String message = "<html><body><h2>Access temporarily blocked</h2>"
                "<meta http-equiv='refresh' content='120;url=/login'>"
                "<p>You tried to log in unsuccessfully 3 times.</p>"
                "<p>Please wait 2 minutes before trying again...</p></body></html>";
  server.send(200, "text/html", message);
}

void printHex2(unsigned v) {
    v &= 0xff;
    if (v < 16)
        Serial.print('0');
    Serial.print(v, HEX);
}

void onEvent (ev_t ev) {
    Serial.print(os_getTime());
    Serial.print(": ");
    switch(ev) {
        case EV_SCAN_TIMEOUT:
            Serial.println(F("EV_SCAN_TIMEOUT"));
            break;
        case EV_BEACON_FOUND:
            Serial.println(F("EV_BEACON_FOUND"));
            break;
        case EV_BEACON_MISSED:
            Serial.println(F("EV_BEACON_MISSED"));
            break;
        case EV_BEACON_TRACKED:
            Serial.println(F("EV_BEACON_TRACKED"));
            break;
        case EV_JOINING:
            Serial.println(F("EV_JOINING"));
            break;
        case EV_JOINED:
            Serial.println(F("EV_JOINED"));
            {
              u4_t netid = 0;
              devaddr_t devaddr = 0;
              u1_t nwkKey[16];
              u1_t artKey[16];
              LMIC_getSessionKeys(&netid, &devaddr, nwkKey, artKey);
              Serial.print("netid: ");
              Serial.println(netid, DEC);
              Serial.print("devaddr: ");
              Serial.println(devaddr, HEX);
              Serial.print("AppSKey: ");
              for (size_t i=0; i<sizeof(artKey); ++i) {
                if (i != 0)
                  Serial.print("-");
                printHex2(artKey[i]);
              }
              Serial.println("");
              Serial.print("NwkSKey: ");
              for (size_t i=0; i<sizeof(nwkKey); ++i) {
                      if (i != 0)
                              Serial.print("-");
                      printHex2(nwkKey[i]);
              }
              Serial.println();
            }

            LMIC_setLinkCheckMode(0);
            break;
        case EV_JOIN_FAILED:
            Serial.println(F("EV_JOIN_FAILED"));
            break;
        case EV_REJOIN_FAILED:
            Serial.println(F("EV_REJOIN_FAILED"));
            break;
        case EV_TXCOMPLETE:
            Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));
            if (LMIC.txrxFlags & TXRX_ACK)
              Serial.println(F("Received ack"));
            if (LMIC.dataLen) {
              Serial.print(F("Received "));
              Serial.print(LMIC.dataLen);
              Serial.println(F(" bytes of payload"));
            }
            // Schedule next transmission
            os_setTimedCallback(&sendjob, os_getTime()+sec2osticks(TX_INTERVAL), do_send);
            break;
        case EV_LOST_TSYNC:
            Serial.println(F("EV_LOST_TSYNC"));
            break;
        case EV_RESET:
            Serial.println(F("EV_RESET"));
            break;
        case EV_RXCOMPLETE:
            // data received in ping slot
            Serial.println(F("EV_RXCOMPLETE"));
            break;
        case EV_LINK_DEAD:
            Serial.println(F("EV_LINK_DEAD"));
            break;
        case EV_LINK_ALIVE:
            Serial.println(F("EV_LINK_ALIVE"));
            break;
        case EV_TXSTART:
            Serial.println(F("EV_TXSTART"));
            break;
        case EV_TXCANCELED:
            Serial.println(F("EV_TXCANCELED"));
            break;
        case EV_RXSTART:
            /* do not print anything -- it wrecks timing */
            break;
        case EV_JOIN_TXCOMPLETE:
            Serial.println(F("EV_JOIN_TXCOMPLETE: no JoinAccept"));
            break;

        default:
            Serial.print(F("Unknown event: "));
            Serial.println((unsigned) ev);
            break;
    }
}

void do_send(osjob_t* j){
    // Check if there is not a current TX/RX job running
    if (LMIC.opmode & OP_TXRXPEND) {
        Serial.println(F("OP_TXRXPEND, not sending"));
    } else {
        // Prepare upstream data transmission at the next possible time.
        LMIC_setTxData2(1, mydata, sizeof(mydata)-1, 0);
        Serial.println(F("Packet queued"));
    }
    // Next TX is scheduled after TX_COMPLETE event.
}

void sendLoginEvent(String statusMessage) {
  if (LMIC.opmode & OP_TXRXPEND) {
    Serial.println(F("LoRa busy, cannot send login event now"));
    return;
  }

  // Convert message to bytes
  uint8_t payload[50];
  int len = statusMessage.length();
  len = (len > 50) ? 50 : len;
  memcpy(payload, statusMessage.c_str(), len);

  // Send via LoRa
  LMIC_setTxData2(1, payload, len, 0);
  Serial.print(F("Login event sent over LoRa: "));
  Serial.println(statusMessage);
}

void setup() {
  // 1. Initialize hardware pins first
  pinMode(greenLedPin, OUTPUT);
  pinMode(redLedPin, OUTPUT);
  pinMode(sensorPin, INPUT);
  myservo.attach(servoPin);

  // 2. Initialize Serial
  Serial.begin(115200);
  delay(1000); // Give serial time to initialize

  // 3. Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  Serial.println("Connecting to WiFi...");
  Serial.print("SSID: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    if (attempts % 10 == 0) {
      Serial.println();
      Serial.print("Still trying... Status: ");
      Serial.println(WiFi.status());
    }
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi Connection FAILED!");
    Serial.print("Final status: ");
    Serial.println(WiFi.status());
  }

  // 4. Initialize LoRaWAN AFTER WiFi is connected
  Serial.println("Initializing LoRaWAN...");
  os_init();
  LMIC_reset();
  LMIC_selectSubBand(1);
  LMIC_startJoining();
  Serial.println("Attempting to join LoRaWAN network...");

  // 5. Start web server
  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/bad-auth", handleErrorPage);
  server.on("/inline", []() {
    server.send(200, "text/plain", "this works without need of authentification");
  });

  server.onNotFound(handleNotFound);
  
  const char * headerkeys[] = {"User-Agent", "Cookie"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  delay(2);//allow the cpu to switch to other tasks

  //lockout procedure checks for timer end
  if(lockedOut && millis() - lockoutStart > 120000) {
    lockedOut = false;
    numFailedAttempts = 0;
    Serial.println("Lockout expired, login enabled");
  }
  os_runloop_once();
}