/*
  This file implements a wireless acces control system using ESP32. 
  It hosts a web server that allows users to log in with room ID and password.
  A servo motor simulates the door knob, and LEDs indicate success or failure.
  After 3 failed login attempts, the user is locked out for 2 minutes.
*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESP32Servo.h>

const char* ssid = "";      // network SSID
const char* password = "";  // network password

//pins numbers
int servoPin = 12;
int greenLedPin = 13;
int redLedPin = 21;

int numFailedAttempts = 0;
unsigned long lockoutStart = 0;
bool lockedOut = false;

WebServer server(80);

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
  if (server.hasArg("ROOMID") && server.hasArg("PASSWORD")) {
    if ((server.arg("ROOMID") == "admin" &&  server.arg("PASSWORD") == "admin") ||
        (server.arg("ROOMID") == "123" &&  server.arg("PASSWORD") == "gogo") || 
        (server.arg("ROOMID") == "456" &&  server.arg("PASSWORD") == "dalle") ||
        (server.arg("ROOMID") == "789" &&  server.arg("PASSWORD") == "okay")) {
      numFailedAttempts = 0;  // reset on success
      server.sendHeader("Location", "/");
      server.sendHeader("Cache-Control", "no-cache");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=1");
      server.send(301);
      Serial.println("Log in Successful");
      digitalWrite(greenLedPin, HIGH); delay(1000); //turn on green led
      digitalWrite(greenLedPin, LOW); delay(1000); 
      myservo.write(0); delay(1000); //activate motor to simulate door knob//activate motor to simulate door knob
      myservo.write(90); delay(1000);
      myservo.write(0); delay(1000); 
      return;
    }
    msg = "Wrong room id/password! try again.";
    numFailedAttempts++;
    Serial.println("Log in Failed");
    digitalWrite(redLedPin, HIGH); delay(1000); //turn on red led
    digitalWrite(redLedPin, LOW); delay(1000); 
    if (numFailedAttempts >= 3) {
      Serial.println("Login unsuccesful for consecutive 3 times. User suspended for 2 min");
      lockedOut = true; //navigate to error page
      lockoutStart = millis();
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

void setup() {
  pinMode(greenLedPin, OUTPUT);
  pinMode(redLedPin, OUTPUT);
  myservo.attach(servoPin);

  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());


  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/bad-auth", handleErrorPage);//declare error page
  server.on("/inline", []() {
    server.send(200, "text/plain", "this works without need of authentification");
  });

  server.onNotFound(handleNotFound);
  //here the list of headers to be recorded
  const char * headerkeys[] = {"User-Agent", "Cookie"} ;
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  //ask server to track these headers
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
}
