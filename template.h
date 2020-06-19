#ifndef BOILERPLATE_H
#define BOILERPLATE_H

#define _SSID "Asio2.4"
#define _SSID2 "EdimaxWiFiPLC"
#define _PWD "EnrotalphEg4ChrebfeOgEr1"

#define _SYSLOG "192.168.1.17"

#define _HOSTNAME "ledstrip"
#define _OTA_PWD "4quitVojwigig"

void setup_stub();
void loop_stub();
String http_uptime_stub();
void reboot(void);

void ledBright(unsigned int val);
void ledRamp(int start, int finish, unsigned int duration, unsigned int steps);
int getArgValue(String name);

extern int ONBOARD_LED_PIN;
extern int led_range;

#endif
