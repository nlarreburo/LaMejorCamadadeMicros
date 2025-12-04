#ifndef PACKAGE_H 
#define PACKAGE_H 

#include <stdint.h>
#include "esp_mesh.h"

typedef enum {
    CMD_OFF_ALL         =1,
    CMD_ON_ALL          =2,
    CMD_CTRL_NODO       =3,
    CMD_RESTART         =4,
    CMD_REQ_DATA        =5,
    CMD_REPORT_DATA     =6
} cmd_type_t;

typedef struct{
    uint8_t state;                   //estado del led
    //uint8_t brillo;               //nivel del brillo del led
} led_ctrl_t;

typedef struct{
    uint8_t state_led;
    float volt;
} report_data_t;

typedef struct __attribute__((packed)){
    uint8_t cmd;                  //tipo de comando
    mesh_addr_t target;           //direccion del destinatario
    mesh_addr_t src;
    union{
        led_ctrl_t led;
        report_data_t report;
    } payload;
} mesh_packet_t;         

#endif