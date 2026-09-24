#include "bootstrap.h"
#include "syscalls.h"

#define ESCAPE_INTERVAL_MS 1000
#define REFRACTORY_PERIOD_MS 250

void vvi_controller_task(void *pvParameters) {
  sys_log_event("Controlador VVI iniciado.");

  while (1) {
    sys_kick_watchdog();
    uint32_t listen_window = ESCAPE_INTERVAL_MS - REFRACTORY_PERIOD_MS;

    // Tarea bloqueada por el OS hasta detectar evento o agotar ventana
    bool sensed = sys_wait_sensing(listen_window);

    if (sensed) {
      sys_log_event("Onda R detectada. Estimulacion inhibida.");
    } else {
      sys_log_event("Escape agotado. Estimulando ventriculo...");
      sys_pace_pulse(500);
    }

    // Periodo refractario para evitar sobre-sensado
    sys_sleep_ms(REFRACTORY_PERIOD_MS);
  }
}

void diagnostics_task(void *pvParameters) {
  volatile uint32_t dummy_counter = 0;
  uint32_t last_log = 0;

  while (1) {
    // Carga de procesamiento continua (Busy-wait) para competir por la CPU
    dummy_counter++;

    if (millis() - last_log > 2000) {
      sys_log_event("Diagnostico rutinario en proceso...");
      last_log = millis();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  bootstrap_init();
  vvi_sys_init();

  // Asignacion estricta de prioridades en el mismo nucleo (Core 1)
  // vvi_controller_task expulsara a diagnostics_task en caso de interrupcion
  xTaskCreatePinnedToCore(vvi_controller_task, "VVI_Task", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(diagnostics_task, "Diag_Task", 2048, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}
