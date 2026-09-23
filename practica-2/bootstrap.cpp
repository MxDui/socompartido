#include "bootstrap.h"

#include <Arduino.h>
#include <string.h>
#include <esp_system.h>
#include <esp_crc.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define WDT_TIMEOUT_SECONDS 3
#define SRAM_TEST_SIZE      128
#define PATTERN_A           0xAA55AA55u
#define PATTERN_B           0x55AA55AAu

const uint32_t EXPECTED_FLASH_CRC = 0xC3576568;
const uint8_t flash_block_test[64] = {
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x55,
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x55
};

static volatile uint32_t sram_test_buffer[SRAM_TEST_SIZE];

static void print_reset_reason(void);
static void init_watchdog(void);
static bool post_flash_crc(void);
static bool post_sram_march(void);
static void enter_safe_state(const char *reason);

void bootstrap_init(void) {
    Serial.println();
    Serial.println("=== Marcapasos VVI :: Bootstrapping / POST ===");

    print_reset_reason();
    init_watchdog();

    if (!post_flash_crc()) {
        enter_safe_state("Integridad de Flash fallida (CRC32 no coincide)");
    }

    if (!post_sram_march()) {
        enter_safe_state("Fallo en celdas SRAM (March test)");
    }

    Serial.println("[POST] Autodiagnostico de plataforma CONCLUIDO correctamente.");
    Serial.println("[BOOT] Control transferido a la capa de syscalls.");
}

static void print_reset_reason(void) {
    const esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("[RESET] Codigo: %d — ", static_cast<int>(reason));

    switch (reason) {
    case ESP_RST_UNKNOWN:
        Serial.println("Causa desconocida (no se pudo determinar).");
        break;
    case ESP_RST_POWERON:
        Serial.println("Encendido inicial (power-on).");
        break;
    case ESP_RST_EXT:
        Serial.println("Reinicio por pin externo (no aplica en ESP32 clasico).");
        break;
    case ESP_RST_SW:
        Serial.println("Reinicio por software (esp_restart).");
        break;
    case ESP_RST_PANIC:
        Serial.println("Reinicio por excepcion / panic.");
        break;
    case ESP_RST_INT_WDT:
        Serial.println("Reinicio por Interrupt Watchdog (IWDT).");
        break;
    case ESP_RST_TASK_WDT:
        Serial.println("Reinicio por Task Watchdog (TWDT).");
        break;
    case ESP_RST_WDT:
        Serial.println("Reinicio por otro watchdog de hardware.");
        break;
    case ESP_RST_DEEPSLEEP:
        Serial.println("Despertar desde deep sleep.");
        break;
    case ESP_RST_BROWNOUT:
        Serial.println("Fallo de alimentacion (brownout).");
        break;
    case ESP_RST_SDIO:
        Serial.println("Reinicio por SDIO.");
        break;
    default:
        Serial.println("Causa no catalogada en este firmware.");
        break;
    }
}

static void init_watchdog(void) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };

    esp_err_t err = esp_task_wdt_init(&twdt_config);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_reconfigure(&twdt_config);
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[WDT] init/reconfigure fallo: %s\n", esp_err_to_name(err));
    }
#else
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
    disableCore0WDT();
    disableCore1WDT();
#endif

    Serial.printf("[WDT] Task WDT activo, timeout=%d s, panic=ON\n",
                  WDT_TIMEOUT_SECONDS);
    Serial.println("[WDT] Solo el hilo supervisor (KernelService) se suscribira.");
}

static bool post_flash_crc(void) {
    const uint32_t crc = esp_crc32_le(0, flash_block_test, sizeof(flash_block_test));
    Serial.printf("[POST][Flash] CRC32 calculado=0x%08X  esperado=0x%08X\n",
                  crc, EXPECTED_FLASH_CRC);

    if (crc != EXPECTED_FLASH_CRC) {
        Serial.println("[POST][Flash] FALLO: imagen/bloque inconsistente.");
        return false;
    }

    Serial.println("[POST][Flash] OK");
    return true;
}

static bool post_sram_march(void) {
    for (size_t i = 0; i < SRAM_TEST_SIZE; ++i) {
        sram_test_buffer[i] = PATTERN_A;
        if (sram_test_buffer[i] != PATTERN_A) {
            Serial.printf("[POST][SRAM] FALLO patron A en indice %u\n",
                          static_cast<unsigned>(i));
            return false;
        }

        sram_test_buffer[i] = PATTERN_B;
        if (sram_test_buffer[i] != PATTERN_B) {
            Serial.printf("[POST][SRAM] FALLO patron B en indice %u\n",
                          static_cast<unsigned>(i));
            return false;
        }

        sram_test_buffer[i] = 0x00000000u;
        if (sram_test_buffer[i] != 0x00000000u) {
            Serial.printf("[POST][SRAM] FALLO al restablecer indice %u\n",
                          static_cast<unsigned>(i));
            return false;
        }
    }

    Serial.printf("[POST][SRAM] OK (%u palabras, patrones A/B)\n",
                  static_cast<unsigned>(SRAM_TEST_SIZE));
    return true;
}

static void enter_safe_state(const char *reason) {
    Serial.printf("[CRITICAL ERROR] %s\n", reason);
    Serial.println("[CRITICAL ERROR] Estimulacion INHIBIDA. Estado seguro retenido.");
    Serial.println("[CRITICAL ERROR] WDT desuscrito para evitar bootloop.");
    Serial.flush();

    (void)esp_task_wdt_delete(NULL);

    while (1) {
    }
}
