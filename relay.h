#ifndef RELAY_H
#define RELAY_H

#include <Arduino.h>

void initRelay();
void startRelayTask();
void triggerOutput(int32_t pin, uint32_t durationMs);

#endif
