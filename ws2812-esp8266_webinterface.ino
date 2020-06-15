/*  WS2812FX Webinterface.
  
  Harm Aldick - 2016
  www.aldick.org

  
  FEATURES
  * Webinterface with mode, color, speed and brightness selectors


  LICENSE

  The MIT License (MIT)

  Copyright (c) 2016  Harm Aldick 

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.

  
  CHANGELOG
  2016-11-26 initial version
  2018-01-06 added custom effects list option and auto-cycle feature
  
*/

#define NO_SETUP

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
#include <Adafruit_NeoPixel.h>
#include "FastLED.h" // be sure to install and include the FastLED lib

#include <WS2812FX.h>
#include "template.h"
String CODE_VERSION = "$Revision$";

#include <Syslog.h>

extern Syslog syslog;

extern const char index_html[];
extern const char main_js[];

ESP8266WebServer server(80);
//FIXME: the light strip doesn't like analogwrite on internal led pin.
const int LEDSTRIP1_PIN = D5;
const int LEDSTRIP1_COUNT = 300;
const int LEDSTRIP2_PIN = D4;   // be sure to set the pin appropriately before rebooting
const int LEDSTRIP2_COUNT = 100;

// QUICKFIX...See https://github.com/esp8266/Arduino/issues/263
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

#define DEFAULT_COLOR 0xFF5900
#define DEFAULT_BRIGHTNESS 128
#define DEFAULT_SPEED 1000
#define DEFAULT_MODE FX_MODE_STATIC

unsigned long auto_last_change = 0;
String modes = "";
uint8_t myModes[] = {}; // *** optionally create a custom list of effect/mode numbers
boolean auto_cycle = false;

boolean state_power = true;
uint8_t state_brightness = DEFAULT_BRIGHTNESS;

WS2812FX ws2812fx = WS2812FX(LEDSTRIP1_COUNT, LEDSTRIP1_PIN, NEO_GRB + NEO_KHZ800);
// FIXME: second strip will need to be configured with:
//WS2812FX ws2812fx = WS2812FX(LEDSTRIP2_COUNT, LEDSTRIP2_PIN, NEO_BRG + NEO_KHZ400)

String contents_row(String title, String params) {
    String message="";

    message += "<tr>";
    message += "<td><a href=\"" + params + "&strip=-1\">both</a></td>";
    message += "<td><a href=\"" + params + "&strip=1\">left</a></td>";
    message += "<td><a href=\"" + params + "&strip=2\">right</a></td>";

    message += "<td>" + title + "</td>";

    message += "</tr>";

    return message;
}

String handleRoot_stub() {
    server.send_P(200,"text/html", index_html);
    return "NA";
}

String http_uptime_stub() {
    return "";
}

/*
 * Build <li> string for all modes.
 */
void modes_setup() {
    modes = "";
    uint8_t num_modes = sizeof(myModes) > 0 ? sizeof(myModes) : ws2812fx.getModeCount();
    for(uint8_t i=0; i < num_modes; i++) {
        uint8_t m = sizeof(myModes) > 0 ? myModes[i] : i;
        modes += "<li><a href='#'>";
        modes += ws2812fx.getModeName(m);
        modes += "</a></li>";
    }
}

// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 50, suggested range 20-100 
//#define COOLING  66

// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
//#define SPARKING 100

// following are just for ws2812fx_custom_FastLed
#define LED_PIN LEDSTRIP1_PIN
#define NUM_LEDS LEDSTRIP1_COUNT

void Fire2012();
#include "ws2812fx_custom_FastLED.h"
#include "RainbowFireworks.h"
#include "DualLarson.h"

/* #####################################################
   #  Webserver Functions
   ##################################################### */

void srv_handle_not_found() {
    server.send(404, "text/plain", "File Not Found");
}

void srv_handle_index_html() {
//    server.send_P(200,"text/html", index_html);
//    server.send(200, "text/plain", "Yeah nah yeah");
}

void srv_handle_main_js() {
    server.send_P(200,"application/javascript", main_js);
}

void srv_handle_modes() {
    server.send(200,"text/plain", modes);
}

void srv_handle_get() {
    String content = "Current state:\n";
    content = content + "power: " + String(state_power) + "\n";
    content = content + "cycle: " + String(auto_cycle) + "\n";
    content = content + "brightness: " + String(( state_power ? ws2812fx.getBrightness() : state_brightness )) + "\n";
    content = content + "colour: " + String(ws2812fx.getColor()) + "\n";
    content = content + "mode: " + String(ws2812fx.getMode())+ "\n";
    
    server.send(200,"text/plain", content);
}

void srv_handle_set() {
    if (!state_power) {
        ws2812fx.setBrightness(state_brightness);
        state_power = true;
    }
    
    for (uint8_t i=0; i < server.args(); i++){
        if(server.argName(i) == "c") {
            uint32_t tmp = (uint32_t) strtol(server.arg(i).c_str(), NULL, 16);
            if(tmp >= 0x000000 && tmp <= 0xFFFFFF) {
                ws2812fx.setColor(tmp);
            }
            syslog.logf(LOG_INFO, "c: \"%s\" -> %d", server.arg(i).c_str(), ws2812fx.getColor());
        }

        if(server.argName(i) == "m") {
            uint8_t tmp = (uint8_t) strtol(server.arg(i).c_str(), NULL, 10);
            syslog.logf(LOG_INFO, "m: %s", server.arg(i).c_str());
            ws2812fx.setMode(tmp % ws2812fx.getModeCount());
            Serial.print("mode is "); Serial.println(ws2812fx.getModeName(ws2812fx.getMode()));
        }

        if(server.argName(i) == "b") {
            if(server.arg(i)[0] == '-') {
                ws2812fx.setBrightness(ws2812fx.getBrightness() * 0.8);
            } else if(server.arg(i)[0] == ' ') {
                ws2812fx.setBrightness(min(max(ws2812fx.getBrightness(), 5) * 1.2, 255));
            } else { // set brightness directly
                uint8_t tmp = (uint8_t) strtol(server.arg(i).c_str(), NULL, 10);
                ws2812fx.setBrightness(tmp);
            }
            syslog.logf(LOG_INFO, "b: \"%s\" -> %d", server.arg(i).c_str(), ws2812fx.getBrightness());
            Serial.print("brightness is "); Serial.println(ws2812fx.getBrightness());
        }

        if(server.argName(i) == "s") {
            if(server.arg(i)[0] == '-') {
                ws2812fx.setSpeed(max(ws2812fx.getSpeed(), 5) * 1.2);
            } else {
                ws2812fx.setSpeed(ws2812fx.getSpeed() * 0.8);
            }
            syslog.logf(LOG_INFO, "s: \"%s\" -> %d", server.arg(i).c_str(), ws2812fx.getSpeed());
            Serial.print("speed is "); Serial.println(ws2812fx.getSpeed());
        }

        if(server.argName(i) == "a") {
            if(server.arg(i)[0] == '-') {
                auto_cycle = false;
            } else {
                auto_cycle = true;
                auto_last_change = 0;
            }
            syslog.logf(LOG_INFO, "a: \"%s\" -> %d", server.arg(i).c_str(), auto_cycle);
        }
    }
    server.send(200, "text/plain", "OK");
}

uint16_t ledsOff(void) {
  WS2812FX::Segment* seg = ws2812fx.getSegment(); // get the current segment
  uint16_t seglen = seg->stop - seg->start + 1;

  if (state_power) {
      state_brightness = ws2812fx.getBrightness();
      state_power = false;
      auto_cycle = false;

      ws2812fx.setBrightness(0);
      ws2812fx.fade_out();
  }

  return(seg->speed / seglen);
}

void setup_stub(void) {
    digitalWrite(D8, LOW);  // should be unused, reset pin, assign to known state
    digitalWrite(D4, HIGH); // should be unused, reset pin, assign to known state
//    digitalWrite(D3, HIGH); // should be unused, reset pin, assign to known state - overriden by input_pullup below
//    pinMode(D3, OUTPUT);  // switch installed on this pin - maybe configure as input PULLUP instead?

    pinMode(D3, INPUT_PULLUP);
    pinMode(D4, OUTPUT);
    pinMode(D8, OUTPUT);

    ledRamp(0,led_range,1000,30);

//    pinMode(relayPin, OUTPUT);

//    Serial.begin(115200);
//    Serial.println("");
//    Serial.println("Booting");

//    WiFi.setAutoConnect(true);  // Autoconnect to last known Wifi on startup
//    WiFi.setAutoReconnect(true);
//    WiFi.onEvent(eventWiFi);      // Handle WiFi event

//    WiFi.mode(WIFI_STA);
//    WiFi.begin(ssid, password);

//    Serial.println("");

//    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
//        Serial.println("Connection Failed! Rebooting...");
//        delay(5000);
//        reboot();
//    }

    //first parameter is name of access point, second is the password
//    wifiManager.autoConnect("ledstrip");

//    WiFiManager wifiManager;

    Serial.println("");

    //find it as http://lights.local
    /*if (MDNS.begin("lights")) 
      {
      Serial.println("MDNS responder started");
      }*/
  
    ledRamp(led_range,0,1000,30);

    //FIXME: default to the last mode used - store this in EEPROM
         // https://www.best-microcontroller-projects.com/arduino-eeprom.html
         // Write to it max 10 times a day for 27 years lifetime.  Use
         // .update to only write if unchanged.  
    
//    server.on("/", handleRoot);

    ws2812fx.setCustomMode(0, F("Fire2012"), myCustomEffect);
    ws2812fx.setCustomMode(1, F("Dual Larson"), dualLarson);
    ws2812fx.setCustomMode(2, F("Rainbow Fireworks"), rainbowFireworks);
    ws2812fx.setCustomMode(3, F("Off"), ledsOff);

    modes.reserve(5000);
    modes_setup();

    Serial.println("WS2812FX setup");
    ws2812fx.init();
    ws2812fx.setMode(DEFAULT_MODE);
    ws2812fx.setColor(DEFAULT_COLOR);
    ws2812fx.setSpeed(DEFAULT_SPEED);
    ws2812fx.setBrightness(DEFAULT_BRIGHTNESS);

    ws2812fx.start();

//    Serial.println("Wifi setup");
//    wifi_setup();
 
    Serial.println("HTTP server extra setup");
//    server.on("/", srv_handle_index_html); //handled by handleRoot_stub
    server.on("/main.js", srv_handle_main_js);
    server.on("/modes", srv_handle_modes);
    server.on("/set", srv_handle_set);
    server.on("/get", srv_handle_get);
//    server.onNotFound(srv_handle_not_found);

    EEPROM.begin(512);

    int i = EEPROM.read(12);
    Serial.println();
    Serial.print("eeprom12: ");
    Serial.println(i);

    int j = EEPROM.read(12);  //FIXME: or put and get

    Serial.print("eeprom13: ");
    Serial.println(j);

//    EEPROM.write(12, i+1);  // comment out next 3 lines after first upload, re upload sketch, and check values.
//    EEPROM.write(13, j+1);
//    EEPROM.commit();

    ledRamp(0,led_range,80,30);

    // Port defaults to 8266
    // ArduinoOTA.setPort(8266);

//    strip1.begin();
//    strip1.show(); // Initialize all pixels to 'off'
//    strip2.begin();
//    strip2.show(); // Initialize all pixels to 'off'

//    ledBright(1);  // WARNING: don't leave led at analogue value for length of time, since led strip doesn't like it even when it's operating on another pin.  Timing?
    digitalWrite(ONBOARD_LED_PIN, 1);
}


void loop_stub(void) {
    unsigned long now = millis();

    ws2812fx.service();

    if(auto_cycle && (now - auto_last_change > 10000)) { // cycle effect mode every 10 seconds
        uint8_t next_mode = (ws2812fx.getMode() + 1) % ws2812fx.getModeCount();
        if(sizeof(myModes) > 0) { // if custom list of modes exists
            for(uint8_t i=0; i < sizeof(myModes); i++) {
                if(myModes[i] == ws2812fx.getMode()) {
                    next_mode = ((i + 1) < sizeof(myModes)) ? myModes[i + 1] : myModes[0];
                    break;
                }
            }
        }
        ws2812fx.setMode(next_mode);
        Serial.print("mode is "); Serial.println(ws2812fx.getModeName(ws2812fx.getMode()));
        auto_last_change = now;
    }
}



