#include <stdlib.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_sleep.h"
#endif

#if CONFIG_ESP_SLEEP_DEBUG
#include "esp_private/esp_pmu.h"
#include "esp_private/esp_sleep_internal.h"
#endif

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include "driver/rtc_io.h"
#endif

#include "alarm_timer.h"

#include "esp_zigbee.h"

#include "watermeter.h"


#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "ezbee/bdb.h"
#include "ezbee/app_signals.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/cluster/basic.h"
#include "ezbee/zcl/cluster/identify.h"
#include "ezbee/zcl/cluster/metering.h"
#include "ezbee/af.h"
#include "ezbee/zha.h"


// ============================================================ //
// CONFIGURAÇÕES E PINOS
// ============================================================ //
#define HA_ENDPOINT             1
#define ESP_MANUFACTURER_NAME   "TavaresLAB"
#define ESP_MODEL_IDENTIFIER    "WaterMeter"

#define CNY70_LED_GPIO          GPIO_NUM_4
#define ADC_UNIT_USED           ADC_UNIT_1
#define ADC_CHANNEL_USED        ADC_CHANNEL_0 // GPIO0

// Limites do CNY70 (Histerese)
#define ADC_TRUE_LIMIT          300
#define ADC_FALSE_LIMIT         1000

// Intervalo de amostragem (10 segundos) e estabilização do LED IR
#define LEITURA_INTERVALO_MS    10000
#define LED_ESTABILIZACAO_MS    20

// Incremento por volta detectada (10 Litros)
#define INCREMENTO_LITROS       10

#define NVS_NAMESPACE           "water_meter"
#define NVS_KEY_VOLTAS          "total_voltas"

static adc_oneshot_unit_handle_t adc_handle;

static esp_pm_lock_handle_t light_sleep_block_lock = NULL;

// ============================================================ //
// VARIÁVEIS DE ESTADO E MEDIÇÃO
// ============================================================ //
static volatile int adc_raw = 0;
static volatile bool cny_estado = false;
static volatile uint64_t total_voltas = 0; // Total acumulado em Litros
static volatile bool estado_inicializado = false;
static volatile bool zigbee_connected = false;

portMUX_TYPE meter_mux = portMUX_INITIALIZER_UNLOCKED;

// Protótipo
static void report_zigbee_measurement(uint64_t current_volume);

static const char *TAG = "LIGHT_SLEEP_END_DEVICE";



// ============================================================ //
// ALARM TIMER (AGENDADOR DE TAREFFA) usage alarm_timer_schedule(minha_funcao, meu_argumento(uint), 5000 '5 segundos');
// ============================================================ //

typedef struct alarm_timer_s {
    esp_timer_handle_t     timer;
    alarm_timer_callback_t cb;
    alarm_timer_arg_t      arg;
} alarm_timer_t;

static void alarm_timer_cb(void *arg)
{
    alarm_timer_t *alarm = (alarm_timer_t *)arg;

    esp_timer_delete(alarm->timer);
    alarm->cb(alarm->arg);
    free(alarm);
}

esp_err_t alarm_timer_schedule(alarm_timer_callback_t cb, alarm_timer_arg_t arg, uint32_t time_ms)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    alarm_timer_t *alarm = malloc(sizeof(alarm_timer_t));
    if (alarm == NULL) {
        return ESP_ERR_NO_MEM;
    }

    alarm->cb    = cb;
    alarm->arg   = arg;
    alarm->timer = NULL;

    esp_timer_create_args_t timer_args = {
        .callback = alarm_timer_cb,
        .arg      = alarm,
    };

    esp_err_t err = esp_timer_create(&timer_args, &alarm->timer);
    if (err != ESP_OK) {
        free(alarm);
        return err;
    }

    uint64_t time_us = (uint64_t)time_ms * 1000;
    err              = esp_timer_start_once(alarm->timer, time_us);
    if (err != ESP_OK) {
        esp_timer_delete(alarm->timer);
        free(alarm);
        return err;
    }

    return ESP_OK;
}

// ============================================================ //
// GERENCIAMENTO NVS
// ============================================================ //
static void carregar_total_voltas(void)
{
    nvs_handle_t nvs_h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        total_voltas = 0;
        ESP_LOGI(TAG, "Nenhum contador salvo na NVS. Iniciando em 0 L.");
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para leitura: %s", esp_err_to_name(ret));
        total_voltas = 0;
        return;
    }

    uint32_t valor_salvo = 0;
    ret = nvs_get_u32(nvs_h, NVS_KEY_VOLTAS, &valor_salvo);
    nvs_close(nvs_h);

    if (ret == ESP_OK) {
        total_voltas = (uint64_t)valor_salvo;
        ESP_LOGI(TAG, "Total de volume carregado da NVS: %llu L", total_voltas);
    } else {
        total_voltas = 0;
        ESP_LOGI(TAG, "Falha na leitura da NVS (%s). Iniciando em 0 L.", esp_err_to_name(ret));
    }
}

static void salvar_total_voltas(void)
{
    nvs_handle_t nvs_h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para escrita: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_set_u32(nvs_h, NVS_KEY_VOLTAS, (uint32_t)total_voltas);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao gravar total de voltas na NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_h);
        return;
    }

    ret = nvs_commit(nvs_h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erro no commit da NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Total de volume salvo na NVS: %llu L", total_voltas);
    }
    nvs_close(nvs_h);
}

// ============================================================ //
// INICIALIZAÇÃO DE HARDWARE (GPIO & ADC)
// ============================================================ //
static void cny70_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CNY70_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(CNY70_LED_GPIO, 1); // HIGH = LED IR Desligado
    ESP_LOGI(TAG, "LED IR (GPIO4) configurado.");
}

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_USED
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_USED, &config));
    ESP_LOGI(TAG, "ADC configurado: GPIO0 / ADC1_CHANNEL_0");
}

// ============================================================ //
// TAREFA DE LEITURA DO SENSOR CNY70 (A CADA 10 SEGUNDOS)
// ============================================================ //
static void adc_task(void *pvParameters)
{
    while (1) {
        int raw = 0;

    #if CONFIG_ESP_SLEEP_DEBUG
        esp_pm_dump_locks(stdout);
    #endif /* CONFIG_ESP_SLEEP_DEBUG */

        // 1. Liga o LED IR (GPIO4 LOW)
        gpio_set_level(CNY70_LED_GPIO, 0);

        //esp_pm_dump_locks(stdout);

        // 2. Aguarda estabilizar a emissão (20ms)
        vTaskDelay(pdMS_TO_TICKS(LED_ESTABILIZACAO_MS));

        // 3. Efetua a leitura do ADC
        esp_err_t ret = adc_oneshot_read(adc_handle, ADC_CHANNEL_USED, &raw);

        // 4. Desliga o LED IR imediatamente (GPIO4 HIGH)
        gpio_set_level(CNY70_LED_GPIO, 1);

        if (ret == ESP_OK) {
            adc_raw = raw;
            bool novo_estado = cny_estado;

            if (raw < ADC_TRUE_LIMIT) {
                novo_estado = true;
            } else if (raw > ADC_FALSE_LIMIT) {
                novo_estado = false;
            }

            if (!estado_inicializado) {
                cny_estado = novo_estado;
                estado_inicializado = true;
                ESP_LOGI(TAG, "Estado inicial CNY: %s | ADC: %d | Total: %llu L",
                         cny_estado ? "TRUE" : "FALSE", adc_raw, total_voltas);
            } else {
                // Transição FALSE -> TRUE indica detecção de volta
                if (cny_estado == false && novo_estado == true) {
                    uint64_t novo_total;

                    portENTER_CRITICAL(&meter_mux);
                    total_voltas += INCREMENTO_LITROS;
                    novo_total = total_voltas;
                    portEXIT_CRITICAL(&meter_mux);

                    ESP_LOGI(TAG, ">>> VOLTA DETECTADA! +%d L | Novo Total: %llu L", INCREMENTO_LITROS, novo_total);

                    salvar_total_voltas();

                    report_zigbee_measurement(novo_total);
                }
                cny_estado = novo_estado;
            }
        } else {
            ESP_LOGE(TAG, "Erro na leitura do ADC: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(LEITURA_INTERVALO_MS));
    }
}

// ============================================================ //
// ENVIO ZIGBEE (CLUSTER METERING / ZIGBEE2MQTT)
// ============================================================ //

static void report_zigbee_measurement(uint64_t current_volume)
{
    if (!zigbee_connected) {
        return;
    }

    ezb_zcl_uint48_t volume_total_u48 = (ezb_zcl_uint48_t)(current_volume & 0xFFFFFFFFFFFFULL);


    esp_zigbee_lock_acquire(portMAX_DELAY);

    ezb_zcl_set_attr_value(
        HA_ENDPOINT,
        EZB_ZCL_CLUSTER_ID_METERING,
        EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        0,
        &volume_total_u48,
        false
    );

// 2. Envia Report Attributes (Current Summation Delivered)
    ezb_zcl_report_attr_cmd_t report_cmd = {
        .cmd_ctrl = {
            .fc.direction       = EZB_ZCL_CMD_DIRECTION_TO_CLI,
            .dst_addr.addr_mode = EZB_ADDR_MODE_NONE,   // usa binding table
            .src_ep             = HA_ENDPOINT,
            .cluster_id         = EZB_ZCL_CLUSTER_ID_METERING,
        },
        .payload = {
            .attr_id = EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        },
    };

    ezb_err_t ret = ezb_zcl_report_attr_cmd_req(&report_cmd);
    if (ret != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Falha ao enviar Report Attr (summation): 0x%04x", ret);
    }

    esp_zigbee_lock_release();

    ESP_LOGI(TAG, "Reporte enviado ao Z2M");
}

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}


static void release_light_sleep_lock(alarm_timer_arg_t arg)
{
    if (light_sleep_block_lock != NULL) {
        esp_err_t err = esp_pm_lock_release(light_sleep_block_lock);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Entrevista/Conexao concluida! Light sleep LIBERADO.");
            ezb_nwk_set_rx_on_when_idle(false);
        }
    }
}

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device reboot");
                zigbee_connected = true;
                report_zigbee_measurement(total_voltas);
                alarm_timer_schedule(release_light_sleep_lock, 0, 10000);
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status(0x%02x), please retry", ezb_app_signal_to_string(signal_type), status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            zigbee_connected = true;
            report_zigbee_measurement(total_voltas);
            alarm_timer_schedule(release_light_sleep_lock, 0, 10000);
        } else {
            ESP_LOGW(TAG, "Failed to join network with status(0x%02x)", status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *leave_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Left network successfully with type(0x%02x)", leave_params->leave_type);
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        if (duration) {
            ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds", ezb_nwk_get_panid(), duration);
        } else {
            ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", ezb_nwk_get_panid());
        }
    } break;
    default:
        ESP_LOGI(TAG, "Zigbee APP Signal: %s(type: 0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *default_rsp = (ezb_zcl_cmd_default_rsp_message_t *)message;
        ESP_LOGI(TAG, "Received ZCL Default Response with status(0x%02x)", default_rsp->in.status_code);
    } break;
    default:
        ESP_LOGW(TAG, "ZCL Core Action: ID(0x%04lx)", callback_id);
        break;
    }
}

esp_err_t esp_zigbee_create_sleepy_metering_device(void)
{   
// ---------- Basic ----------
    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version  = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    ezb_zcl_cluster_desc_t basic_desc = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)"\x0A" ESP_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)"\x0A" ESP_MODEL_IDENTIFIER);

        // ---------- Identify ----------
    ezb_zcl_identify_cluster_server_config_t identify_cfg = {
        .identify_time = 0,
    };
    ezb_zcl_cluster_desc_t identify_desc = ezb_zcl_identify_create_cluster_desc(&identify_cfg, EZB_ZCL_CLUSTER_SERVER);

    // ---------- Metering ----------


    ezb_zcl_metering_cluster_server_config_t metering_cfg = {
        .current_summation_delivered = 0,
        .status = EZB_ZCL_METERING_STATUS_DEFAULT_VALUE,
        .unit_of_measure = EZB_ZCL_METERING_UNIT_M3_M3H_BINARY,
        .summation_formatting = 0x12,
        .metering_device_type = EZB_ZCL_METERING_WATER_METERING,
    };
    
    ezb_zcl_cluster_desc_t metering_desc = ezb_zcl_metering_create_cluster_desc(&metering_cfg, EZB_ZCL_CLUSTER_SERVER);

    // Atributos normais (sem reporting)

    ezb_zcl_uint24_t multiplier = 1;
    ezb_zcl_metering_cluster_desc_add_attr(metering_desc,
                                        EZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
                                        &multiplier);

    ezb_zcl_uint24_t divisor = 1000;
    ezb_zcl_metering_cluster_desc_add_attr(metering_desc,
                                        EZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                        &divisor);

    // ============================================================
    // ATRIBUTOS REPORTÁVEIS — usando a função genérica
    // ============================================================

    // Current Summation Delivered JÁ EXISTE → só força a flag de reporting
    ezb_zcl_attr_desc_t attr_desc = ezb_zcl_cluster_get_attr_desc(
        metering_desc,
        EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        EZB_ZCL_STD_MANUF_CODE
    );

    if (attr_desc != EZB_INVALID_ZCL_ATTR_DESC) {
        // Força a flag REPORTING
        ezb_err_t ret = ezb_zcl_attr_desc_set_access(
            attr_desc,
            EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING
        );
        if (ret == EZB_ERR_NONE) {
            ESP_LOGI(TAG, "CurrentSummationDelivered agora é REPORTABLE");
        } else {
            ESP_LOGE(TAG, "Falha ao setar access do CurrentSummationDelivered: 0x%04x", ret);
        }
    } else {
        ESP_LOGE(TAG, "Não encontrou o atributo CurrentSummationDelivered");
    }

    // ---------- Endpoint ----------
    ezb_af_ep_config_t ep_config = {
        .ep_id              = HA_ENDPOINT,                 // campo correto: ep_id
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_METER_INTERFACE_DEVICE_ID,
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_config);

    ezb_af_endpoint_add_cluster_desc(ep_desc, basic_desc);
    ezb_af_endpoint_add_cluster_desc(ep_desc, identify_desc);
    ezb_af_endpoint_add_cluster_desc(ep_desc, metering_desc);


    // ---------- Device ----------
    ezb_af_device_desc_t device_desc = ezb_af_create_device_desc();
    ezb_af_device_add_endpoint_desc(device_desc, ep_desc);
    ezb_af_device_desc_register(device_desc);

    //ezb_zcl_basic_cluster_server_init(HA_ENDPOINT);
    //ezb_zcl_identify_cluster_server_init(HA_ENDPOINT);
    //ezb_zcl_metering_cluster_server_init(HA_ENDPOINT);

    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);

    return ESP_OK;
}

esp_err_t esp_zigbee_setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ESP_ZIGBEE_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ESP_ZIGBEE_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));

    return ESP_OK;
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
esp_err_t esp_pm_entry_light_sleep_cb(int64_t sleep_time_us, void *arg)
{
    ESP_EARLY_LOGI(TAG, "Enter Light Sleep");
    return ESP_OK;
}

esp_err_t esp_pm_exit_light_sleep_cb(int64_t sleep_time_us, void *arg)
{
    ESP_EARLY_LOGI(TAG, "Exit Light Sleep");
    return ESP_OK;
}

esp_pm_sleep_cbs_register_config_t s_sleep_cbs_config = {
    .enter_cb          = esp_pm_entry_light_sleep_cb,
    .exit_cb           = esp_pm_exit_light_sleep_cb,
    .enter_cb_user_arg = NULL,
    .exit_cb_user_arg  = NULL,
    .enter_cb_prior    = 0,
    .exit_cb_prior     = 0,
};
#endif /* CONFIG_PM_LIGHT_SLEEP_CALLBACKS */

#ifdef CONFIG_PM_ENABLE

#if CONFIG_ESP_SLEEP_DEBUG
static esp_sleep_context_t s_sleep_ctx;
static TimerHandle_t       s_sleep_debug_timer;

static void sleep_debug_timer_cb(TimerHandle_t xTimer)
{
    assert(xTimer);

    ESP_EARLY_LOGI(TAG, "sleep_flags 0x%08lx, trigger: 0x%08lx", s_sleep_ctx.sleep_flags, s_sleep_ctx.wakeup_triggers);
    ESP_EARLY_LOGI(TAG, "PMU_SLEEP_PD_TOP: %s", (s_sleep_ctx.sleep_flags & PMU_SLEEP_PD_TOP) ? "True" : "False");
    ESP_EARLY_LOGI(TAG, "PMU_SLEEP_PD_MODEM: %s", (s_sleep_ctx.sleep_flags & PMU_SLEEP_PD_MODEM) ? "True" : "False");
    ESP_EARLY_LOGI(TAG, "Wakeup source: 0x%04x", esp_sleep_get_wakeup_causes());
}
#endif /* CONFIG_ESP_SLEEP_DEBUG */

static esp_err_t esp_pm_light_sleep_config(void)
{
    esp_err_t rc = ESP_OK;
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    int             cur_cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    esp_pm_config_t pm_config        = {
               .max_freq_mhz       = cur_cpu_freq_mhz,
               .min_freq_mhz       = 40,
               .light_sleep_enable = true,
    };
    rc = esp_pm_configure(&pm_config);
    
    if (light_sleep_block_lock == NULL) {
        ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "zb_lock", &light_sleep_block_lock));
    }
    ESP_ERROR_CHECK(esp_pm_lock_acquire(light_sleep_block_lock));
    ESP_LOGI(TAG, "Power Management configurado. Light sleep bloqueado para a entrevista.");

#endif /* CONFIG_FREERTOS_USE_TICKLESS_IDLE */

#if CONFIG_ESP_SLEEP_DEBUG
    esp_sleep_set_sleep_context(&s_sleep_ctx);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    s_sleep_debug_timer = xTimerCreate("Sleep_xTimer", pdMS_TO_TICKS(20000), pdTRUE, NULL, sleep_debug_timer_cb);
    xTimerStart(s_sleep_debug_timer, 0);
#endif /* CONFIG_ESP_SLEEP_DEBUG */

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    rc == ESP_OK ? esp_pm_light_sleep_register_cbs(&s_sleep_cbs_config) : rc;
#endif /* CONFIG_PM_LIGHT_SLEEP_CALLBACKS */
    return rc;
}
#endif /* CONFIG_PM_ENABLE */

static void esp_zigbee_stack_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ESP_ERROR_CHECK(esp_zigbee_setup_commissioning());

    ESP_ERROR_CHECK(esp_zigbee_create_sleepy_metering_device());

    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();

    vTaskDelete(NULL);
}

void app_main(void)
{
    //esp_zigbee_factory_reset();
    //ESP_ERROR_CHECK(nvs_flash_init());
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // NVS da stack Zigbee (obrigatório em v2.x)
    ESP_ERROR_CHECK(nvs_flash_init_partition(ESP_ZIGBEE_STORAGE_PARTITION_NAME));

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Hidrômetro CNY70 + Zigbee (SDK v2.x) - Iniciando");
    ESP_LOGI(TAG, "========================================");

    carregar_total_voltas();
    cny70_led_init();
    adc_init();

#ifdef CONFIG_PM_ENABLE
    ESP_ERROR_CHECK(esp_pm_light_sleep_config());
#endif

    ESP_LOGI(TAG, "Start ESP Zigbee Stack");
    xTaskCreate(adc_task, "cny70_adc_task", 3072, NULL, 5, NULL);
    xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
