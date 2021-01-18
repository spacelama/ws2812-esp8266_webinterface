#ifndef BOILERPLATE_H
#define BOILERPLATE_H

// #define _SYSLOG "syslog ip address"

// #define _SSID "primary ssid"
// #define _SSID2 "failover ssid"
// #define _PWD "wifi wpa2 passphrase (for both primary and failover)"
// #define _HOSTNAME "ledstrip"

// #define _OTA_PWD "ota flash password"

void setup_stub();
void loop_stub();
String http_uptime_stub();
void reboot(void);

void ledBright(unsigned int val);
void ledRamp(int start, int finish, unsigned int duration, unsigned int steps);
int getArgValue(String name);
String getArgValueStr(String name);

extern int ONBOARD_LED_PIN;
extern int led_range;
extern String syslog_buffer;

#endif
