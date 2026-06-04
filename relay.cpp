#include "relay.h"
#include "config.h"

// Module state
static SemaphoreHandle_t gRelayLock = NULL;
static TimerHandle_t gRelayTimer = NULL;
static volatile bool gRelayActive = false;
static volatile int32_t gRelayPin = -1;
static volatile uint32_t gRelayOffAt = 0;
static TaskHandle_t gRelayTaskHandle = NULL;

// Timer callback: turns relay off (runs in timer task)
static void relayTimerCb(TimerHandle_t xTimer)
{
    (void)xTimer;

    if (gRelayLock && xSemaphoreTake(gRelayLock, (TickType_t)50) == pdTRUE)
    {
        if (gRelayActive && gRelayPin >= 0)
            digitalWrite((uint8_t)gRelayPin, LOW);
        gRelayActive = false;
        gRelayPin = -1;
        gRelayOffAt = 0;
        xSemaphoreGive(gRelayLock);
        return;
    }

    // Best-effort fallback: try to clear pin without mutex
    noInterrupts();
    if (gRelayActive && gRelayPin >= 0)
        digitalWrite((uint8_t)gRelayPin, LOW);
    gRelayActive = false;
    gRelayPin = -1;
    gRelayOffAt = 0;
    interrupts();
}

// Initialize relay module (pin, mutex, timer)
void initRelay()
{
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    if (gRelayLock == NULL)
        gRelayLock = xSemaphoreCreateMutex();

    if (gRelayTimer == NULL)
        gRelayTimer = xTimerCreate("RelayTimer", pdMS_TO_TICKS(1000), pdFALSE, NULL, relayTimerCb);
}

// Non-blocking trigger: arms relay and schedules turn-off via timer (or fallback)
void triggerOutput(int32_t pin, uint32_t durationMs)
{
    // Fallback when RTOS primitives are not available
    if (gRelayLock == NULL || gRelayTimer == NULL)
    {
        noInterrupts();
        if (gRelayActive)
        {
            interrupts();
            Serial.println("[Relay] Trigger ignored (fallback active).");
            return;
        }
        gRelayActive = true;
        gRelayPin = pin;
        digitalWrite((uint8_t)gRelayPin, HIGH);
        gRelayOffAt = millis() + durationMs;
        interrupts();
        return;
    }

    if (xSemaphoreTake(gRelayLock, (TickType_t)50) != pdTRUE)
    {
        Serial.println("[Relay] Could not take mutex   ignoring trigger.");
        return;
    }

    if (gRelayActive)
    {
        xSemaphoreGive(gRelayLock);
        Serial.println("[Relay] Trigger ignored   relay already active.");
        return;
    }

    gRelayPin = pin;
    gRelayActive = true;
    digitalWrite((uint8_t)gRelayPin, HIGH);

    // (Re)start one-shot timer for the requested duration
    xTimerStop(gRelayTimer, 0);
    xTimerChangePeriod(gRelayTimer, pdMS_TO_TICKS(durationMs), 0);
    xTimerStart(gRelayTimer, 0);

    xSemaphoreGive(gRelayLock);
}

// Relay task: provides fallback deadline-based turn-off when timers unavailable.
// Safe to call startRelayTask() even if timers are present.
static void relayTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        // If a proper RTOS timer exists, let the timer handle turn-off and sleep longer.
        if (gRelayTimer != NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Fallback: check deadline and clear relay when expired.
        if (gRelayActive && gRelayPin >= 0)
        {
            uint32_t now = millis();
            if ((int32_t)(gRelayOffAt - now) <= 0)
            {
                noInterrupts();
                if (gRelayActive && gRelayPin >= 0)
                {
                    digitalWrite((uint8_t)gRelayPin, LOW);
                    gRelayActive = false;
                    gRelayPin = -1;
                    gRelayOffAt = 0;
                }
                interrupts();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Starts the relay task (idempotent)
void startRelayTask()
{
    if (gRelayTaskHandle != NULL)
        return;

    xTaskCreate(relayTask, "RelayTask", 2048, nullptr, 1, &gRelayTaskHandle);
}