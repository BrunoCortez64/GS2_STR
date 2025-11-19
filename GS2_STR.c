#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"

#define LOG_PREFIX "[WIFI_MONITOR] "

#define TAMANHO_FILA 5
#define TIMEOUT_RECEPCAO_SSID_MS 5000
#define TIMEOUT_MUTEX_MS 1000
#define TIMEOUT_WDT_S 10
#define PERIODO_VERIFICACAO_WIFI 4000
#define PERIODO_SUPERVISAO 5000

const char *listaRedesSeguras[5] = {
    "Rede_Segura_1",
    "Rede_Segura_2",
    "REDE_DIRETORIA_TESTE",
    "Laboratorio_Dev",
    "Home_Office_Admin"
};
const int numRedesSeguras = 5;

QueueHandle_t filaSSID;
SemaphoreHandle_t mutexListaSegura;
TaskHandle_t handleTaskWifiMonitor;
TaskHandle_t handleTaskWifiValidator;
TaskHandle_t handleTaskSupervisor;

volatile bool task_monitor_ok = false;
volatile bool task_validator_ok = false;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void wifi_init_sta(void);

void TaskWifiMonitor(void *parametros) {
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    wifi_ap_record_t ap_info;

    while (1) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            
            char *ssid_conectado = (char *)malloc(33 * sizeof(char));
            
            if (ssid_conectado == NULL) {
                printf(LOG_PREFIX "[MONITOR] Falha ao alocar memoria para o SSID!\n");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            strcpy(ssid_conectado, (char *)ap_info.ssid);

            if (xQueueSend(filaSSID, &ssid_conectado, (TickType_t)0) == pdTRUE) {
                printf(LOG_PREFIX "[MONITOR] Conectado a: %s. Enviado para validacao.\n", ssid_conectado);
                task_monitor_ok = true;
            } else {
                printf(LOG_PREFIX "[MONITOR] Fila cheia! SSID %s descartado.\n", ssid_conectado);
                free(ssid_conectado);
            }

        } else {
            printf(LOG_PREFIX "[MONITOR] Nao conectado. Aguardando conexao...\n");
            task_monitor_ok = true;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(PERIODO_VERIFICACAO_WIFI));
    }
}

void TaskWifiValidator(void *parametros) {
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    char *ssid_recebido;

    while (1) {
        if (xQueueReceive(filaSSID, &ssid_recebido, pdMS_TO_TICKS(TIMEOUT_RECEPCAO_SSID_MS)) == pdTRUE) {
            
            printf(LOG_PREFIX "[VALIDADOR] Recebido SSID: %s. Validando...\n", ssid_recebido);
            bool redeSegura = false;

            if (xSemaphoreTake(mutexListaSegura, pdMS_TO_TICKS(TIMEOUT_MUTEX_MS)) == pdTRUE) {
                
                for (int i = 0; i < numRedesSeguras; i++) {
                    if (strcmp(ssid_recebido, listaRedesSeguras[i]) == 0) {
                        redeSegura = true;
                        break;
                    }
                }
                xSemaphoreGive(mutexListaSegura);
            
            } else {
                printf(LOG_PREFIX "[VALIDADOR] Timeout ao esperar mutex da lista de redes!\n");
                redeSegura = false; 
            }

            if (redeSegura) {
                printf(LOG_PREFIX "[VALIDADOR] REDE SEGURA. (%s) esta na lista.\n", ssid_recebido);
            } else {
                printf("**************************************************\n");
                printf(LOG_PREFIX "[ALERTA] REDE NAO AUTORIZADA DETECTADA!\n");
                printf(LOG_PREFIX "[ALERTA] Conectado a: %s\n", ssid_recebido);
                printf("**************************************************\n");
            }

            free(ssid_recebido);
            task_validator_ok = true;

        } else {
            printf(LOG_PREFIX "[VALIDADOR] Timeout! Nao ha SSIDs para validar.\n");
            task_validator_ok = true; 
        }

        esp_task_wdt_reset();
    }
}

void TaskSupervisor(void *parametros) {
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1) {
        printf("--------------------------------------------------\n");
        printf(LOG_PREFIX "[SUPERVISOR] Status das Tarefas:\n");
        printf(LOG_PREFIX "[SUPERVISOR] - TaskWifiMonitor: %s\n", task_monitor_ok ? "OK" : "FALHA");
        printf(LOG_PREFIX "[SUPERVISOR] - TaskWifiValidator: %s\n", task_validator_ok ? "OK" : "FALHA/TIMEOUT");
        printf("--------------------------------------------------\n");

        if (task_monitor_ok && task_validator_ok) {
             esp_task_wdt_reset();
        } else {
            printf(LOG_PREFIX "[SUPERVISOR] FALHA DETECTADA! Aguardando WDT reiniciar...\n");
        }

        task_monitor_ok = false;
        task_validator_ok = false;

        vTaskDelay(pdMS_TO_TICKS(PERIODO_SUPERVISAO));
    }
}

void app_main(void) {
    printf(LOG_PREFIX "[SISTEMA] Iniciando Monitor de Redes Seguras...\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    printf(LOG_PREFIX "[SISTEMA] Iniciando Wi-Fi em modo Station...\n");
    wifi_init_sta();

    printf(LOG_PREFIX "[SISTEMA] Configurando o Watchdog Timer...\n");
    
    ESP_ERROR_CHECK(esp_task_wdt_deinit());

    esp_task_wdt_config_t config_wdt = {
        .timeout_ms = TIMEOUT_WDT_S * 1000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&config_wdt));
    printf(LOG_PREFIX "[SISTEMA] WDT configurado.\n");

    filaSSID = xQueueCreate(TAMANHO_FILA, sizeof(char *));
    if (filaSSID == NULL) {
        printf(LOG_PREFIX "[SISTEMA] Falha ao criar a fila SSID. Reiniciando...\n");
        esp_restart();
    }

    mutexListaSegura = xSemaphoreCreateMutex();
    if (mutexListaSegura == NULL) {
        printf(LOG_PREFIX "[SISTEMA] Falha ao criar o mutex. Reiniciando...\n");
        esp_restart();
    }
    printf(LOG_PREFIX "[SISTEMA] Fila e Mutex criados.\n");

    xTaskCreate(TaskWifiMonitor,   "TaskWifiMonitor",   4096, NULL, 5, &handleTaskWifiMonitor);
    xTaskCreate(TaskWifiValidator, "TaskWifiValidator", 4096, NULL, 5, &handleTaskWifiValidator);
    xTaskCreate(TaskSupervisor,    "TaskSupervisor",    4096, NULL, 4, &handleTaskSupervisor);

    printf(LOG_PREFIX "[SISTEMA] Tarefas criadas. Monitoramento iniciado.\n");
}

static void event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf(LOG_PREFIX "[WIFI] Modo Station iniciado. Conectando...\n");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        printf(LOG_PREFIX "[WIFI] Desconectado. Tentando reconectar...\n");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        printf(LOG_PREFIX "[WIFI] Conectado! IP obtido: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "REDE_DE_TESTE_INSEGURA",
            .password = "senha123",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf(LOG_PREFIX "[WIFI] Inicializacao do Wi-Fi completa.\n");
}