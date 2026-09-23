/*
 * Practica 2 SO — Syscalls / marcapasos VVI
 *
 * Wokwi: https://wokwi.com/projects/new/esp32
 *   1. Crear proyecto ESP32 y subir TODOS los archivos de esta carpeta
 *      (sketch.ino, diagram.json, bootstrap.*, syscalls.*).
 *   2. Iniciar la simulacion. Sin pulsar el boton: escape cada 1000 ms.
 *   3. Pulsar el boton amarillo (GPIO 4) ~500 ms despues de un evento
 *      para inhibir el pulso (onda R simulada).
 *
 * Escenario 4 (validacion defensiva): cambiar PACE_PULSE_US a 50 o 3000.
 */

#include "bootstrap.h"
#include "syscalls.h"

#define ESCAPE_INTERVAL_MS    1000  /* Ritmo base garantizado de 60 ppm */
#define REFRACTORY_PERIOD_MS   250  /* Periodo refractario ventricular (VRP) */

#ifndef PACE_PULSE_US
#define PACE_PULSE_US          500  /* Ancho terapeutico nominal (us) */
#endif

void vvi_controller_task(void *pvParameters) {
    (void)pvParameters;
    sys_log_event("Controlador VVI iniciado.");

    /* Instante del ultimo evento ventricular (pulso o onda R). El intervalo
     * de escape se mide desde aqui. */
    uint64_t cycle_start_us = sys_get_time_us();

    while (1) {
        sys_kick_watchdog();

        /* Ventana de escucha activa nominal = Tescape - VRP = 750 ms.
         * Se descuenta lo ya consumido en este ciclo (VRP + log + pulso)
         * para que los escapes queden separados exactamente Tescape. */
        const uint64_t now_us     = sys_get_time_us();
        const uint32_t elapsed_ms = (uint32_t)((now_us - cycle_start_us) / 1000ULL);
        const uint32_t listen_window =
            (elapsed_ms < ESCAPE_INTERVAL_MS) ? (ESCAPE_INTERVAL_MS - elapsed_ms) : 0;

        const bool sensed = sys_wait_sensing(listen_window);

        /* Sensado o escape: aqui se reinicia el intervalo de escape. */
        cycle_start_us = sys_get_time_us();

        if (sensed) {
            sys_log_event("Onda R detectada. Estimulacion inhibida.");
        } else {
            sys_log_event("Escape agotado. Estimulando ventriculo...");
            (void)sys_pace_pulse(PACE_PULSE_US);
        }

        /* VRP: la tarea duerme y el kernel descarta flancos residuales. */
        sys_sleep_ms(REFRACTORY_PERIOD_MS);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    bootstrap_init();
    sys_init();

    xTaskCreatePinnedToCore(vvi_controller_task, "VVI_Task",
                            4096, NULL, 2, NULL, 0);
}

void loop() {
    /* El hilo de Arduino no forma parte del modelo de privilegio. */
    vTaskDelete(NULL);
}
