#include "mqtt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <mqtt_client.h>
#include <string.h>

#define THINGSBOARD_ACCESS_TOKEN "WEsJQBc3vPKGTIVAXJMh" //<-- Token de acceso a ThingsBoard

static const char *TAG = "mqtt_thingsboard";
static esp_mqtt_client_handle_t client = NULL;
static EventGroupHandle_t wifi_event_group = NULL;
static const EventBits_t MQTT_CONNECTED_BIT = BIT0;

static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED al broker de ThingsBoard");
        // Establecemos el bit para señalizar que la conexion esta lista
        if (wifi_event_group) {
            xEventGroupSetBits(wifi_event_group, MQTT_CONNECTED_BIT);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
        // Limpiamos el bit si nos desconectamos
        if (wifi_event_group) {
            xEventGroupClearBits(wifi_event_group, MQTT_CONNECTED_BIT);
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Last error code (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;

    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void mqtt5_app_start(void)
{
    if (!wifi_event_group) {
        wifi_event_group = xEventGroupCreate();
        if (!wifi_event_group) {
            ESP_LOGE(TAG, "No se pudo crear el event group de MQTT");
            return;
        }
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://demo.thingsboard.io",
        .broker.address.port = 1883,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .credentials.username = THINGSBOARD_ACCESS_TOKEN,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void enviar_por_mqtt(const char *topic, const char *msg){
    if (!client) {
        ESP_LOGW(TAG, "Cliente MQTT no inicializado, mensaje descartado");
        return;
    }

    if (!wifi_event_group || !(xEventGroupGetBits(wifi_event_group) & MQTT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "MQTT no conectado, mensaje no enviado");
        return;
    }

    int msg_id = esp_mqtt_client_publish(client, topic, msg, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Fallo al publicar en el topic %s", topic);
    }
}

void subir_a_thingsboard(const uint8_t mac[6], uint8_t estado_led){
    char json_payload[200];
    int len = snprintf(json_payload, sizeof(json_payload),
                       "{\"" MACSTR "\":%d}",
                       MAC2STR(mac), estado_led);

    if (len > 0) {
        enviar_por_mqtt("v1/devices/me/telemetry", json_payload);
    }
}