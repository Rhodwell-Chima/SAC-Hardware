#ifndef RFID_H
#define RFID_H

#include <Arduino.h>

void initRFID();
void startRFIDTask();

bool checkCardAuth(const String &uid);
void triggerOutput(int32_t pin, uint32_t durationMs);

#endif