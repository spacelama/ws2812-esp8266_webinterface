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
#include <ESP_EEPROM.h>
#include <WiFiUdp.h>
#include <Adafruit_NeoPixel.h>
#include "FastLED.h" // be sure to install and include the FastLED lib

#include "Switch.h"

#include <WS2812FX.h>
#include "template.h"
String CODE_VERSION = "$Revision: 1.21 $";

#include <Syslog.h>

extern Syslog syslog;

extern const char index_html[];
extern const char main_js[];

ESP8266WebServer server(80);
//FIXME: the light strip doesn't like analogwrite on internal led pin.
#define LED_PIN_V1     D2    // virtual digital pin (dummy - used during slow initialisation, pin must not be connected to anything)
//FIXME: It is preferred not to use D4 (or D3,D8)
#define LED_PIN_P1     D4    // physical digital pin used to drive the first physical LED strip
#define LED_COUNT_P1   100   // number of LEDs on the first physical strip
#define LED_PIN_P2     D5    // physical digital pin used to drive the second physical LED strip
#define LED_COUNT_P2   300   // number of LEDs on the second physical strip
#define LED_COUNT_V1   (LED_COUNT_P1 + LED_COUNT_P2)
#define mainButtonPin  D6
#define mainButtonPair D7

// QUICKFIX...See https://github.com/esp8266/Arduino/issues/263
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min3(a,b,c) ((a)<min((b),(c))?(a):min((b),(c)))
#define max3(a,b,c) ((a)>max((b),(c))?(a):max((b),(c)))

#define DEFAULT_COLOR 0xFFAA88
#define DEFAULT_BRIGHTNESS 128 // 16
#define DEFAULT_SPEED 1000
#define DEFAULT_MODE FX_MODE_STATIC

uint8_t current_mode;
unsigned long auto_last_change = 0;
String modes = "";
//const int increment_from=35;
uint8_t myModes[] =  // [increment_from+62] =
    // WARNING: 1st and 3rd positions must remain fixed because of external callers (flashLightsNotification,turnNightLightsOn), plus DEFAULT_MODE is MODE_STATIC which wants to be 0 because of the current way the myModes works
{0,59,2,60,61,58,57,24,25,23,22,21,20,19,56,55,2,15,12,33,11,13,14,17,18,43,44,45,46,47,48,49,62}; // *** optionally create a custom list of effect/mode numbers
//uint8_t myModes[] = {}; // *** optionally create a custom list of effect/mode numbers
boolean auto_cycle = false;

boolean state_power = true;
uint8_t state_brightness = DEFAULT_BRIGHTNESS;
unsigned long last_eeprom_write_time = 0;
unsigned long reboot_at = 0;
boolean eeprom_write_triggered = false;

// create an instance of one virtual strip and two physical strips.
// the physical strips are only initialized with a count of one LED (except for those that we end up reversing on the fly - we store the reversed pixels in the space allocated here, since
// these strips will ultimately use the pixel data of the virtual strip.
// (Note the instances are created with support of only one segment and one
// segment_runtime, just so the sketch fits in an Arduino's limited SRAM.)
// Pixel data(GRB) takes effect at setpixel time, so is only relevant for virtual strip
// Speed data takes effect at readout time, so is only relevant for physical strips
uint8_t *pixels_p1_reverse;
WS2812FX ws2812fx = WS2812FX(LED_COUNT_P1 + LED_COUNT_P2, LED_PIN_V1, NEO_GRB + NEO_KHZ800, 1, 1);
WS2812FX ws2812fx_p1 = WS2812FX(LED_COUNT_P1, LED_PIN_V1, NEO_BRG + NEO_KHZ400);
//WS2812FX ws2812fx_p1_swapped = WS2812FX(1, LED_PIN_P1, NEO_BRG + NEO_KHZ400);
WS2812FX ws2812fx_p2 = WS2812FX(1, LED_PIN_V1, NEO_GRB + NEO_KHZ800);

bool long_pressed = false;
bool double_clicked = false;
bool triple_clicked = false;
unsigned long time_button_released = 0;

bool disabled_at_boot = false;

#define LONG_CLICK 500
#define DOUBLE_CLICK 450

Switch mainButton = Switch(mainButtonPin,INPUT_PULLUP,LOW,50,LONG_CLICK,DOUBLE_CLICK);  // Switch between a digital pin and GND

// https://www.cs.rit.edu/~ncs/color/t_convert.html
// r,g,b values are from 0 to 1
// h = [0,360], s = [0,1], v = [0,1]
//		if s == 0, then h = -1 (undefined)

float fade_dirn=0;
float initial_fade_multiplier=1;
int s_dirn=1;
int v_dirn=1;
float h1_saved=0;
float s1_saved=0;
float v1_saved=0;
unsigned long hsv_read_time = 0;

void RGBtoHSV( uint8_t r, uint8_t g, uint8_t b, float *hf, float *sf, float *vf )
{
    float cmin, cmax, delta;
    float rf, gf, bf;

    rf = float(r)/255;
    gf = float(g)/255;
    bf = float(b)/255;

    cmin = min3( rf, gf, bf );
    cmax = max3( rf, gf, bf );
    *vf = cmax;				// v

    delta = cmax - cmin;
//    syslog.logf(LOG_INFO, "min,max,delta = %f,%f,%f\n", min,max,delta);


    if( cmax != 0 )
        *sf = delta / cmax;		// s
    else {
        // r = g = b = 0		// s = 0, v is undefined
        *sf = 0;
        *hf = -1;
        return;
    }

    if (delta == 0)
        *hf = 0;
    else if( rf == cmax )
        *hf = ( gf - bf ) / delta;          // between yellow & magenta
    else if( gf == cmax )
        *hf = 2 + ( bf - rf ) / delta;	// between cyan & yellow
    else if( bf == cmax )
        *hf = 4 + ( rf - gf ) / delta;	// between magenta & cyan
    else {
        syslog.logf(LOG_INFO, "FIXME: compare floating points better: rf, gf, bf, cmax= %f %f %f %f", rf,gf,bf, cmax);
        *hf = 0;
    }

    *hf *= 60;				// 0-360 degrees
    if( *hf < 0 )
        *hf += 360;
    syslog.logf(LOG_INFO,"rf,gf,bf, cmin,cmax, delta, hf,sf,vf->%0.3f,%0.3f,%0.3f, %0.3f,%0.3f, %0.3f, %0.3f,%0.3f,%0.3f", rf,gf,bf, cmin,cmax, delta, *hf,*sf,*vf);
}

void HSVtoRGB( uint8_t *r, uint8_t *g, uint8_t *b, float hf, float sf, float vf )
{
    int i;
    float f, p, q, t;
    float rf, gf, bf;

    if( sf == 0 ) {
        // achromatic (grey)
        *r = *g = *b = vf*255;
        return;
    }

    hf /= 60;			// sector 0 to 5
    i = floor( hf );
    f = hf - i;			// factorial part of h
    p = vf * ( 1 - sf );
    q = vf * ( 1 - sf * f );
    t = vf * ( 1 - sf * ( 1 - f ) );

    switch( i ) {
      case 0:
          rf = vf;
          gf = t;
          bf = p;
          break;
      case 1:
          rf = q;
          gf = vf;
          bf = p;
          break;
      case 2:
          rf = p;
          gf = vf;
          bf = t;
          break;
      case 3:
          rf = p;
          gf = q;
          bf = vf;
          break;
      case 4:
          rf = t;
          gf = p;
          bf = vf;
          break;
      default:		// case 5:
          rf = vf;
          gf = p;
          bf = q;
          break;
    }
    *r = rf*255;
    *g = gf*255;
    *b = bf*255;

    syslog.logf(LOG_INFO,"hf,i,f,p,q,t, rf,gf,bf->%0.3f,%i,%0.3f,%0.3f,%0.3f,%0.3f, %0.3f,%0.3f,%0.3f, %i,%i,%i", hf,i,f,p,q,t, rf,gf,bf, *r,*g,*b);
}

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

void handleRoot() {
    server.send_P(200,"text/html", index_html);
}

String http_uptime_stub() {
    return "";
}

void read_Eeprom() {
    Serial.println("Reading eeprom");
    EEPROM.begin(18);

    // FIXME: record and reread last boot.  If rebooted too rapidly,
    // assume an early crash, and default to a safe mode (handle it
    // through reading button press at start instead)

    uint8_t mode;
    uint16_t speed;
    uint32_t colors[3];

    EEPROM.get(0, state_power); //byte
    EEPROM.get(1, auto_cycle); //byte
    EEPROM.get(2, state_brightness); //byte
    EEPROM.get(3, mode); //byte
    EEPROM.get(4, speed); //2 bytes
    EEPROM.get(6, colors[0]); //4 bytes * 3 colours
    EEPROM.get(10, colors[1]);
    EEPROM.get(14, colors[2]);
//    EEPROM.get(18, last_eeprom_write_time); // 4 bytes
//    EEPROM.get(14, last_boot_time); // 4 bytes

    Serial.printf("Read eeprom: state_power=%hhu, auto_cycle=%hhu, state_brightness=%hhu, mode=%hhu, speed=%u, colors=%lu,%lu,%lu, last_eeprom_write_time=%lu\n",
                  state_power, auto_cycle, state_brightness, mode, speed, colors[0],colors[1],colors[2], last_eeprom_write_time);
    syslog.logf(LOG_INFO, "Read eeprom: state_power=%hhu, auto_cycle=%hhu, state_brightness=%hhu, mode=%hhu, speed=%u, colors=%lu,%lu,%lu, last_eeprom_write_time=%lu\n",
                  state_power, auto_cycle, state_brightness, mode, speed, colors[0],colors[1],colors[2], last_eeprom_write_time);

    if (mode == 62) {
        Serial.println("Mode erroneously read as \"reboot\".  Resetting to 0");
        mode = 0;
    }

    //FIXME: implement a CRC or detect that we've just been flashed, and go back to default mode instead

    if (state_power) {
        ws2812fx.setBrightness(state_brightness);
    } else {
        ws2812fx.setBrightness(0);
    }
    ws2812fx.setMode(mode);
    ws2812fx.setSpeed(speed);
    ws2812fx.setColors(0, colors);
}

void commit_Eeprom() {
    // FIXME: should write eeprom with the final destination
    // results before we start fading, given the apparently
    // high chances of it crashing and rebooting during the
    // fade
    uint32_t *colors = ws2812fx.getColors(0);
    uint32_t color0=colors[0];
    boolean power=state_power;

    uint8_t r1;
    uint8_t g1;
    uint8_t b1;

    EEPROM.begin(18);

    // FIXME: record and reread last boot.  If rebooted too rapidly,
    // assume an early crash, and default to a safe mode

    if (fade_dirn != 0) {
        if (fade_dirn > 0) {
            HSVtoRGB(&r1,&g1,&b1, h1_saved, s1_saved, 1.0);
            color0 = r1*256*256 + g1*256 + b1;
        } else {
            power=false;
        }
    }
    last_eeprom_write_time = millis();
    EEPROM.put(0, power); //byte
    EEPROM.put(1, auto_cycle); //byte
    EEPROM.put(2, state_brightness); //byte
    EEPROM.put(3, ws2812fx.getMode()); //byte
    EEPROM.put(4, ws2812fx.getSpeed()); //2 bytes
    EEPROM.put(6, color0); //4 bytes * 3 colours
    EEPROM.put(10, colors[1]);
    EEPROM.put(14, colors[2]);
//    EEPROM.put(18, last_eeprom_write_time); // 4 bytes
//    EEPROM.put(14, last_boot_time); // 4 bytes

    EEPROM.commit();

    syslog.logf(LOG_INFO, "Write(commit) eeprom: state_power=%hhu, auto_cycle=%hhd, state_brightness=%hhu, mode=%hhu, speed=%u, colors=%lu,%lu,%lu, last_eeprom_write_time=%lu\n",
                state_power, auto_cycle, state_brightness, current_mode, ws2812fx.getSpeed(), colors[0],colors[1],colors[2], last_eeprom_write_time);

    eeprom_write_triggered = false;
}

void write_Eeprom() {
    unsigned long now = millis();

    // https://www.best-microcontroller-projects.com/arduino-eeprom.html
    // Write to it max 10 times a day for 27 years lifetime.  Use
    // .update to only write if unchanged.  
    if (eeprom_write_triggered && abs(now - last_eeprom_write_time) > 60000) {
        commit_Eeprom();
    }
}

void trigger_eeprom_write(void) {
    uint32_t *colors = ws2812fx.getColors(0);

    eeprom_write_triggered=true;
    syslog.logf(LOG_INFO, "Triggering eeprom for future write");
    syslog.logf(LOG_INFO, "    : state_power=%hhu, auto_cycle=%hhd, state_brightness=%hhu, mode=%hhu, speed=%u, colors=%lu,%lu,%lu, last_eeprom_write_time=%lu\n",
                state_power, auto_cycle, state_brightness, current_mode, ws2812fx.getSpeed(), colors[0],colors[1],colors[2], last_eeprom_write_time);
}

uint16_t mode_reboot(void) {
    WS2812FX::Segment* seg = ws2812fx.getSegment(); // get the current segment
    uint16_t seglen = seg->stop - seg->start + 1;

    ws2812fx.setMode(current_mode);  // mode prior to being reset
                                     // by the user calling a
                                     // function or pressing a
                                     // button
    syslog.logf(LOG_INFO, "Rebooting upon request.  Mode will reset to %hhu", ws2812fx.getMode());
    Serial.printf("Rebooting upon request.  Mode will reset to %hhu\n", ws2812fx.getMode());
    write_Eeprom();  // if triggered
    reboot_at=millis()+500;
    return(seg->speed / seglen);
}

uint16_t reset_default(void) {
    auto_cycle = false;
//    state_power = true;  // whoever called us will turn power on instead
    ws2812fx.setMode(DEFAULT_MODE);
    uint32_t colors[3] = {DEFAULT_COLOR,0,0};
    ws2812fx.setColors(0,colors);
    ws2812fx.setSpeed(DEFAULT_SPEED);
    ws2812fx.setBrightness(DEFAULT_BRIGHTNESS);
    ws2812fx.fill(ws2812fx.getColor());

    trigger_eeprom_write();

    return ws2812fx.getSpeed();
}

/*
 * Build <li> string for all modes.
 */
void modes_setup() {
    /* for (uint8_t i=increment_from; i < sizeof(myModes); i++){ */
    /*     myModes[i]=i-increment_from-1; */
    /* } */
    modes = "";
    uint8_t num_modes = sizeof(myModes) > 0 ? sizeof(myModes) : ws2812fx.getModeCount();
    for(uint8_t i=0; i < num_modes; i++) {
        uint8_t m = sizeof(myModes) > 0 ? myModes[i] : i;
        modes += "<li><a href='#'>";
        modes += ws2812fx.getModeName(m);
        modes += "</a></li>";
    }
}

// Maximum number of packets to hold in the buffer. Don't change this.
#define BUFFER_LEN 1024
// Toggles FPS output (1 = print FPS over serial, 0 = disable output)
#define PRINT_FPS 1

unsigned int audio_listen_localPort = 7777;
char packetBuffer[BUFFER_LEN];

WiFiUDP audio_listen_port;
boolean audio_active = false;

// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 50, suggested range 20-100 
#define COOLING  66

// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
#define SPARKING 100

// following are just for ws2812fx_custom_FastLed
#define LED_PIN LED_PIN_V1
#define NUM_LEDS LED_COUNT_V1

void Fire2012();
#include "ws2812fx_custom_FastLED.h"
#include "RainbowFireworks.h"
#include "DualLarson.h"

/* #####################################################
   #  Webserver Functions
   ##################################################### */

uint16_t ledsOff(void) {
    WS2812FX::Segment* seg = ws2812fx.getSegment(); // get the current segment
    uint16_t seglen = seg->stop - seg->start + 1;

    if (state_power) {
        ws2812fx.setMode(current_mode);  // mode prior to being reset
                                         // by the user calling a
                                         // function or pressing a
                                         // button
        state_brightness = ws2812fx.getBrightness();
        state_power = false;
        auto_cycle = false;

        ws2812fx.setBrightness(0);

        trigger_eeprom_write();
    }

    return(seg->speed / seglen);
}

uint16_t ledsOn(void) {
    WS2812FX::Segment* seg = ws2812fx.getSegment(); // get the current segment
    uint16_t seglen = seg->stop - seg->start + 1;

    ws2812fx.setBrightness(state_brightness);
    state_power = true;

    trigger_eeprom_write();

    return(seg->speed / seglen);
}

uint16_t warm_light(void) {
    WS2812FX::Segment* seg = ws2812fx.getSegment(); // get the current segment
    uint16_t seglen = seg->stop - seg->start + 1;
    
    ws2812fx.setMode(FX_MODE_STATIC);
    state_brightness=160;
    ws2812fx.setBrightness(state_brightness);
    uint32_t color=16739896; //=(255,110,56)
    ws2812fx.setColor(color);
    ws2812fx.fill(ws2812fx.getColor());

    return(seg->speed / seglen);
}

uint16_t dark_ambient(void) {
    WS2812FX::Segment* seg = ws2812fx.getSegment(); // get the current segment
    uint16_t seglen = seg->stop - seg->start + 1;

    ws2812fx.setMode(FX_MODE_STATIC);
    state_brightness=20;
    ws2812fx.setBrightness(state_brightness);
    uint32_t color=16734464; //=(255,89,0)
    ws2812fx.setColor(color);
    ws2812fx.fill(ws2812fx.getColor());

    return(seg->speed / seglen);
}

void pollButtons() {
    // Read buttons
    mainButton.poll();

    if (mainButton.switched()) {
        fade_dirn = 0;
        syslog.log(LOG_INFO, "main button switched");
    }

    if (mainButton.pushed()) {
        syslog.log(LOG_INFO, "main button pushed");
        if (!triple_clicked) { // if single clicked, turn off (if
                               // held, we just end up resetting
                               // brightness after double-click
                               // delay).  If double-click-held, then
                               // brightness will be restored.  But
                               // for triple-click, we know the next
                               // movement will be to modify
                               // saturation, so don't turn off yet
            time_button_released=0;
            syslog.log(LOG_INFO, "main button pressed, not long, double or triple");

            if (state_power) {
                ledsOff();
            } else {
                ledsOn();
            }
        }
        double_clicked = false;
        triple_clicked = false;
    }
    if (mainButton.released()) {
        syslog.log(LOG_INFO, "main button released");
        if (!long_pressed && !double_clicked && !triple_clicked) {
            time_button_released = millis();
        }
        long_pressed = false;
    } else if (mainButton.tripleClick()) {
        syslog.log(LOG_INFO, "registering main button tripleClick");
        triple_clicked = true;
        double_clicked = false;
        time_button_released=0;
    } else if (mainButton.doubleClick()) {
        syslog.log(LOG_INFO, "registering main button doubleClick");
        double_clicked = true;
        time_button_released=0;
    } else if (long_pressed || mainButton.longPress()) {
        long_pressed = true;
        time_button_released=0;

        uint32_t *colors = ws2812fx.getColors(0); 
        uint8_t r1;
        uint8_t g1;
        uint8_t b1;
        float h1=h1_saved;
        float s1=s1_saved;
        float v1=v1_saved;
        uint32_t color=0;

        if (millis() > hsv_read_time + 50) {
            r1 = (colors[0] >> 16) & 0xff;
            g1 = (colors[0] >>  8) & 0xff;
            b1 =  colors[0]        & 0xff;
            RGBtoHSV( r1, g1, b1, &h1, &s1, &v1 );
        }
        hsv_read_time=millis();

        if (triple_clicked) {
            // saturation, back and forth, constant speed
            s1 += 0.01*s_dirn;
            if (s1 >= 1) {
                s1=0.999;
                s_dirn=-1;
            } else if (s1 <= 0) {
                s1=0.001;
                s_dirn=1;
            }

            syslog.logf(LOG_INFO, "click: setting saturation to %0.2f", s_dirn, s1);
        } else if (double_clicked) {
            // hue, circular, accelerating
            h1 += 0.5; // FIXME: make this depend on time since clicked so accelarates up to a limit
            if (h1 > 360) {
                h1-=360;
            }

            syslog.logf(LOG_INFO, "click: setting hue to %0.2f", h1);
        } else { // single clicked
            // brightness, back and forth, accelerating
            v1 += (v1/20)*v_dirn;
            if (v1 >= 1) {
                v1=0.999; // FIXME: make this depend on time since clicked so accelarates up to a limit
                v_dirn=-1;
            } else if (v1 <= 0.001) {
                v1=0.001; // FIXME: make this depend on time since clicked so accelarates up to a limit
                v_dirn=1;
            }

            syslog.logf(LOG_INFO, "click: setting brightness %i to %0.3f", v_dirn, v1);
        }
        h1_saved=h1;
        s1_saved=s1;
        v1_saved=v1*state_brightness/255;
        state_brightness=255;
        state_power=true;
        HSVtoRGB(&r1,&g1,&b1, h1, s1, v1);
//        syslog.logf(LOG_INFO, "r,g,b=%u,%u,%u", r1,g1,b1);
        color = r1*256*256 + g1*256 + b1;
//        syslog.logf(LOG_INFO, "color=%lu", color);

        colors[0]=color;

//        syslog.logf(LOG_INFO, "set->colors[]=%lu,%lu,%lu", colors[0],colors[1],colors[2]);
        ws2812fx.setColors(0, colors);
        ws2812fx.setBrightness(state_brightness);
        ws2812fx.trigger();

        trigger_eeprom_write();
    }
}

void srv_handle_not_found() {
    server.send(404, "text/plain", "File Not Found\n");
}

void srv_handle_main_js() {
    server.send_P(200,"application/javascript", main_js);
}

void srv_handle_modes() {
    server.send(200,"text/plain", modes);
}

void srv_handle_get() {
    uint32_t *colors = ws2812fx.getColors(0);
    uint8_t r1 = (colors[0] >> 16) & 0xff;
    uint8_t g1 = (colors[0] >>  8) & 0xff;
    uint8_t b1 =  colors[0]        & 0xff;

    uint8_t r2 = (colors[1] >> 16) & 0xff;
    uint8_t g2 = (colors[1] >>  8) & 0xff;
    uint8_t b2 =  colors[1]        & 0xff;

    uint8_t r3 = (colors[2] >> 16) & 0xff;
    uint8_t g3 = (colors[2] >>  8) & 0xff;
    uint8_t b3 =  colors[2]        & 0xff;

    float h1, s1, v1;
    float h2, s2, v2;
    float h3, s3, v3;
    RGBtoHSV( r1, g1, b1, &h1, &s1, &v1 );
    RGBtoHSV( r2, g2, b2, &h2, &s2, &v2 );
    RGBtoHSV( r3, g3, b3, &h3, &s3, &v3 );
    String content = "Current state:\n";
    content = content + "power: " + String(state_power) + "\n";
    content = content + "cycle: " + String(auto_cycle) + "\n";
    content = content + "brightness: " + String(( state_power ? ws2812fx.getBrightness() : state_brightness )) + "\n";
    content = content + "colour: " + String(colors[0]) + " " + String(colors[1]) + " " + String(colors[2]) + "\n";
    content = content + "colourRGB: " + String(r1)+","+String(g1)+","+String(b1) + " " +
        String(r2)+","+String(g2)+","+String(b2) + " " +
        String(r3)+","+String(g3)+","+String(b3) + "\n";
    content = content + "colourHSV: " + String(h1)+","+String(s1)+","+String(v1) + " " +
        String(h2)+","+String(s2)+","+String(v2) + " " +
        String(h3)+","+String(s3)+","+String(v3) + "\n";
    content = content + "mode: " + String(ws2812fx.getMode())+ "\n";
    content = content + "speed: " + String(ws2812fx.getSpeed())+ "\n";
    
    server.send(200,"text/plain", content);
}

void handle_fade() {
    uint32_t *colors = ws2812fx.getColors(0); 
    uint8_t r1;
    uint8_t g1;
    uint8_t b1;
    float h1=h1_saved;
    float s1=s1_saved;
    float v1=v1_saved;
    uint32_t color=0;

    if (millis() > hsv_read_time + 50) {
        r1 = (colors[0] >> 16) & 0xff;
        g1 = (colors[0] >>  8) & 0xff;
        b1 =  colors[0]        & 0xff;
        RGBtoHSV( r1, g1, b1, &h1, &s1, &v1 );
    }
    hsv_read_time = millis();

    v1 *= initial_fade_multiplier;
    v1 += (v1/20)/fade_dirn;
    initial_fade_multiplier = 1;
    if (v1 > 1) {
        v1 = 1;
        fade_dirn = 0;
        ws2812fx.setBrightness(state_brightness);
    } else if ((fade_dirn < 0) && (v1 <= 0.005)) {
        v1 = 1;
        fade_dirn = 0;
        state_power = false;
        ws2812fx.setBrightness(0);
    } else {
        ws2812fx.setBrightness(state_brightness);
    }

    syslog.logf(LOG_INFO, "fade %0.2f (~x0.5s): setting brightness to %0.3f", fade_dirn, v1);
    delay(1); // give time for syslog UDP buffer
    h1_saved = h1;
    s1_saved = s1;
    v1_saved = v1; //*state_brightness/255;
//    state_brightness=255;
//        ws2812fx.setBrightness(state_brightness);
    HSVtoRGB(&r1,&g1,&b1, h1, s1, v1);
//        syslog.logf(LOG_INFO, "r,g,b=%u,%u,%u", r1,g1,b1);
    color = r1*256*256 + g1*256 + b1;
//        syslog.logf(LOG_INFO, "color=%lu", color);

    colors[0] = color;

//        syslog.logf(LOG_INFO, "set->colors[]=%lu,%lu,%lu", colors[0],colors[1],colors[2]);
    ws2812fx.setColors(0, colors);
    ws2812fx.trigger();
}

void srv_handle_set() {
    logQuery();

    bool enabling = true;
    int effecting_change = 0;
    fade_dirn = 0;
    
    for (uint8_t i=0; i < server.args(); i++){
        int this_loop_effecting_change=1;
        if(server.argName(i) == "power") {
            bool new_state_power = strtol(server.arg(i).c_str(), NULL, 10);
            if (new_state_power != state_power) {
                state_power=new_state_power;
            } else {
                this_loop_effecting_change=0;
            }

            if (!state_power) {
                enabling = false;
            }
        } else if(server.argName(i) == "colours") {
            char *arg = strdup(server.arg(i).c_str());
            char *token;

            uint32_t colors[3] = {0,0,0};

            int i=0;
            token = strtok(arg, " ");
            while (token != NULL) {
                colors[i]=(uint32_t) strtol(token, NULL, 10);
                token = strtok(NULL, " ");
                i++;
                if (i > 2)
                    break;
            }

            ws2812fx.setColors(0, colors);
            free(arg);
        } else if(server.argName(i) == "coloursRGB") {
            char *arg = strdup(server.arg(i).c_str());
            char *saveptr1, *saveptr2;
            char *token, *subtoken;
            char *arg_str,*color_str;
            int i,j;

            uint32_t colors[3] = {0,0,0};
//            syslog.logf(LOG_INFO, "arg=%s", arg);

            // example from man strtok
            
            for (arg_str=arg, i = 0; ; i++, arg_str = NULL) {
                token = strtok_r(arg_str, " ", &saveptr1);
                if (token == NULL)
                    break;
                if (i > 2)
                    break;
//                syslog.logf(LOG_INFO, "arg_str=%s", arg_str);
//                syslog.logf(LOG_INFO, "token=%s", token);

                uint32_t color=0;
                for (color_str = token, j = 0; ; j++, color_str = NULL) {
                    subtoken = strtok_r(color_str, ",", &saveptr2);
                    if (subtoken == NULL)
                        break;
                    if (j > 2)
                        break;
//                    syslog.logf(LOG_INFO, "color_str=%s", color_str);
//                    syslog.logf(LOG_INFO, "subtoken=%s", subtoken);

                    color = color*256 + (uint8_t) strtol(subtoken, NULL, 10);
                }
//                syslog.logf(LOG_INFO, "colors[%d]=%lu", i,color);
                colors[i]=color;
            }

            syslog.logf(LOG_INFO, "set->colors[]=%lu,%lu,%lu", colors[0],colors[1],colors[2]);
            ws2812fx.setColors(0, colors);
            free(arg);
        } else if(server.argName(i) == "coloursHSV") {
            char *arg = strdup(server.arg(i).c_str());
            char *saveptr1, *saveptr2;
            char *token, *subtoken;
            char *arg_str,*color_str;
            int i,j;

            uint32_t colors[3] = {0,0,0};
//            syslog.logf(LOG_INFO, "arg=%s", arg);

            // example from man strtok
            
            for (arg_str=arg, i = 0; ; i++, arg_str = NULL) {
                token = strtok_r(arg_str, " ", &saveptr1);
                if (token == NULL)
                    break;
                if (i > 2)
                    break;
//                syslog.logf(LOG_INFO, "arg_str=%s", arg_str);
//                syslog.logf(LOG_INFO, "token=%s", token);

                uint32_t color=0;
                float color_t[3] = {0.0,0.0,0.0};
                for (color_str = token, j = 0; ; j++, color_str = NULL) {
                    subtoken = strtok_r(color_str, ",", &saveptr2);
                    if (subtoken == NULL)
                        break;
                    if (j > 2)
                        break;
//                    syslog.logf(LOG_INFO, "color_str=%s", color_str);
//                    syslog.logf(LOG_INFO, "subtoken=%s", subtoken);

                    color_t[j] = strtof(subtoken, NULL);
                }
                uint8_t r,g,b;
//                syslog.logf(LOG_INFO, "h,s,v=%.2f,%.2f,%.2f", color_t[0], color_t[1], color_t[2]);
                HSVtoRGB(&r,&g,&b, color_t[0], color_t[1], color_t[2]);
//                syslog.logf(LOG_INFO, "r,g,b=%u,%u,%u", r,g,b);
                color = r*256*256 + g*256 + b;
//                syslog.logf(LOG_INFO, "colors[%d]=%lu", i,color);

                colors[i]=color;
            }

            syslog.logf(LOG_INFO, "set->colors[]=%lu,%lu,%lu", colors[0],colors[1],colors[2]);
            ws2812fx.setColors(0, colors);
            free(arg);

            uint32_t color1 = (uint32_t) strtol(server.arg(i).c_str(), NULL, 10);
            uint32_t color2 = (uint32_t) strtol(server.arg(i).c_str(), NULL, 10);
            uint32_t color3 = (uint32_t) strtol(server.arg(i).c_str(), NULL, 10);
//            void HSVtoRGB( int &r, int &g, int &b, float h, float s, float v )
        } else if(server.argName(i) == "speed") {
            uint16_t speed = (uint16_t) strtol(server.arg(i).c_str(), NULL, 10);
            ws2812fx.setSpeed(speed);
        } else if(server.argName(i) == "auto_cycle") {
            uint8_t tmp = (uint8_t) strtol(server.arg(i).c_str(), NULL, 10);
            auto_cycle = tmp;
        } else if(server.argName(i) == "brightness") {
            uint8_t tmp = (uint8_t) strtol(server.arg(i).c_str(), NULL, 10);
            state_brightness = tmp;
            ws2812fx.setBrightness(state_brightness);
        } else if(server.argName(i) == "fadeOn") {
            if (!state_power) {
                fade_dirn = (float) strtof(server.arg(i).c_str(), NULL);
                initial_fade_multiplier=0.005;
                state_power = true;
                ws2812fx.setBrightness(state_brightness);
                syslog.logf(LOG_INFO, "Fade up: fade_dirn=%.2f", fade_dirn);
                commit_Eeprom(); // before the first handle_fade() has fiddled with vf
                handle_fade();
            } else {
                this_loop_effecting_change = 0;
            }
        } else if(server.argName(i) == "fadeOff") {
            if (state_power) {
                fade_dirn = -(float) strtof(server.arg(i).c_str(), NULL);
                syslog.logf(LOG_INFO, "Fade down: fade_dirn=%.2f", fade_dirn);
                commit_Eeprom(); // before the first handle_fade() has fiddled with vf
            } else {
                this_loop_effecting_change = 0;
                enabling = false;
            }
        }

        else if(server.argName(i) == "c") {
            uint32_t tmp = (uint32_t) strtol(server.arg(i).c_str(), NULL, 10);
            if(tmp >= 0x000000 && tmp <= 0xFFFFFF) {
                ws2812fx.setColor(tmp);
            }
            uint32_t color = ws2812fx.getColor();
            uint8_t r = (color >> 16) & 0xff;
            uint8_t g = (color >>  8) & 0xff;
            uint8_t b =  color        & 0xff;
            syslog.logf(LOG_INFO, "c: \"%s\" -> %ul (%u,%u,%u)", server.arg(i).c_str(), color, r,g,b);
        } else if(server.argName(i) == "C") {
            uint32_t tmp = (uint32_t) strtol(server.arg(i).c_str(), NULL, 10);
            if(tmp >= 0x000000 && tmp <= 0xFFFFFF) {
                uint32_t colors[3] = {ws2812fx.getColor(),tmp,0};
                //set the second color, if there was a cursor movement between press and release https://stackoverflow.com/questions/6042202/how-to-distinguish-mouse-click-and-drag
                ws2812fx.setColors(0,colors);
            }
            uint32_t color = ws2812fx.getColor();
            uint8_t r = (color >> 16) & 0xff;
            uint8_t g = (color >>  8) & 0xff;
            uint8_t b =  color        & 0xff;
            syslog.logf(LOG_INFO, "C: \"%s\" -> %ul (%u,%u,%u)", server.arg(i).c_str(), color, r,g,b);
        }

        else if((server.argName(i) == "m") || (server.argName(i) == "mode")) {
            uint8_t tmp = (uint8_t) strtol(server.arg(i).c_str(), NULL, 10);
            syslog.logf(LOG_INFO, "m: %s", server.arg(i).c_str());
            tmp = tmp % ws2812fx.getModeCount();
            tmp = sizeof(myModes) > 0 ? myModes[tmp] : tmp;
            ws2812fx.setMode(tmp);
            syslog.logf(LOG_INFO, "mode is %s", ws2812fx.getModeName(ws2812fx.getMode()));
        } else if(server.argName(i) == "b") {
            if(server.arg(i)[0] == '-') {
                state_brightness=ws2812fx.getBrightness() * 0.8;
            } else if(server.arg(i)[0] == ' ') {
                state_brightness=min(max(ws2812fx.getBrightness(), 5) * 1.2, 255);
            } else { // set brightness directly
                uint8_t tmp = (uint8_t) strtol(server.arg(i).c_str(), NULL, 10);
                state_brightness=tmp;
            }
            ws2812fx.setBrightness(state_brightness);
            syslog.logf(LOG_INFO, "b: '%s' -> %d", server.arg(i).c_str(), state_brightness);
        }

        else if(server.argName(i) == "s") {
            if(server.arg(i)[0] == '-') {
                ws2812fx.setSpeed(max(ws2812fx.getSpeed(), 5) * 1.2);
            } else {
                ws2812fx.setSpeed(ws2812fx.getSpeed() * 0.8);
            }
            syslog.logf(LOG_INFO, "s: '%s' -> %d", server.arg(i).c_str(), ws2812fx.getSpeed());
        }

        else if(server.argName(i) == "a") {
            if(server.arg(i)[0] == '-') {
                auto_cycle = false;
            } else {
                auto_cycle = true;
                auto_last_change = 0;
            }
            syslog.logf(LOG_INFO, "a: \"%s\" -> %d", server.arg(i).c_str(), auto_cycle);
        } else {
            server.send(403, "text/plain", "Unrecognised command\n");
            return;
        }
        effecting_change += this_loop_effecting_change;
    }
    server.send(200, "text/plain", "OK\n");
    if (effecting_change) {
        trigger_eeprom_write();
    }
    if (enabling) {
        state_power = true;
        audio_active = false;
    }
    if (state_power) {
        ws2812fx.setBrightness(state_brightness);
    } else {
        ws2812fx.setBrightness(0);
    }
}

void srv_handle_default() {
    logQuery();

    server.sendHeader("Location", "/",true);   //Redirect to our html web page  
    server.send(302, "text/plain","Resetting to default\n");
    reset_default();
}

void srv_handle_off() {
    logQuery();

    server.sendHeader("Location", "/",true);   //Redirect to our html web page  
    server.send(302, "text/plain","Turning Off\n");
    ledsOff();
}

void srv_handle_on() {
    logQuery();

    server.sendHeader("Location", "/",true);   //Redirect to our html web page  
    server.send(302, "text/plain","Turning On\n");
    ledsOn();
}

// also need to swap green/blue for P1, and reverse pixels
// in P1 - would have to do this for every single led update in
// .show - original initialisation was this, which just sets array
// offsets:
void reverse_show(WS2812FX *strip) {
    uint8_t *pixels = strip->getPixels();
    uint16_t numPixels = strip->numPixels();
    uint8_t bytesPerPixel = strip->getNumBytesPerPixel(); // 3=RGB, 4=RGBW
    uint16_t numBytes = strip->getNumBytes();

    // from WS2812FX fireworks:
    // for better performance, manipulate the Adafruit_NeoPixels pixels[] array directly

    //  memmove(pixels + (dest * bytesPerPixel), pixels + (src * bytesPerPixel), count * bytesPerPixel);

//    Serial.printf("numPixels=%i, bytesPerPixel=%i, numBytes=%i\n", numPixels, bytesPerPixel, numBytes);
    
    for(uint16_t i=0; i <numBytes; i += bytesPerPixel) {
        uint16_t j = numBytes - i-3; // dest, reversed from input
        uint8_t g = pixels[i];
        uint8_t r = pixels[i + 1];
        uint8_t b = pixels[i + 2];

        pixels_p1_reverse[j]     = min(255,b+2); // also insert a constant offset for the dimmer left strip
        pixels_p1_reverse[j + 1] = min(255,r+3);
        pixels_p1_reverse[j + 2] = min(255,g+2);

//        Serial.printf("i=%i, j=%i ; r=%i, g=%i, b=%i\n", i, j, r, g, b);
        if(bytesPerPixel == 4) {
            pixels_p1_reverse[j + 3] = pixels[i + 3]; // for RGBW LEDs
        }
    }

    // get only the first numPixels from the master virtual strip,
    // which correspond to the first physical strip we're trying to
    // swap
    strip->setPixels(numPixels, pixels_p1_reverse, false);
    strip->show();
    strip->setPixels(numPixels, pixels, false); // swap back to normal position
}

boolean pins_reassigned = false;
// update the physical strips's LEDs
void myCustomShow(void) {
//    Serial.println("custom show called now");
    reverse_show(&ws2812fx_p1);
    ws2812fx_p2.show();
    if (!pins_reassigned) {
        pins_reassigned = true;
        Serial.println("reassigning pins after having already rendered");
        ws2812fx_p1.setPin(LED_PIN_P1);
        ws2812fx_p2.setPin(LED_PIN_P2);
    }
//    syslog.logf(LOG_INFO, "time: %d\n", millis());
}

void http_start_stub() {
    Serial.println("HTTP server extra setup");
    server.on("/", handleRoot);
    server.on("/main.js", srv_handle_main_js);
    server.on("/modes", srv_handle_modes);
    server.on("/set", srv_handle_set);
    server.on("/get", srv_handle_get);
    server.on("/default", srv_handle_default);
    server.on("/off", srv_handle_off);
    server.on("/on", srv_handle_on);
//    server.onNotFound(srv_handle_not_found);

    audio_listen_port.begin(audio_listen_localPort);
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

    pinMode(mainButtonPair, OUTPUT);    // In the absense of a spare
                                        // ground pin, and a switch
                                        // with a dual connector,
                                        // let's just set the pin next
                                        // to mainButtonPin LOW, and
                                        // pretend it's ground
    digitalWrite(mainButtonPair, LOW);

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

    // XXXXX Suppress reading of potentially bad value eeprom if button is pressed at bootup
    // Suppress allowing reboot if button pushed at boot
    if (!digitalRead(mainButtonPin)) {
        // can't use mainButton.pushed(), because debounce delayed at bootup
        Serial.println("Main button pushed at boot, so disabling reboot");
        syslog_buffer="Main button pushed at boot, so disabling reboot";
        disabled_at_boot=true;
    }

    ws2812fx.setCustomMode(0, F("Fire2012"), myCustomEffect);
    ws2812fx.setCustomMode(1, F("Dual Larson"), dualLarson);
    ws2812fx.setCustomMode(2, F("Rainbow Fireworks"), rainbowFireworks);
    ws2812fx.setCustomMode(3, F("Off"), ledsOff);
    ws2812fx.setCustomMode(4, F("Reset Default"), reset_default);
    ws2812fx.setCustomMode(5, F("Warm"), warm_light);
    ws2812fx.setCustomMode(6, F("Dark ambient"), dark_ambient);
    // FIXME: funky green: Bedroom (192.168.1.54) P: 1 (HSBK): (22272, 65535, 65535, 4000)
    ws2812fx.setCustomMode(7, F("Reboot"), mode_reboot);

    modes.reserve(5000);
    modes_setup();

    Serial.println("WS2812FX setup");
    ws2812fx.init();

    read_Eeprom();

    /* ws2812fx.setMode(DEFAULT_MODE); */
    /* ws2812fx.setColor(DEFAULT_COLOR); */
    /* ws2812fx.setSpeed(DEFAULT_SPEED); */
    /* ws2812fx.setBrightness(DEFAULT_BRIGHTNESS); */

    ws2812fx.start();

    // init the physical strip's GPIOs and reassign their pixel data
    // pointer to use the virtual strip's pixel data (after saving the
    // location of the strip that will be reversed on-the-fly)
    pixels_p1_reverse=ws2812fx_p1.getPixels();
    ws2812fx_p1.init();
    ws2812fx_p1.setPixels(LED_COUNT_P1, ws2812fx.getPixels(), false);
    ws2812fx_p2.init();
    ws2812fx_p2.setPixels(LED_COUNT_P2, ws2812fx.getPixels() + (LED_COUNT_P1 * ws2812fx.getNumBytesPerPixel()), true);

    // config a custom show() function for the virtual strip, so pixel
    // data gets sent to the physical strips's LEDs instead
    ws2812fx.setCustomShow(myCustomShow);

//    Serial.println("Wifi setup");
//    wifi_setup();
 
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


// from audio-reactive-led-strip/.../ws2812_controller_esp8266.ino
uint16_t fpsCounter = 0;
uint32_t secondTimer = 0;

unsigned long audio_active_time;

boolean check_audio(void) {
    // Read data over socket
    int packetSize = audio_listen_port.parsePacket();
    // If packets have been received, interpret the command
    if (packetSize) {
/* FIXME: test whether this is necessary - test from off, but play audio        if (!state_power) {
            ws2812fx.setBrightness(state_brightness);
            state_power = true;
            } */
        audio_active = true;
        audio_active_time = millis();

        int len = audio_listen_port.read(packetBuffer, BUFFER_LEN);
        uint8_t N = 0;
        for(int i = 0; i < len; i+=4) {
            packetBuffer[len] = 0;
            N = packetBuffer[i];
            ws2812fx.setPixelColor(N, (uint8_t)packetBuffer[i+1], (uint8_t)packetBuffer[i+2], (uint8_t)packetBuffer[i+3]);
        } 
        ws2812fx.show();
        fpsCounter++;

        if (millis() - secondTimer >= 1000U) {
            secondTimer = millis();
            Serial.printf("FPS: %d\n", fpsCounter);
            fpsCounter = 0;
        }
    }
    if (audio_active && (millis() - audio_active_time > 10000)) {
        audio_active = false;
        ws2812fx.setBrightness(state_brightness);
    }
    return audio_active;
}

void loop_stub(void) {
    unsigned long now = millis();

    if (!disabled_at_boot && reboot_at && (now > reboot_at)) {
        reboot();
    }

    write_Eeprom();

    if (!check_audio()) {
        if (auto_cycle && (now - auto_last_change > 10000)) { // cycle effect mode every 10 seconds
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
            syslog.logf(LOG_INFO, "mode is %s", ws2812fx.getModeName(ws2812fx.getMode()));
            auto_last_change = now;
        }
    }
    if (fade_dirn != 0) {
        handle_fade();
    }
    pollButtons();

    if (!disabled_at_boot) {
        ws2812fx.service();
    }
    current_mode=ws2812fx.getMode();
}


