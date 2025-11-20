#include <stdint.h>
#include "esp_mesh.h"

typedef enum {
    PACKET_TYPE_COMMAND_LIGHT = 1, //comando para controlar el led
} packet_type_t;

typedef struct __attribute__((packed)){
    uint8_t type;                 //tipo de paquete
    mesh_addr_t target;           //direccion del destinatario
    uint8_t on;                   //estado del led
    uint8_t brillo;               //nivel del brillo del led
} command_light_packet_t;         //comando para controlar el led 1..100

