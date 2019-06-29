#ifndef BOILERPLATE_H
#define BOILERPLATE_H

void setup_stub();
void loop_stub();
String http_uptime_stub();

void ledBright(unsigned int val);
void ledRamp(int start, int finish, unsigned int duration, unsigned int steps);
int getArgValue(String name);

extern int ONBOARD_LED_PIN;
extern int led_range;

#endif
