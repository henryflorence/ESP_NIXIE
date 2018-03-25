/* 
 *    ESP Nixie Clock 
 *	  Copyright (C) 2016  Larry McGovern
 *	
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License <http://www.gnu.org/licenses/> for more details.
 */
	
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <Timezone.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Switch.h>
#include <ESP8266WebServer.h>

WiFiClient client;

const char* timezonedbAPIkey = "57J1MAM4QACH";
char timezonedbLocation[50] = "Europe/London";
long dstChange1 = -1;
long dstChange2 = -1;
char dstState = -1;

#include <timezonedb.h>

#define D0 16 // LED_BUILTIN
#define D1 5 // I2C Bus SCL (clock)
#define D2 4 // I2C Bus SDA (data)
#define D3 0 // 
#define D4 2 //  Blue LED 
#define D5 14 // SPI Bus SCK (clock)
#define D6 12 // SPI Bus MISO 
#define D7 13 // SPI Bus MOSI
#define D8 15 // SPI Bus SS (CS) 
#define D9 3 // RX0 (Serial console) 
#define D10 1 // TX0 (Serial console) 

const int dataPin  = D5; // SER (pin 14)
const int latchPin = D6; // RCLK (pin 12)
const int clockPin = D7; // SRCLK (pin 11)

const int encoderPinA = D9;
const int encoderPinB = D10;
const int encoderButtonPin = D0;

//#ifdef DEBUG_ESP_PORT
//#define DEBUG_MSG(...) DEBUG_ESP_PORT.printf( __VA_ARGS__ )
//#else
//#define DEBUG_MSG(...)
//#endif

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.nist.gov", 0, 7200000); // Update time every two hours

// Set timezone rules.  Offsets set to zero, since they will be loaded from EEPROM
TimeChangeRule myDST = {"DST", Second, Sun, Mar, 2, 0};    
TimeChangeRule mySTD = {"STD", First, Sun, Nov, 2, 0};     
Timezone myTZ(myDST, mySTD);

#define OLED_RESET  LED_BUILTIN
Adafruit_SSD1306 display(OLED_RESET);

int encoderPos, encoderPosPrev;

enum Menu {
  TOP,
  SETTINGS,
  RESET_WIFI,
  SET_UTC_OFFSET,
  ENABLE_DST
} menu;

// EEPROM addresses
const int EEPROM_addr_UTC_offset = 0; 
const int EEPROM_addr_DST = 1;  
const int EEPROM_addr_24hr = 2;
const int EEPROM_addr_timezone = 3;

bool enableDST;  // Flag to enable DST
bool twenty4hour;

std::unique_ptr<ESP8266WebServer> server;

void updateSettings() {
  if(!server->hasArg("offset")) {
    server->send(500, "text/plain", "malformed GET string");
    return;
  }

  enableDST = server->hasArg("dst");
  int UTC_STD_Offset = ((server->arg("offset").toInt() + 12) % 24) - 12;
  mySTD.offset = UTC_STD_Offset * 60;
  myDST.offset = mySTD.offset;
  if (enableDST) {
    myDST.offset += 60;
  }
  
  twenty4hour = server->hasArg("twenty4hour"); 
  
  String tzText = server->arg("timezone");
  tzText.replace("%2F", "/");
  tzText.toCharArray(timezonedbLocation, 50);
  
  myTZ = Timezone(myDST, mySTD);

  EEPROM.write(EEPROM_addr_UTC_offset, (unsigned char)(mod(mySTD.offset/60,24))); 
  EEPROM.commit();

  EEPROM.write(EEPROM_addr_DST, (unsigned char)enableDST);
  EEPROM.commit();  

  EEPROM.write(EEPROM_addr_24hr, (unsigned char)twenty4hour);
  EEPROM.commit();  

  for(int i=0; i < 50; i++)
    EEPROM.write(EEPROM_addr_timezone + i, timezonedbLocation[i]);
  EEPROM.commit();

  dstState = -1;
  
  String msg;
//  msg += "updating settings:\n";
//  msg += "utc offset = ";
//  msg += server->arg("offset");
//  msg += "\ndst enabled = ";
//  msg += enableDST ? "true" : "false";
//  msg += "\n24 hour clock = ";
//  msg += twenty4hour ? "true" : "false";
//  server->send(200, "text/plain", msg);
  server->send(200, "text/html", "<meta http-equiv=\"refresh\" content=\"0; url=/\" />");
}

void homePage() {
  String webPage;
  String dst = enableDST ? " checked" :  "";
  String twenty4 = twenty4hour ? " checked" : "";
  String timezone = String(timezonedbLocation);
  int utc_offset = mySTD.offset / 60;
    
  char tbuffer[300];
  sprintf(tbuffer, "<h1>ESP8266 Nixie Clock Settings</h1><form action=\"update\"><p><label><input name=\"offset\" type=\"number\" value=\"%i\"\" style=\"width: 40px;\">&nbsp;UTC Offset</label></p>", utc_offset);
  webPage += tbuffer;
  webPage += "<p><label><input name=\"dst\" type=\"checkBox\"" + dst + ">&nbsp;Enable DST</label></p>";
  webPage += "<p><label><input name=\"timezone\" type=\"text\" value=\"" + timezone + "\">&nbsp;Use timezonedb.com - leave blank to ignore";
  webPage += "<p><label><input name=\"twenty4hour\" type=\"checkBox\"" + twenty4 + ">&nbsp;24 hour clock</label></p>";
  webPage += "<p><input type=\"submit\" value=\"Update\"></p>"; //&nbsp;<a href=\"resetWifi\"><button>Reset Wifi</button></a></p>";
  server->send(200, "text/html", webPage);
}

void setup() {
  Serial.begin(115200);
  
  pinMode(dataPin, OUTPUT);
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT); 
  pinMode(encoderPinA, INPUT_PULLUP);
  pinMode(encoderPinB, INPUT_PULLUP);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // OLED I2C Address, may need to change for different device,
                                              // Check with I2C_Scanner

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.clearDisplay();
  
  display.setCursor(0,0);
  display.println("Connecting...");
  display.println();
  display.println("Cycle Power after a");
  display.println("few minutes if no");
  display.print("connection.");
  display.display();

    // Setup WiFiManager
  WiFiManager MyWifiManager;
  MyWifiManager.setAPCallback(configModeCallback);
  MyWifiManager.autoConnect("NIXIE_CLOCK");
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("Wifi Connected");
  display.display();

  timeClient.begin();

  display.setCursor(0,28);
  display.println("Updating local time");
  display.display();

  EEPROM.begin(53);
  // Read Daylight Savings Time setting from EEPROM
  enableDST = EEPROM.read(EEPROM_addr_DST) != 0;
  
  // Read UTC offset from EEPROM
  int utc_offset = (((int)EEPROM.read(EEPROM_addr_UTC_offset)+12) % 24) - 12;
  mySTD.offset = utc_offset * 60;
  myDST.offset = mySTD.offset;
  if (enableDST) {
    myDST.offset += 60;
  }
  myTZ = Timezone(myDST, mySTD);

  twenty4hour = EEPROM.read(EEPROM_addr_24hr) != 0;

  for(int i=0; i < 50; i++)
    timezonedbLocation[i] = EEPROM.read(EEPROM_addr_timezone + i);

  menu = TOP;
  updateSelection();

  //utc_offset = 1;

  server.reset(new ESP8266WebServer(WiFi.localIP(), 80));
  
  server->on("/", homePage);
  server->on("/update", updateSettings);
  server->begin();

  Serial.println("end of setup");
}

void updateTzUsingTimezone(long gmtEpoch) {
  long gmtOffset;
  boolean dst = 0;
  getTimezoneDst(&dst, &dstChange1, &dstChange2, &gmtOffset);
  if(dst == 0) dstState = -2;
  else {
    if(gmtEpoch < dstChange1) dstState = 0;
    else if(gmtEpoch < dstChange2) dstState = 1;
    else if(gmtEpoch > dstChange2) dstState = 2;
  }
  display.println("sausage");
  display.display();
  mySTD.offset = gmtOffset / 60;
  myDST.offset = mySTD.offset;
  myTZ = Timezone(myDST, mySTD);
}

long checkTimezonedb(long gmtEpoch) {
  // fetch tz offset if uninitialised
  if(dstState == -1) {
    updateTzUsingTimezone(gmtEpoch);
  // if the timezone has dst and the time has changed across the dstchange dates update tz
  } else if( dstState > -1) {
    if((gmtEpoch < dstChange1 && dstState != 0) ||
        (gmtEpoch < dstChange2 && dstState != 1) ||
        (gmtEpoch > dstChange2 && dstState != 2)) {
        updateTzUsingTimezone(gmtEpoch);     
      }
  }
}

time_t prevTime = 0;
bool initial_loop = 1;

void loop() {
  timeClient.update();
  if(strcmp(timezonedbLocation, "") != 0) 
    //checkTimezonedb(timeClient.getEpochTime());
  setTime(myTZ.toLocal(timeClient.getEpochTime()));

  if (now() != prevTime) {
    prevTime = now();
    displayTime();
    Serial.printf("update time");
  }
  //Serial.printf("loop");
  server->handleClient();
}

void displayTime(){
   char tod[10], time_str[20], date_str[20];
   const char* am_pm[] = {" AM", " PM"};
   const char* month_names[] = {"Jan", "Feb", "March", "April", "May", "June", "July", "Aug", "Sept", "Oct", "Nov", "Dec"};
   unsigned char hourBcd = decToBcd((unsigned char)(twenty4hour ? hour() : hourFormat12()));

   if ((hourBcd >> 4) == 0) { // If 10's digit is zero, we don't want to display a zero
    hourBcd |= (15 << 4); 
   }
   
   // Write to shift register
   digitalWrite(latchPin, LOW);
   shiftOut(dataPin, clockPin, MSBFIRST, hourBcd);
   shiftOut(dataPin, clockPin, MSBFIRST, decToBcd((unsigned char)minute()));
   digitalWrite(latchPin, HIGH);
   
   if ((menu == TOP) || (menu == SET_UTC_OFFSET)) {
      int hours = twenty4hour ? hour() : hourFormat12();
      formattedTime(tod, hours, minute(), second());
      sprintf(time_str, "%s%s", tod, twenty4hour ? "" : am_pm[isPM()] );
      sprintf(date_str, "%s %d, %d", month_names[month() - 1], day(), year());
      display.fillRect(20,28,120,8,BLACK);
      display.setCursor(20,28);
      display.print(time_str);
      if (enableDST) {
        if (myTZ.utcIsDST(timeClient.getEpochTime())) {
          display.print(" DST");
        }
        else {
          display.print(" STD");
        }
      }
      display.setCursor(20,36);
      display.print(date_str);
      display.display();
   }
}

unsigned char decToBcd(unsigned char val)
{
  return ( ((val/10)*16) + (val%10) );
}

#define colonDigit(digit) digit < 10 ? ":0" : ":"
void formattedTime(char *tod, int hours, int minutes, int seconds)
{
  sprintf(tod, "%d%s%d%s%d", hours, colonDigit(minutes), minutes, colonDigit(seconds), seconds);  // Hours, minutes, seconds
}

void updateSelection() { // Called whenever encoder is turned
  display.clearDisplay();
  
  display.setTextColor(WHITE,BLACK);
  display.setCursor(0,0);
  display.print("Wifi Connected");
  display.setCursor(20,46);
  display.print(WiFi.localIP());
  
  display.display(); 
}

void resetWiFi(){
  WiFiManager MyWifiManager;
  MyWifiManager.resetSettings();
  ESP.restart();
}

int mod(int a, int b)
{
    int r = a % b;
    return r < 0 ? r + b : r;
}

void configModeCallback (WiFiManager *myWiFiManager) {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("To configure Wifi,  ");
  display.println("connect to Wifi ");
  display.println("network ESPCLOCK and");
  display.println("open 192.168.4.1");
  display.println("in web browser");
  display.display(); 
}



