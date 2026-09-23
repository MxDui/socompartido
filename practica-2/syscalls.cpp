#include "syscalls.h"

#include <Arduino.h>
#include <string.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/* -------------------------------------------------------------------------- */
/*  Recursos internos del kernel (NO exportados en syscalls.h)                */
/* -------------------------------------------------------------------------- */

#define PIN_VENTRICLE_SENSE  4
#define PIN_VENTRICLE_PACE   5

#define PULSE_WIDTH_MIN_US   100u
#define PULSE_WIDTH_MAX_US   2000u

#define SYSCALL_QUEUE_LEN    8
#define KERNEL_STACK_SIZE    4096
#define LOG_RING_CAP         16
#define LOG_MSG_MAX          80

typedef enum {
    SYS_GET_TIME_US = 0,
    SYS_WAIT_SENSING,
    SYS_PACE_PULSE,
    SYS_LOG_EVENT,
    SYS_KICK_WATCHDOG
} syscall_id_t;

typedef struct {
    syscall_id_t      id;
    union {
        uint32_t      timeout_ms;
        uint32_t      pulse_width_us;
        const char   *log_msg;
    } args;
    union {
        uint64_t      time_us;
        bool          success;
    } result;
    SemaphoreHandle_t done_sem;
} syscall_req_t;

typedef struct {
    uint64_t timestamp_us;
    char     msg[LOG_MSG_MAX];
} log_entry_t;

static QueueHandle_t      syscall_queue = NULL;
static SemaphoreHandle_t  sense_sem     = NULL;
static SemaphoreHandle_t  invoke_mutex  = NULL;
static SemaphoreHandle_t  done_sem      = NULL;

static log_entry_t log_ring[LOG_RING_CAP];
static volatile uint8_t log_head  = 0;
static volatile uint8_t log_count = 0;

static void kernel_service_task(void *pvParameters);
static void IRAM_ATTR sensing_isr(void);
static void syscall_invoke(syscall_req_t *req);
static const log_entry_t *log_ring_push(const char *msg);
static void complete_req(syscall_req_t *req);

/* -------------------------------------------------------------------------- */
/*  API publica: empaquetan la solicitud y bloquean hasta el resultado        */
/* -------------------------------------------------------------------------- */

void sys_init(void) {
    if (syscall_queue != NULL) {
        return;
    }

    pinMode(PIN_VENTRICLE_SENSE, INPUT_PULLUP);
    pinMode(PIN_VENTRICLE_PACE, OUTPUT);
    digitalWrite(PIN_VENTRICLE_PACE, LOW);

    syscall_queue = xQueueCreate(SYSCALL_QUEUE_LEN, sizeof(syscall_req_t *));
    sense_sem     = xSemaphoreCreateBinary();
    invoke_mutex  = xSemaphoreCreateMutex();
    done_sem      = xSemaphoreCreateBinary();

    if (syscall_queue == NULL || sense_sem == NULL ||
        invoke_mutex == NULL || done_sem == NULL) {
        Serial.println("[KERNEL] FALLO al crear primitivas de sincronizacion.");
        return;
    }

    attachInterrupt(digitalPinToInterrupt(PIN_VENTRICLE_SENSE),
                    sensing_isr, FALLING);

    const UBaseType_t kprio = (UBaseType_t)(configMAX_PRIORITIES - 1);
    xTaskCreatePinnedToCore(kernel_service_task, "KernelService",
                            KERNEL_STACK_SIZE, NULL, kprio, NULL, 1);

    Serial.printf("[KERNEL] Despachador listo (prio=%u, sense=GPIO%d, pace=GPIO%d)\n",
                  (unsigned)kprio, PIN_VENTRICLE_SENSE, PIN_VENTRICLE_PACE);
}

uint64_t sys_get_time_us(void) {
    syscall_req_t req;
    memset(&req, 0, sizeof(req));
    req.id = SYS_GET_TIME_US;
    syscall_invoke(&req);
    return req.result.time_us;
}

bool sys_wait_sensing(uint32_t timeout_ms) {
    syscall_req_t req;
    memset(&req, 0, sizeof(req));
    req.id = SYS_WAIT_SENSING;
    req.args.timeout_ms = timeout_ms;
    syscall_invoke(&req);
    return req.result.success;
}

bool sys_pace_pulse(uint32_t width_us) {
    syscall_req_t req;
    memset(&req, 0, sizeof(req));
    req.id = SYS_PACE_PULSE;
    req.args.pulse_width_us = width_us;
    syscall_invoke(&req);
    return req.result.success;
}

void sys_log_event(const char *msg) {
    syscall_req_t req;
    memset(&req, 0, sizeof(req));
    req.id = SYS_LOG_EVENT;
    req.args.log_msg = msg;
    syscall_invoke(&req);
}

void sys_kick_watchdog(void) {
    syscall_req_t req;
    memset(&req, 0, sizeof(req));
    req.id = SYS_KICK_WATCHDOG;
    syscall_invoke(&req);
}

void sys_sleep_ms(uint32_t ms) {
    /* Suspender a la tarea llamante no requiere privilegio de kernel.
     * Si el despachador durmiera aqui, congelaria todas las syscalls. */
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* -------------------------------------------------------------------------- */
/*  Despacho interno                                                          */
/* -------------------------------------------------------------------------- */

static void syscall_invoke(syscall_req_t *req) {
    if (syscall_queue == NULL || req == NULL || done_sem == NULL) {
        return;
    }

    xSemaphoreTake(invoke_mutex, portMAX_DELAY);
    req->done_sem = done_sem;

    syscall_req_t *ptr = req;
    if (xQueueSend(syscall_queue, &ptr, portMAX_DELAY) == pdTRUE) {
        xSemaphoreTake(done_sem, portMAX_DELAY);
    }

    req->done_sem = NULL;
    xSemaphoreGive(invoke_mutex);
}

static void complete_req(syscall_req_t *req) {
    if (req != NULL && req->done_sem != NULL) {
        xSemaphoreGive(req->done_sem);
    }
}

static const log_entry_t *log_ring_push(const char *msg) {
    log_entry_t *slot = &log_ring[log_head];
    slot->timestamp_us = (uint64_t)esp_timer_get_time();

    if (msg == NULL) {
        slot->msg[0] = '\0';
    } else {
        strncpy(slot->msg, msg, LOG_MSG_MAX - 1);
        slot->msg[LOG_MSG_MAX - 1] = '\0';
    }

    log_head = (uint8_t)((log_head + 1) % LOG_RING_CAP);
    if (log_count < LOG_RING_CAP) {
        log_count++;
    }
    return slot;
}

static void IRAM_ATTR sensing_isr(void) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sense_sem, &woken);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    portYIELD_FROM_ISR(woken);
#else
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
#endif
}

static void kernel_service_task(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_add(NULL);

    syscall_req_t *req = NULL;

    while (1) {
        if (xQueueReceive(syscall_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (req == NULL) {
            continue;
        }

        switch (req->id) {
        case SYS_GET_TIME_US:
            req->result.time_us = (uint64_t)esp_timer_get_time();
            complete_req(req);
            break;

        case SYS_WAIT_SENSING: {
            /* Descarta un flanco residual (p. ej. ruido durante el VRP). */
            (void)xSemaphoreTake(sense_sem, 0);
            const TickType_t ticks = pdMS_TO_TICKS(req->args.timeout_ms);
            req->result.success = (xSemaphoreTake(sense_sem, ticks) == pdTRUE);
            complete_req(req);
            break;
        }

        case SYS_PACE_PULSE: {
            const uint32_t width = req->args.pulse_width_us;
            if (width < PULSE_WIDTH_MIN_US || width > PULSE_WIDTH_MAX_US) {
                Serial.printf("[KERNEL ANOMALY @ %lluus] SYS_PACE_PULSE rechazado: "
                              "ancho=%lu us (ventana segura %u-%u us)\r\n",
                              (unsigned long long)esp_timer_get_time(),
                              (unsigned long)width,
                              PULSE_WIDTH_MIN_US, PULSE_WIDTH_MAX_US);
                req->result.success = false;
                complete_req(req);
                break;
            }

            /* Tarea a prioridad maxima: el pulso no lo interrumpe la app. */
            digitalWrite(PIN_VENTRICLE_PACE, HIGH);
            delayMicroseconds(width);
            digitalWrite(PIN_VENTRICLE_PACE, LOW);
            req->result.success = true;
            complete_req(req);
            break;
        }

        case SYS_LOG_EVENT:
            if (req->args.log_msg == NULL) {
                Serial.printf("[KERNEL ANOMALY @ %lluus] SYS_LOG_EVENT puntero nulo\r\n",
                              (unsigned long long)esp_timer_get_time());
                complete_req(req);
                break;
            }
            {
                /* Copiar al anillo (memoria del kernel) y liberar a la tarea
                 * medica ANTES de tocar el puerto serie: el log no la bloquea.
                 * Se imprime desde el slot, no desde req (ya puede no existir). */
                const log_entry_t *entry = log_ring_push(req->args.log_msg);
                complete_req(req);
                Serial.printf("[KERNEL LOG @ %lluus] %s\r\n",
                              (unsigned long long)entry->timestamp_us,
                              entry->msg);
            }
            break;

        case SYS_KICK_WATCHDOG:
            /* Solo el supervisor alimenta el TWDT, y solo si llego la
             * solicitud de la aplicacion (senal de que sigue viva). */
            esp_task_wdt_reset();
            complete_req(req);
            break;

        default:
            Serial.printf("[KERNEL ANOMALY @ %lluus] syscall desconocida id=%d\r\n",
                          (unsigned long long)esp_timer_get_time(),
                          (int)req->id);
            complete_req(req);
            break;
        }
    }
}
