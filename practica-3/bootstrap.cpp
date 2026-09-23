#include "bootstrap.h"
#include <Arduino.h>

// --- Funciones auxiliares heredadas (Mockups de la Practica 1) ---

static void print_reset_reason(void) {
  Serial.println("[BOOT] Motivo de reinicio verificado (Simulado).");
}

static bool post_verify_flash(void) {
  // Simulacion de test CRC32 de memoria Flash
  return true; 
}

static bool post_verify_sram(void) {
  // Simulacion de March test de SRAM
  return true; 
}

void bootstrap_enter_safe_state(const char *failure_reason) {
  Serial.printf("[CRITICAL ERROR] %s\r\n", failure_reason);
  while (1) {
    // Retencion en bucle infinito para evitar ejecucion de codigo medico
    delay(1000);
  }
}

// --- Secuencia principal de arranque ---

void bootstrap_init(void) {
  Serial.println("\r\n========================================");
  Serial.println("   VVI PACEMAKER - BOOTSTRAP SEQUENCE   ");
  Serial.println("========================================");

  print_reset_reason();

  // Configuracion del Watchdog compatible con ESP-IDF v4.x y v5.x
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t twdt_config = {
      .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&twdt_config);
  #else
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  #endif

  if (!post_verify_flash()) {
    bootstrap_enter_safe_state("Corrupcion en memoria Flash (Mismatch CRC32)");
  }

  if (!post_verify_sram()) {
    bootstrap_enter_safe_state("Fallo de celda de memoria en SRAM (March Test)");
  }

  Serial.println("[POST SUCCESS] Integridad validada. Inicializando RTOS y componentes...");
}