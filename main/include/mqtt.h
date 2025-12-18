#pragma once

#include <mqtt_client.h>
#include <stdint.h>

void mqtt5_app_start(void);

void enviar_por_mqtt(const char *topic, const char *msg);

void subir_a_thingsboard(const uint8_t mac[6], uint8_t estado_led);