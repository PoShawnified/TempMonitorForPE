// Our custom Config
#include "Config.h"
WiFiConfig wifiConfig;
HttpsConfig httpsConfig;

// Generic
#include <math.h>

// ESP Specifics
#include <ESP8266WiFi.h>
#include <ESPHTTPClient.h>
#include <JsonListener.h>

// Wifi client for HTTPS requests
#include <WiFiClientSecure.h>

// Time
#include <time.h>                       // time() ctime()
#include <sys/time.h>                   // struct timeval
#include <coredecls.h>                  // settimeofday_cb()

// OLED Display
#include "Wire.h"
#include "SSD1306Wire.h"
#include "OLEDDisplayUi.h"

// Graphics
#include "Fonts.h"
#include "Images.h"

// DS18B20 Sensor library
#include <OneWire.h>

/*
  Future Expansion(??):

  While we're CONNECTED to WiFi:
    - Store 10 values, at a rate of 60 seconds, to a ring buffer
    - Alert if 3, or more, values are below the threshold


  While we're DISconnected from WiFi:
    - Store 96 values, at a rate of 900 seconds (15 min), to a ring buffer
*/


/***************************
 * Begin Settings
 **************************/

// WIFI
char* WIFI_SSID = wifiConfig.ssid();
char* WIFI_PWD = wifiConfig.password();

#define TZ              -7       // (utc+) TZ in hours
#define DST_MN          60      // use 60mn for summer time in some countries

// Display Settings
const int I2C_DISPLAY_ADDRESS = 0x3c;
// Pins for OLED Display
const int SDA_PIN = D3;
const int SDC_PIN = D4;
// Initialize the oled display for address 0x3c
SSD1306Wire     display(I2C_DISPLAY_ADDRESS, SDA_PIN, SDC_PIN);

// Set up sensor I/O pins and vars
const int sensor1 = D1;
const int sensor2 = D6;
int tempCheckWaitMillis;
float s1Reading = 0;
float s2Reading = 0;

// Temp we need to alert at
const float tempThreshold = 35.00;
// How long we need to be out of spec before we alert (milli * seconds)
const int maxTempOutOfSpecTime = 1000 * 10;
unsigned long triggeredTempMillis;  // Var to hold and compare timespans
// How long to wait between alerts (milli * seconds)
const int alertInterval = 1000 * 10;
unsigned long triggeredAlertMillis;  //Var to hold and compare timespans

// Define display timeout and current frame vars (milli * seconds)
const unsigned long maxDisplayOnMillis = 1000 * 15;
unsigned long displayOnMillis;  //Var to hold and compare timespans

// Vars for sending test notification (milli * seconds)
const int buttonHoldActionMillis = 1000 * 4;
int testNotificationSent = 0;

// Var for ESP.restart();
const int buttonHoldRestartMillis = 1000 * 20;

// Timezone DST stuff
#define TZ_MN           ((TZ)*60)
#define TZ_SEC          ((TZ)*3600)
#define DST_SEC         ((DST_MN)*60)
time_t now;
const String WDAY_NAMES[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const String MONTH_NAMES[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// HTTPS Client - Host name, port and SHA1 fingerprint of the certificate
const char* IFTTT_Host    = "maker.ifttt.com";
const int httpsPort       = 443; 
String API_KEY            = httpsConfig.apikey();
String IFTTT_ALERT        = httpsConfig.iftttalert();
String IFTTT_NOTIFICATION = httpsConfig.iftttnotification();
char* fingerprint         = httpsConfig.fingerprint();

// HTTP SERVER port and var to store the HTTP request 
WiFiServer server(80);
String header;

// vars for button pin and status
const int buttonPin = D2;
int buttonState = 0;
int buttonLockout = 0;
/***************************
 * End Settings
 **************************/

void setup() {
  Serial.begin(115200);

  // initialize dispaly
  display.init();
  display.clear();
  display.display();

  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setContrast(255);

  // Disable the Soft AP functionality
  WiFi.enableAP(false);

  // Start Wifi
  WiFi.begin(WIFI_SSID, WIFI_PWD);

  int counter = 0;
  // This one is just for testing without WiFi... On startup we *must*
  //   wait for WiFi in order to get the NTP time data we'll need later. 
  //while (WiFi.status() != WL_CONNECTED && counter <= 60) {
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    display.clear();
    display.drawString(64, 10, "Connecting to WiFi");
    display.drawXbm(46, 30, 8, 8, counter % 3 == 0 ? activeSymbole : inactiveSymbole);
    display.drawXbm(60, 30, 8, 8, counter % 3 == 1 ? activeSymbole : inactiveSymbole);
    display.drawXbm(74, 30, 8, 8, counter % 3 == 2 ? activeSymbole : inactiveSymbole);
    display.display();

    counter++;
  }

  // Print local IP address and start web server
  Serial.println();
  Serial.println("WiFi connected.");
  Serial.println("IP address: " + WiFi.localIP().toString());
  server.begin();

  // Get time from network time service 
  //(216.239.35.8 = "time.google.com" in case we can't resolve DNS)
  configTime(TZ_SEC, DST_SEC, "pool.ntp.org", "time.nist.gov", "216.239.35.8");

  // Init the pushbutton input:
  pinMode(buttonPin, INPUT);

  // Shut off the display to save power until the button is pressed
  display.displayOff();

  Serial.println("Setup - Complete. Entering Loop...");
}


void loop() {
  /**********************************************************
   *   WEB SERVER
   * ********************************************************/
  // Listen for incoming clients
  WiFiClient webClientConnection = server.available(); 
  // If a new client connects, kick off a function to send page data
  if (webClientConnection) {sendPage(webClientConnection);}


  /**********************************************************
   *   TEMP SENSOR
   * ********************************************************/
  if (millis() - tempCheckWaitMillis > 1000){
    // Get the current sensor readings
    s1Reading = getTemp(sensor1);
    s2Reading = getTemp(sensor2);
    tempCheckWaitMillis = millis();
  }

  // If either sensor is over the threshold, start checking and reporting
  if (s1Reading < tempThreshold || s2Reading < tempThreshold){
    // If this is the first time we've been out of spec, record the start time.
    if (triggeredTempMillis == 0){triggeredTempMillis = millis();}
    
    // If we're over the max out of spec time
    if (millis() - triggeredTempMillis >= maxTempOutOfSpecTime){
      // If the time between the last triggered time and the current time 
      // is greater than the max time between triggers trigger again.
      if (triggeredAlertMillis == 0){
        //Serial.println("Temperature alert! " + String(s1Reading) + " : " + String(s2Reading));
        triggeredAlertMillis = millis();
        // POST to maker.ifttt.com
        postIFTTT(IFTTT_ALERT, "Temperature alert!", s1Reading, s2Reading);
      }
      else if (millis() - triggeredAlertMillis >= alertInterval){
        //Serial.println("Followup temperature alert! " + String(s1Reading) + " : " + String(s2Reading));
        triggeredAlertMillis = millis();
        // POST to maker.ifttt.com
        postIFTTT(IFTTT_ALERT, "Followup temperature alert!", s1Reading, s2Reading);
      }
    }
  }
  // If the sensors have moved to a non-alert state, and we perviously alerted, 
  // send an "all clear" alert and reset the triggeredalertMillis
  else if (s1Reading >= tempThreshold && s2Reading >= tempThreshold && triggeredAlertMillis > 0){
    postIFTTT(IFTTT_NOTIFICATION, "Normal temperature resumed.", s1Reading, s2Reading);
    triggeredAlertMillis = 0;
  }
  else {
    // Remove the timers
    triggeredTempMillis = 0;
    triggeredAlertMillis = 0;
  }

  /**********************************************************
   *   BUTTON STATE / ACTIONS
   * ********************************************************/
  // read the state of the pushbutton value:
  buttonState = digitalRead(buttonPin);

  // If the button was pressed and we're not in lockout 
  // (e.g. the first press, not subsequent readings while the button was in the down state)
  if (buttonState && !buttonLockout){
    Serial.println("Button Dn (no Lockout): " + String(buttonState));

    // Start Timer... If we're just turning the display on set the "display on time" time to 
    // the current millis time and turn on the display
    displayOnMillis = millis();
    display.displayOn();
    Serial.println("Display On!");

    // Start the lockout timer
    buttonLockout = millis();
  }
  // If the button was held down, here's where we'll catch the additional readings
  else if (buttonState && buttonLockout){
    // If the button was held longer than required for the secondary action, 
    // and we haven't already sent one, send a test notification.
    if ((millis() - buttonLockout > buttonHoldActionMillis) && !testNotificationSent){
      Serial.println("Button Dn ( + Lockout): Send Alert!");

      // Send a test alert
      postIFTTT(IFTTT_NOTIFICATION, "Test Notification.", 0.00, 0.00);

      // Flag that we've sent an notification for this button hold event.
      testNotificationSent = 1;
    }

    // Soft Restart 
    if ((millis() - buttonLockout > buttonHoldRestartMillis)){
      // Send a test alert
      postIFTTT(IFTTT_NOTIFICATION, "Soft Restart Called.", 0.00, 0.00);

      // Call Restart
      ESP.restart();
    }
  }
  // We're no longer in the button down state, reset the lockout and notification flag.
  else {
    buttonLockout = 0;
    testNotificationSent = 0;
  }

  // Turn off the display if it's been on longer than max "on time"
  if (millis() - displayOnMillis > maxDisplayOnMillis){
    // Turn the display off
    display.displayOff();

    //Reset the display on time to 0
    displayOnMillis = 0;
  }

  // Update the display info
  drawInfoGrid();
}

// Function to return temp from a sensor given it's I/O pin
float getTemp(int sensor_pin) {
  // OneWire DS18S20, DS18B20, DS1822 Temperature Example
  //
  // http://www.pjrc.com/teensy/td_libs_OneWire.html
  //
  // The DallasTemperature library can do all this work for you!
  // https://github.com/milesburton/Arduino-Temperature-Control-Library
  
  // (a 4.7K resistor is necessary)
  OneWire  sensor(sensor_pin);

  byte i;
  byte present = 0;
  byte type_s;
  byte data[12];
  byte addr[8];
  float celsius, fahrenheit;
  
  if (!sensor.search(addr)) {
    //Serial.println("No more addresses.");
    //Serial.println();
    sensor.reset_search();
    delay(250);
    return 0;
  }

  if (OneWire::crc8(addr, 7) != addr[7]) {
      Serial.println("CRC is not valid!");
      return 0;
  }

  sensor.reset();
  sensor.select(addr);
  sensor.write(0x44, 0);        // start conversion, with parasite power OFF at the end
  
  //delay(1000);     // maybe 750ms is enough, maybe not
  // we might do a sensor.depower() here, but the reset will take care of it.
  
  present = sensor.reset();
  sensor.select(addr);    
  sensor.write(0xBE);         // Read Scratchpad

  // we need 9 bytes
  for ( i = 0; i < 9; i++) { 
    data[i] = sensor.read();
  }

  // Convert the data to actual temperature
  // because the result is a 16 bit signed integer, it should
  // be stored to an "int16_t" type, which is always 16 bits
  // even when compiled on a 32 bit processor.
  int16_t raw = (data[1] << 8) | data[0];
  if (type_s) {
    raw = raw << 3; // 9 bit resolution default
    if (data[7] == 0x10) {
      // "count remain" gives full 12 bit resolution
      raw = (raw & 0xFFF0) + 12 - data[6];
    }
  } else {
    byte cfg = (data[4] & 0x60);
    // at lower res, the low bits are undefined, so let's zero them
    if (cfg == 0x00) raw = raw & ~7;  // 9 bit resolution, 93.75 ms
    else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
    else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
    //// default is 12 bit resolution, 750 ms conversion time
  }
  celsius = (float)raw / 16.0;
  fahrenheit = celsius * 1.8 + 32.0;
  return fahrenheit;
}

void postIFTTT(String iftttAction, char* strMessage, float s1Reading, float s2Reading){ 
  
  String IFTTT_URI = "/trigger/" + iftttAction + "/with/key/";

  Serial.println("========== postIFTTT() ==========");
  // Add the time to the right side of the display
  now = time(nullptr);
  struct tm* timeInfo;
  timeInfo = localtime(&now);
  char buff[16];
  sprintf_P(buff, PSTR("%02d:%02d:%02d"), timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);

  // Repeat the post data to the buffer
  Serial.println("   (" + String(buff) + ") " + strMessage + 
    " Sensor1 = " + String(s1Reading) + 
    " Sensor2 = " + String(s2Reading));

  // Use WiFiClientSecure class to create TLS connection
  WiFiClientSecure httpClient;
  //Serial.print("connecting to ");
  //Serial.println(IFTTT_Host);
  //Serial.printf("Using fingerprint '%s'\n", fingerprint);

  // Connect to IFTTT
  if (!httpClient.connect(IFTTT_Host, httpsPort)) {
    Serial.println("   Connection failed.");
    return;
  }

  // Verify the fingerprint matches the host we're connected to
  if (!httpClient.verify(fingerprint, IFTTT_Host)) {
    Serial.println("   Certificate doesn't match.");
  } 

  Serial.print("   Requesting URL: ");
  Serial.println(IFTTT_Host + IFTTT_URI);

  // Create the post data json
  String postData = "{"
    "\"value1\":\"(" + String(buff) + ") " + strMessage + "\\n\","
    "\"value2\":\"Sensor1 = " + String(s1Reading) + "\\n\","
    "\"value3\":\"Sensor2 = " + String(s2Reading) + "\""
    "}";
  
  // Send the data to the remote endpoint
  httpClient.print("POST " + IFTTT_URI + API_KEY + " HTTP/1.1\r\n" +
    "Host: " + IFTTT_Host + "\r\n" +
    "Content-length: " + postData.length() + "\r\n" +
    "Content-Type: application/json\r\n" +
    "Connection: close\r\n\r\n" +
    postData
  );
  
  Serial.println("   Request sent.");

  ///*
  // Report what happened
  while (httpClient.connected()) {
    String line = httpClient.readStringUntil('\n');
    if (line == "\r") {
      Serial.println("   Headers received.");
      break;
    }
  }
  /*
  String line = httpClient.readStringUntil('\n');
  if (line.startsWith("{\"state\":\"success\"")) {
    Serial.println("   Success!");
  } else {
    Serial.println("   Failed.");
  }
  Serial.println("   Reply was: " + line);
  */

   //
   httpClient.stop();
}

void drawInfoGrid() {
  display.clear();

  // H start, V start, H end, V end
  display.drawLine(display.getWidth()/2, 0, display.getWidth()/2, 36);
  display.drawHorizontalLine(0, 36, display.getWidth());

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  // Sets the current font. Available default fonts
  // ArialMT_Plain_10, ArialMT_Plain_16, ArialMT_Plain_24
  display.setFont(ArialMT_Plain_10);
  // Draws a String with a maximum width at the given location.
  // If the given String is wider than the specified width
  // The text will be wrapped to the next line at a space or dash
  display.drawString(0, 0, "Sensor 1:");
  display.drawString((display.getWidth()/2)+4, 0, "Sensor 2:");
  display.drawString(0, display.getHeight()-26, "Time: ");
  display.drawString(0, display.getHeight()-14, "IP:   ");
  
  // Change to right allignment
  display.setTextAlignment(TEXT_ALIGN_RIGHT);

  // Add the IP to the right side of the display
  display.drawString(display.getWidth(), display.getHeight()-14, WiFi.localIP().toString());

  // Add the time to the right side of the display
  now = time(nullptr);
  struct tm* timeInfo;
  timeInfo = localtime(&now);
  char buff[16];
  sprintf_P(buff, PSTR("%02d:%02d:%02d"), timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);
  display.drawString(display.getWidth(), display.getHeight()-26, String(buff));
  
  // Draw in the temp readings
  float tempS1 = getTemp(sensor1);
  float tempS1Rounded = round(tempS1 * 10)/10.0;

  float tempS2 = getTemp(sensor2);
  float tempS2Rounded = round(tempS2 * 10)/10.0;

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_24);
  // Display Sensor1
  display.drawStringMaxWidth(0, 10, (display.getWidth()/2)-2, String(tempS1Rounded, 1) + "°");
  // Display Sensor2
  display.drawStringMaxWidth((display.getWidth()/2)+4, 10, display.getWidth(), String(tempS2Rounded, 1) + "°");

  // Send the completed data to the screen
  display.display();
}

void sendPage(WiFiClient client) {
    Serial.println("New Client.");          // print a message out in the serial port
    String currentLine = "";                // make a String to hold incoming data from the client

    while (client.connected()) {            // loop while the client's connected
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        Serial.write(c);                    // print it out the serial monitor
        header += c;
        if (c == '\n') {                    // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          if (currentLine.length() == 0) {
            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();

            // HTML starts here
            //
            // Declare doctype and style info
            // We're forcing an HTML meta-refresh here until the, lazy, dev 
            // can work out funneling sensor data directly through to the client.
            client.println("<!DOCTYPE html><html>");
            client.println("<head>");
            client.println("  <meta http-equiv=\"refresh\" content=\"15\">");
            client.println("  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
            client.println("  <link rel=\"icon\" href=\"data:,\">");
            client.println("  <style>");
            client.println("    html {font-family: Helvetica;}");
            client.println("    table {border-collapse:collapse;width: 50%}");
            client.println("    table, td, th {border:1px solid gray;padding:5px;text-align:center;}");
            client.println("    th {background-color: #666361; color: white;}");
            client.println("  </style>");
            
            // Declare JavaScript functions
            client.println("  <script>");
            client.println("    function localTime() {");
            client.println("      var now     = new Date();");
            client.println("      var day     = now.getDay();");
            client.println("      var date    = now.getDate();");
            client.println("      var month   = now.getMonth();");
            client.println("      var year    = now.getYear();");
            client.println("      var hours   = now.getHours();");
            client.println("      var minutes = now.getMinutes();");
            client.println("      var seconds = now.getSeconds();");
            client.println("      day         = dateIntToString(day);");
            client.println("      month       = monthIntToString(month);");
            client.println("      hours       = correctHourMinSec(hours);");
            client.println("      minutes     = correctHourMinSec(minutes);");
            client.println("      seconds     = correctHourMinSec(seconds);");
            client.println("      document.getElementById('LocalTime').innerHTML = day + \" \" + month + \" \" + date + \", \" + (year + 1900) + \" - \" + hours + \":\" + minutes + \":\" + seconds;");
            client.println("      var t = setTimeout(localTime, 500);");
            client.println("    }");
            client.println("    function dateIntToString(int){");
            client.println("      var dayString = \"\"");
            client.println("      switch (int) {");
            client.println("        case 0:dayString = \"Sun\";break;");
            client.println("        case 1:dayString = \"Mon\";break;");
            client.println("        case 2:dayString = \"Tue\";break;");
            client.println("        case 3:dayString = \"Wed\";break;");
            client.println("        case 4:dayString = \"Thu\";break;");
            client.println("        case 5:dayString = \"Fri\";break;");
            client.println("        case 6:dayString = \"Sat\";break;");
            client.println("      }");
            client.println("      return dayString;");
            client.println("    }");
            client.println("    function monthIntToString(int){");
            client.println("      var monthString = \"\"");
            client.println("      switch (int) {");
            client.println("        case 0: monthString = \"Jan\";break;");
            client.println("        case 1: monthString = \"Feb\";break;");
            client.println("        case 2: monthString = \"Mar\";break;");
            client.println("        case 3: monthString = \"Apr\";break;");
            client.println("        case 4: monthString = \"May\";break;");
            client.println("        case 5: monthString = \"Jun\";break;");
            client.println("        case 6: monthString = \"Jul\";break;");
            client.println("        case 7: monthString = \"Aug\";break;");
            client.println("        case 8: monthString = \"Sep\";break;");
            client.println("        case 9: monthString = \"Oct\";break;");
            client.println("        case 10:monthString = \"Nov\";break;");
            client.println("        case 11:monthString = \"Dec\";break;");
            client.println("      }");
            client.println("      return monthString;");
            client.println("    }");
            client.println("    function correctHourMinSec(int) {");
            client.println("      if (int < 10) {int = \"0\" + int};  // add zero in front of numbers < 10");
            client.println("      return int;");
            client.println("    }");
            client.println("  </script>");
            client.println("</head>");

            // Get and format the current time to plug into the page HTML
            now = time(nullptr);
            struct tm* timeInfo;
            timeInfo = localtime(&now);
            char buff[32];
            sprintf_P(buff, PSTR("%s %s %02d, %d - %02d:%02d:%02d"), 
              WDAY_NAMES[timeInfo->tm_wday].c_str(), 
              MONTH_NAMES[timeInfo->tm_mon].c_str(), 
              timeInfo->tm_mday, 
              timeInfo->tm_year+1900, 
              timeInfo->tm_hour, 
              timeInfo->tm_min, 
              timeInfo->tm_sec);

            // Get the sensor readings
            float s1Reading = getTemp(sensor1);
            float s2Reading = getTemp(sensor2);

            // Open BODY and place content
            client.println("<body>");
            client.println("<br>");
            client.println("<table align=\"center\" style=\"width: 100%; max-width: 500px;\">");
            client.println("  <tr>");
            client.println("    <th>Remote Timestamp</th>");
            client.println("  </tr>");
            client.println("  <tr>");
            client.println("    <td>");
            client.println(       String(buff));
            client.println("    </td>");
            client.println("  </tr>");
            client.println("</table>");
            client.println("<br>");
            client.println("<table align=\"center\" style=\"width: 100%; max-width: 500px;\">");
            client.println("  <tr>");
            client.println("    <th>Sensor 1 Temp</th>");
            client.println("    <th>Sensor 2 Temp</th>");
            client.println("  </tr>");
            client.println("  <tr>");
            // If the sensor is out of spec turn the temp text bold red
            if (s1Reading < tempThreshold){
              client.println("    <td><font size=\"5\" color=\"red\"><b>" + String(s1Reading) + "&deg!</b></font></td>");
            }
            else {
              client.println("    <td><font size=\"5\">" + String(s1Reading) + "&deg</font></td>");
            }
            if (s2Reading < tempThreshold){
              client.println("    <td><font size=\"5\" color=\"red\"><b>" + String(s2Reading) + "&deg!</b></font></td>");
            }
            else {
              client.println("    <td><font size=\"5\">" + String(s2Reading) + "&deg</font></td>");
            }
            client.println("  </tr>");
            client.println("</table>");
            client.println("<script>localTime();</script>");
            client.println("</body>");
            client.println("</html>");

            // The HTTP response ends with another blank line
            client.println();
            // Break out of the while loop
            break;
          } 
          else { // if you got a newline, then clear currentLine
            currentLine = "";
          }
        } 
        else if (c != '\r') {  // if you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }
      }
    }
    // Clear the header variable
    header = "";
    // Close the connection
    client.stop();
    Serial.println("Client disconnected.");
    Serial.println("");
}