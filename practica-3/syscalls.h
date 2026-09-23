#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

#define PIN_VENTRICLE_SENSE 4
#define PIN_VENTRICLE_PACE  5

typedef enum {
  SYS_WAIT_EVENT = 1,
  SYS_GET_TIME_US,
  SYS_WAIT_SENSING,
  SYS_PACE_PULSE,
  SYS_LOG_EVENT,
  SYS_KICK_WATCHDOG
} syscall_id_t;

// API publica para las tareas de usuario
void sys_init(void);
uint64_t sys_get_time_us(void);
void sys_sleep_ms(uint32_t ms);
bool sys_wait_sensing(uint32_t timeout_ms);
bool sys_pace_pulse(uint32_t pulse_width_us);
void sys_log_event(const char *msg);
void sys_kick_watchdog(void);