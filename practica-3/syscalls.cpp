#include "syscalls.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

typedef struct {
  syscall_id_t id;
  union {
    uint32_t timeout_ms;
    uint32_t pulse_width_us;
    const char *log_msg;
  } args;
  union {
    uint64_t time_us;
    bool success;
  } result;
  SemaphoreHandle_t done_sem;
} syscall_req_t;

static QueueHandle_t syscall_queue = NULL;
static SemaphoreHandle_t sense_event_sem = NULL;

// ISR para capturar sensado intrinseco (onda R)
static void IRAM_ATTR sense_isr_handler(void) {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  
  // Libera el semaforo para notificar a la tarea medica
  xSemaphoreGiveFromISR(sense_event_sem, &xHigherPriorityTaskWoken);
  
  // Solicita al planificador un cambio de contexto inmediato si la 
  // tarea desbloqueada tiene mayor prioridad que la actual
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// Despachador de Kernel: ejecuta peticiones con acceso al hardware
static void kernel_service_task(void *pvParameters) {
  syscall_req_t req;

  while (1) {
    if (xQueueReceive(syscall_queue, &req, portMAX_DELAY) == pdTRUE) {
      switch (req.id) {
        case SYS_GET_TIME_US:
          req.result.time_us = (uint64_t)esp_timer_get_time();
          break;

        case SYS_WAIT_SENSING:
          req.result.success = (xSemaphoreTake(sense_event_sem, pdMS_TO_TICKS(req.args.timeout_ms)) == pdTRUE);
          break;

        case SYS_PACE_PULSE:
          if (req.args.pulse_width_us >= 100 && req.args.pulse_width_us <= 2000) {
            digitalWrite(PIN_VENTRICLE_PACE, HIGH);
            delayMicroseconds(req.args.pulse_width_us);
            digitalWrite(PIN_VENTRICLE_PACE, LOW);
            req.result.success = true;
          } else {
            req.result.success = false;
          }
          break;

        case SYS_LOG_EVENT:
          Serial.printf("[KERNEL LOG @ %lluus] %s\r\n", (uint64_t)esp_timer_get_time(), req.args.log_msg);
          break;

        case SYS_KICK_WATCHDOG:
          esp_task_wdt_reset();
          break;

        default:
          break;
      }

      if (req.done_sem != NULL) {
        xSemaphoreGive(req.done_sem);
      }
    }
  }
}

void sys_init(void) {
  pinMode(PIN_VENTRICLE_PACE, OUTPUT);
  digitalWrite(PIN_VENTRICLE_PACE, LOW);

  pinMode(PIN_VENTRICLE_SENSE, INPUT_PULLDOWN);
  sense_event_sem = xSemaphoreCreateBinary();
  attachInterrupt(digitalPinToInterrupt(PIN_VENTRICLE_SENSE), sense_isr_handler, RISING);

  syscall_queue = xQueueCreate(10, sizeof(syscall_req_t));
  
  // Nucleo 1, Prioridad Maxima
  xTaskCreatePinnedToCore(kernel_service_task, "KernelService", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 1);
}

// === Wrappers de la API hacia el espacio de usuario ===

uint64_t sys_get_time_us(void) {
  syscall_req_t req = {.id = SYS_GET_TIME_US, .done_sem = xSemaphoreCreateBinary()};
  xQueueSend(syscall_queue, &req, portMAX_DELAY);
  xSemaphoreTake(req.done_sem, portMAX_DELAY);
  vSemaphoreDelete(req.done_sem);
  return req.result.time_us;
}

void sys_sleep_ms(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

bool sys_wait_sensing(uint32_t timeout_ms) {
  syscall_req_t req = {.id = SYS_WAIT_SENSING, .args = {.timeout_ms = timeout_ms}, .done_sem = xSemaphoreCreateBinary()};
  xQueueSend(syscall_queue, &req, portMAX_DELAY);
  xSemaphoreTake(req.done_sem, portMAX_DELAY);
  vSemaphoreDelete(req.done_sem);
  return req.result.success;
}

bool sys_pace_pulse(uint32_t pulse_width_us) {
  syscall_req_t req = {.id = SYS_PACE_PULSE, .args = {.pulse_width_us = pulse_width_us}, .done_sem = xSemaphoreCreateBinary()};
  xQueueSend(syscall_queue, &req, portMAX_DELAY);
  xSemaphoreTake(req.done_sem, portMAX_DELAY);
  vSemaphoreDelete(req.done_sem);
  return req.result.success;
}

void sys_log_event(const char *msg) {
  syscall_req_t req = {.id = SYS_LOG_EVENT, .args = {.log_msg = msg}, .done_sem = NULL};
  xQueueSend(syscall_queue, &req, portMAX_DELAY);
}

void sys_kick_watchdog(void) {
  syscall_req_t req = {.id = SYS_KICK_WATCHDOG, .done_sem = NULL};
  xQueueSend(syscall_queue, &req, portMAX_DELAY);
}