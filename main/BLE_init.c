#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "driver/gpio.h"
#include "os/os_mbuf.h"

#include "mesh_init.h"
#include "BLE_init.h"
#include "package.h"
#include "esp_mac.h"

static const char *BLE_TAG = "BLE_APP";
static char ble_device_name[30];            //nombre del dispositivo
static uint8_t g_own_addr_type;             //dirección de BLE
extern volatile float g_latest_voltage;     //voltaje
static int ble_gap_event(struct ble_gap_event *event, void *arg);
void ble_app_on_sync(void);
void ble_host_task(void *param);
uint16_t notify_handle; //ID de notificacion
uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;   //ID de conn

static int gatt_manager(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op)
    {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:  //LOGICA DE ESCRITURA
        uint8_t received[20];
        uint16_t len = ctxt->om->om_len;
        if (len >= sizeof(received)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        memcpy(received, ctxt->om->om_data, len);
        uint8_t cmd_ble = received[0]; //comando
        ESP_LOGI(BLE_TAG, "Se recibio el comando: %d", cmd_ble);
        
        switch (cmd_ble)
        {
            case 1:
                send_mesh_packet(CMD_OFF_ALL, NULL, 0);
                break;
            case 2:
                send_mesh_packet(CMD_ON_ALL, NULL, 0);
                break;
            case 3:
                if (len>=8){
                    uint8_t *target_mac = &received[1];
                    uint8_t state = received[7];
                    //ESP_LOGW(BLE_TAG,"MAC "MACSTR" Estado: %d", MAC2STR(target_mac), state);
                    send_mesh_packet(CMD_CTRL_NODO, target_mac, state);
                } else {
                    ESP_LOGE(BLE_TAG, "Se ingreso mal el comando");
                }
                break;
            case 4:
                send_mesh_packet(CMD_RESTART, NULL, 0);
                esp_restart();
                break;
            case 5:
                send_mesh_packet(CMD_REQ_DATA, NULL, 0);
                break;
            case 6:
                send_mesh_packet(CMD_ON_OFF, NULL, 0);
                break;
            default:
                ESP_LOGE(BLE_TAG, "Sin comando");
                break;
            
        }
        return 0;

    case BLE_GATT_ACCESS_OP_READ_CHR: //LOGICA DE LECTURA
        {
            if(esp_mesh_is_root()){
                ESP_LOGI(BLE_TAG, "Envio de tabla MAC %d", MAX_NODES);
                char buffer[100] = {0};
                for (int i=0; i<MAX_NODES;i++){
                    if (lista_nodos[i].existe){
                        int len = snprintf(buffer, sizeof(buffer), MACSTR "/%d/%.2f|",
                                                MAC2STR(lista_nodos[i].mac),
                                                lista_nodos[i].status_led,
                                                lista_nodos[i].volt);
                                                //lista_nodos[i].activo ? 1 : 0
                        ESP_LOGI(BLE_TAG, "Datos enviados: %s", buffer);
                        os_mbuf_append(ctxt->om, buffer, len);
                        
                    }
                }
                return 0;
            } else {
                char *buffer_list = tomar_buffer_list();
                ESP_LOGW(BLE_TAG, "Datos por enviar: %s", buffer_list);
                if (strlen(buffer_list)>0){
                    os_mbuf_append(ctxt->om, buffer_list, strlen(buffer_list));
                }
                return 0;
            }
        }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

const struct ble_gatt_svc_def gatt_ctrl_led[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFEA0),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(0xFEA1),
                .access_cb = gatt_manager,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &notify_handle,
            }, {0},
        },
    }, {0},
};

void notify_nodo(int index_nodo)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE){
        return;
    }
    nodo_status_t *nodo = &lista_nodos[index_nodo];
    if (!nodo->existe){
        return;
    }
    char buffer[100];
    int len = snprintf(buffer, sizeof(buffer), MACSTR"/%d/%.2f|",
                              MAC2STR(nodo->mac),
                              nodo->status_led,
                              nodo->volt);
                              //nodo->activo ? 1 : 0
    ESP_LOGW(BLE_TAG,"Valor del buffer: %s",buffer);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buffer, len); //crea el paquete
    if(!om){
        ESP_LOGE(BLE_TAG,"no hay memoria para notificacion");
        return;
    }
    int rc = ble_gattc_notify_custom(conn_handle,notify_handle,om);
    if (rc == 0){
        ESP_LOGW(BLE_TAG,"notificacion enviada: %s",buffer);
    } else {
        ESP_LOGE(BLE_TAG,"error en la notificacion: %d",rc);
    }
}

static int gatt_credenciales(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char received_data[200];
    uint16_t len = ctxt->om->om_len;
    if (len >= sizeof(received_data)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    
    memcpy(received_data, ctxt->om->om_data, len);
    received_data[len] = '\0';

    ESP_LOGI(BLE_TAG, "Credenciales recibidas por BLE: %s", received_data);

    char* mesh_id = strtok(received_data, ",");
    char* mesh_pass = strtok(NULL, ",");
    char* router_ssid = strtok(NULL, ",");
    char* router_pass = strtok(NULL, ",");

    if (mesh_id && mesh_pass) {
        nvs_handle_t my_handle;
        nvs_open("mesh_config", NVS_READWRITE, &my_handle);
        nvs_set_str(my_handle, "mesh_id", mesh_id);
        nvs_set_str(my_handle, "mesh_pass", mesh_pass);
        if (router_ssid && router_pass) {
            nvs_set_str(my_handle, "router_ssid", router_ssid);
            nvs_set_str(my_handle, "router_pass", router_pass);
        }
        nvs_set_u8(my_handle, "config_finish", 1);
        nvs_commit(my_handle);
        nvs_close(my_handle);

        ESP_LOGI(BLE_TAG, "Credenciales guardadas");

        ble_gap_adv_stop();                                 //detener la publicidad BLE actual
        esp_restart();                                      //reiniciar el BLE con los nuevos servicios de control.
    } else {
        ESP_LOGE(BLE_TAG, "Formato de credenciales incorrecto.");
    }
    return 0;
}

const struct ble_gatt_svc_def gatt_ctrl_credencial[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xABCD), // UUID
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(0x1234), // UUID de Característica
                .access_cb = gatt_credenciales,
                .flags = BLE_GATT_CHR_F_WRITE,
            }, {0},
        },
    }, {0},
};

static void restart_ble_advertising(void) {
    struct ble_hs_adv_fields fields;                                    //Configurar los datos básicos (nombre y flags)
    memset(&fields, 0, sizeof(fields));         
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;    //Configurar los parámetros básicos (conectable y visible)
    fields.name = (uint8_t *)ble_device_name;   
    fields.name_len = strlen(ble_device_name);
    fields.name_is_complete = 1;
    ESP_ERROR_CHECK(ble_gap_adv_set_fields(&fields));                   //Iniciar la publicidad

    struct ble_gap_adv_params adv_params;   
    memset(&adv_params, 0, sizeof(adv_params)); 
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;       
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;                   

    ESP_ERROR_CHECK(ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL));
    
    ESP_LOGI(BLE_TAG, "Iniciada publicidad como '%s'", ble_device_name);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch(event->type){
        case BLE_GAP_EVENT_CONNECT:
        {
            ESP_LOGI(BLE_TAG, "Cliente BLE conectado: %d",event->connect.conn_handle);
            conn_handle = event->connect.conn_handle;
            send_mesh_packet(CMD_REQ_LIST,NULL,0);
            break;
        }
        case BLE_GAP_EVENT_DISCONNECT:
        {
            ESP_LOGI(BLE_TAG, "Cliente BLE desconectado");
            conn_handle = event->connect.conn_handle;
            send_mesh_packet(CMD_REMOVE_LIST,NULL,0);
            restart_ble_advertising();
            break;
        }
    }
    return 0;
}

void ble_app_on_sync(void) {
    
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "Error al inferir la dirección; rc=%d", rc);
        return;
    }
    restart_ble_advertising();
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void start_ble_service(const char* device_name, const struct ble_gatt_svc_def* services)
{
    static bool is_ble_initialized = false;
    
    // Guardar el nombre del dispositivo
    strncpy(ble_device_name, device_name, sizeof(ble_device_name) - 1);
    ble_device_name[sizeof(ble_device_name) - 1] = '\0';
    

    
    if (!is_ble_initialized) {
        nimble_port_init();

        // Reiniciar los servicios GATT
        ble_svc_gatt_init();
        ble_gatts_count_cfg(services);
        ble_gatts_add_svcs(services);

        ble_svc_gap_device_name_set(ble_device_name);
        ble_svc_gap_init();
        
        ble_hs_cfg.sync_cb = ble_app_on_sync;
        nimble_port_freertos_init(ble_host_task);

        is_ble_initialized = true;
    } else {
        ble_gap_adv_stop();  //detener la publicidad
        ble_svc_gatt_init();  //reiniciar los servicios GATT
        ble_gatts_count_cfg(services); //configurar los parametros
        ble_gatts_add_svcs(services); //agregar los servicios
        ble_svc_gap_device_name_set(ble_device_name); //establecer el nombre del dispositivo
        restart_ble_advertising();  //reiniciar la publicidad
    }
}