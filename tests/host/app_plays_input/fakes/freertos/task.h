#pragma once
#include "freertos/FreeRTOS.h"
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *argument, unsigned priority, void *handle);
void vTaskDelay(TickType_t delay);
