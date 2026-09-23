#pragma once

#include <Arduino.h>
#include "esp_crc.h"
#include "esp_task_wdt.h"

#define SRAM_TEST_SIZE 128
#define WDT_TIMEOUT_SECONDS 3

void bootstrap_init(void);
void bootstrap_enter_safe_state(const char *failure_reason);