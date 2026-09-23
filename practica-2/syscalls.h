#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Interfaz publica de syscalls (espacio de aplicacion).
 *
 * La logica medica SOLO puede usar estas funciones. GPIO, ISR, WDT,
 * colas internas y el despachador viven en syscalls.cpp.
 */

void     sys_init(void);
uint64_t sys_get_time_us(void);
bool     sys_wait_sensing(uint32_t timeout_ms);
bool     sys_pace_pulse(uint32_t width_us);
void     sys_log_event(const char *msg);
void     sys_kick_watchdog(void);
void     sys_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* SYSCALLS_H */
