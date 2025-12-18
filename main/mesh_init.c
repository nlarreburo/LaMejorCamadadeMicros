/* 
    Mesh Internal Communication Example

    This example code is in the Public Domain (or CC0 licensed, at your option.)

    Unless required by applicable law or agreed to in writing, this
    software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
    CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "mesh_init.h"
#include "package.h"
#include <time.h>
#include "BLE_init.h"
#include "mqtt.h"


/*******************************************************
 *                Macros
 *******************************************************/

/*******************************************************
 *                Constants
 *******************************************************/
#define RX_SIZE          (1460)
#define TX_SIZE          (1460)

/*******************************************************
 *                Variable Definitions
 *******************************************************/
static const char *MESH_TAG = "MESH_APP";
static const uint8_t MESH_ID[6] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
static uint8_t rx_buf[RX_SIZE] = { 0, };
static bool is_running = true;
static bool is_mesh_connected = false;
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static esp_netif_t *netif_sta = NULL;
nodo_status_t lista_nodos[MAX_NODES] = {0};
ble_subscriber_t lista_suscriptores[10] = {0};
static char g_buffer_list[1024] = {0};
static bool mqtt_started = false;


/*******************************************************
 *                Function Declarations
 *******************************************************/


static void esp_mesh_p2p_rx_main(void *arg);
void task_boton(void *arg);
static void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
int actualizar_lista_nodos(const uint8_t *mac, uint8_t estado_led, float volt, bool activo);

/*******************************************************
 *                Function Definitions
 *******************************************************/

char* tomar_buffer_list(void){
    return g_buffer_list;
}


void task_boton(void *arg)
{
    int estado_anterior = 1;
    while(1)
    {
        int estado_actual = gpio_get_level(0);
        if (estado_anterior && (!estado_actual)){
            gpio_set_level(2,(!gpio_get_level(2)));
            send_mesh_packet(CMD_REPORT_DATA,NULL,0);
        }
        estado_anterior = estado_actual;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

int actualizar_lista_nodos(const uint8_t *mac, uint8_t estado_led, float volt, bool activo)
{
    
    int indice = -1;
    bool encontrado = false;

    uint8_t my_sta_mac[6];
    uint8_t my_ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, my_sta_mac);
    esp_wifi_get_mac(WIFI_IF_AP, my_ap_mac);
    const uint8_t *mac_final = mac;
    if (memcmp(mac, my_ap_mac, 6) == 0) { //IMPORTANTE el root tiene 2 mac, AP y STA, para nuesta lista de nodos nos interesa la STA, por lo tanto verificamos y corregimos
        mac_final = my_sta_mac;           //MAC STA (station) -> es la mac que usamos para conectarnos al router internet
    }                                     //MAC AP (soft acces point) -> punto de acceso, es la mac que los nodos hijos "pueden ver" y conectarse
    for (int i=0; i<MAX_NODES;i++){
        if (lista_nodos[i].existe && memcmp(mac_final,lista_nodos[i].mac,6)==0){
            encontrado = true;
            if (activo){                                                                     //Caso 1: El nodo ya existe y esta activo -> Actualizar datos
                lista_nodos[i].volt = volt;
                lista_nodos[i].status_led = estado_led;
                indice = i;
                subir_a_thingsboard(lista_nodos[i].mac, lista_nodos[i].status_led);
                break;
            } else {                                                                         //Caso 2: El nodo existe pero se desconecto -> Marcarlo como inactivo
                lista_nodos[i].volt = volt;
                lista_nodos[i].status_led = estado_led;
                lista_nodos[i].activo = false;
                break;
            }
        }
        if (!lista_nodos[i].existe && indice == -1){    //buscamos lugar libre
            indice = i;
        }
    }

    if ((!encontrado && indice != -1)){                 //guardamos el nodo
        memcpy(lista_nodos[indice].mac,mac_final,6);
        ESP_LOGW(MESH_TAG,"Nodos agregados: "MACSTR,MAC2STR(mac_final));
        lista_nodos[indice].volt = volt;
        lista_nodos[indice].status_led = estado_led;
        lista_nodos[indice].existe = true;
        lista_nodos[indice].activo = true;
    }

    for(int i=0;i<10;i++){                              //propago la lista de nodos actualizada a mis suscriones de BLE
        if (lista_suscriptores[i].activo){
            send_mesh_packet(CMD_SEND_LIST,lista_suscriptores[i].mac,0);
        }
    }

    return indice;
}


void send_mesh_packet(cmd_type_t cmd, const uint8_t *target_mac, uint8_t state)
{
    //CAMBIO IMPORTANTE: Usar memoria dinámica (Heap) en lugar de Stack
    mesh_packet_t *packet = (mesh_packet_t*) calloc(1, sizeof(mesh_packet_t)); //calloc -> clear allocation, da memoria con ceros
    if (packet == NULL) {
        ESP_LOGE(MESH_TAG, "No hay memoria para enviar paquete");
        return;
    }

    packet->cmd = cmd;
    esp_wifi_get_mac(WIFI_IF_STA, packet->src.addr);    //Guardo la mac del emisor

    switch (packet->cmd){ //Dependiendo del comando que llegue vamos a llenar el "union" del paquete
        case CMD_OFF_ALL:
        case CMD_ON_ALL:
        case CMD_ON_OFF:
        case CMD_CTRL_NODO:
        {
            packet->payload.led.state = state;
            break;
        }

        case CMD_REPORT_DATA:
        {
            packet->payload.report.state_led = gpio_get_level(2);
            packet->payload.report.volt = ((float)rand() / RAND_MAX) * 100;
            break;
        }

        case CMD_REQ_LIST:
        {
            break;
        }

        case CMD_SEND_LIST: // el Root envía la lista completa de nodos conocidos
        {
            int contador = 0;
            for(int i=0;i<MAX_NODES;i++){
                if(lista_nodos[i].existe){ //copiamos los nodos que existen
                    packet->payload.nodos_lista.list[contador] = lista_nodos[i];
                    contador++;
                }
            }
            packet->payload.nodos_lista.cont = contador;
            break;
        }

        case CMD_REMOVE_LIST:
        {
            break;
        }
    }

    mesh_data_t data = {
        .data = (uint8_t *)packet,      //puntero a nuestra estructura
        .size = sizeof(mesh_packet_t),  //tamaño total
        .proto = MESH_PROTO_BIN,        //protocolo binario
        .tos = MESH_TOS_P2P,            //tipo de servicio
    };

    if (target_mac == NULL){            //si no tenemos destinatario
        if(esp_mesh_is_root()){         //si no hay destinatario y soy root hago un broadcast a todos los nodos de la tabla de ruteo
            memset(packet->target.addr, 0xFF, 6);   //broadcast interna para logica
            
            mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
            int route_table_size = 0;
            esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
            for (int i=0; i<route_table_size; i++){
                esp_mesh_send(&route_table[i], &data, MESH_DATA_P2P, NULL, 0);
            }
        } else {    //si soy hijo envio para arriba (root)
            esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
        }
    } else { //si tenemos direccion tenemos un p2p
        memcpy(packet->target.addr, target_mac, 6);
        esp_mesh_send(&packet->target, &data, MESH_DATA_P2P, NULL, 0);
    }

    free(packet);   //libero memoria
}

void esp_mesh_p2p_rx_main(void *arg)
{
    esp_err_t err;
    mesh_addr_t from;
    mesh_data_t data;
    int flag = 0;
    data.data = rx_buf;
    is_running = true;

    while (is_running) 
    {
        data.size = RX_SIZE;    //resetear el tamaño esperado en cada iteracion
        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0); //espera el paquete (bloqueante)
        if (err == ESP_OK && data.size > 0) {
            mesh_packet_t *packet_rec = (mesh_packet_t *)data.data;     // convertimos los bytes crudos a nuestra estructura mesh_packet_t
            ESP_LOGI(MESH_TAG, "comando recibido: %d de "MACSTR, packet_rec->cmd, MAC2STR(packet_rec->src.addr));
            uint8_t my_mac[6];
            uint8_t my_ap_mac[6];
            esp_wifi_get_mac(WIFI_IF_AP, my_ap_mac);
            esp_wifi_get_mac(WIFI_IF_STA, my_mac);
            bool is_for_me = (memcmp(packet_rec->target.addr,my_mac,6) == 0);   //verifico si va mi mac
            bool is_broadcast = (memcmp(packet_rec->target.addr, (uint8_t[6]){0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, 6) == 0); //verifico si va para todos
            bool for_root = (memcmp(packet_rec->target.addr, my_mac, 6) == 0) || (memcmp(packet_rec->target.addr, my_ap_mac, 6) == 0);  //verifico si va al root

            switch (packet_rec->cmd){   //procesamos el comando que nos llega
                case CMD_OFF_ALL:       //apagamos todos los led de los nodos, si soy root propago el msj, si soy nodo (broadcast) lo ejecuto
                {
                    if(esp_mesh_is_root() && !is_broadcast){
                        send_mesh_packet(CMD_OFF_ALL, NULL, 0);
                    }else if(is_broadcast){
                        gpio_set_level(2, 0);
                        send_mesh_packet(CMD_REPORT_DATA,NULL,0);
                    }
                    break;
                }

                case CMD_ON_ALL:    //encendemos todos los led de los nodos, si soy root propago el msj, si soy nodo (broadcast) lo ejecuto
                {   
                    if(esp_mesh_is_root() && !is_broadcast){
                        send_mesh_packet(CMD_ON_ALL, NULL, 0);
                    } else if(is_broadcast){
                        gpio_set_level(2, 1);
                        send_mesh_packet(CMD_REPORT_DATA,NULL,0);
                    }
                    break;
                }

                case CMD_CTRL_NODO: //control individual de un nodo especifico por mac
                {
                    if(esp_mesh_is_root()){
                        if(for_root){       //caso si root se controla a si mismo
                            ESP_LOGW(MESH_TAG,"Entro aca?");
                            gpio_set_level(2, packet_rec->payload.led.state);
                            uint8_t my_mac[6];
                            esp_wifi_get_mac(WIFI_IF_STA, my_mac);
                            int index_nodo = actualizar_lista_nodos(my_mac, packet_rec->payload.led.state, ((float)rand() / RAND_MAX) * 100, true);
                            notify_nodo(index_nodo);
                        } else {            //si no es para el root, reenvio al destino
                            send_mesh_packet(CMD_CTRL_NODO, packet_rec->target.addr,packet_rec->payload.led.state);
                        }
                    } else if(is_for_me) {  //caso si soy nodo hijo, y es para mi
                        gpio_set_level(2, packet_rec->payload.led.state);
                        send_mesh_packet(CMD_REPORT_DATA,NULL,0);   //responder al root con mi nuevo estado
                    }
                    break;
                }

                case CMD_RESTART:
                {
                    esp_restart();
                    break;
                }

                case CMD_REQ_DATA:      //solicitud de datos: si NO SOY ROOT, envio mi reporte
                {
                    if (!esp_mesh_is_root()){
                        send_mesh_packet(CMD_REPORT_DATA,NULL,0);
                    }
                    break;
                }

                case CMD_REPORT_DATA:   //reporte de datos, solo el root procesa los reportes para actualizar la tabla global
                {
                    if (esp_mesh_is_root()){
                        ESP_LOGW(MESH_TAG,"Reporte del nodo: "MACSTR" -> Voltaje: %.2fV, Led: %d",
                                MAC2STR(packet_rec->src.addr),
                                packet_rec->payload.report.volt,
                                packet_rec->payload.report.state_led);
                        int index_nodo = actualizar_lista_nodos(packet_rec->src.addr,packet_rec->payload.report.state_led,packet_rec->payload.report.volt,true);
                        notify_nodo(index_nodo);
                        
                    }
                    break;
                }

                case CMD_ON_OFF: //invierto estado del led
                {
                    if(esp_mesh_is_root() && !is_broadcast){
                        send_mesh_packet(CMD_ON_OFF, NULL, 0);
                    }else if(is_broadcast){
                        gpio_set_level(2,(!gpio_get_level(2)));
                        send_mesh_packet(CMD_REPORT_DATA,NULL,0);
                    }
                    break;
                }

                case CMD_REQ_LIST: //lista y gestion de suscripciones de BLE
                {
                    if(esp_mesh_is_root()){
                        send_mesh_packet(CMD_SEND_LIST, packet_rec->src.addr, 0);   //enviar la tabla actual al solicitante
                        
                        int indice_existente = -1;
                        int primer_hueco_libre = -1;

                        for(int i = 0; i < 10; i++){
                            if (memcmp(lista_suscriptores[i].mac, packet_rec->src.addr, 6) == 0) {  //verifico si existe la mac
                                indice_existente = i;
                                break;
                            }
                            if (!lista_suscriptores[i].activo && primer_hueco_libre == -1){         //busco hueco libre
                                primer_hueco_libre = i;
                            } 
                        }

                        if (indice_existente != -1) {           //reactivo
                            lista_suscriptores[indice_existente].activo = true; 
                            ESP_LOGI(MESH_TAG, "Re-suscribiendo nodo existente en indice %d", indice_existente);
                        } 
                        else if (primer_hueco_libre != -1) {    //nuevo
                            memcpy(lista_suscriptores[primer_hueco_libre].mac, packet_rec->src.addr, 6);
                            lista_suscriptores[primer_hueco_libre].activo = true;
                            ESP_LOGI(MESH_TAG, "Suscribiendo nuevo nodo en indice %d", primer_hueco_libre);
                        } 
                        else {
                            ESP_LOGW(MESH_TAG, "Lista de suscriptores llena");
                        }
                        for(int i = 0; i < 10; i++){            //debug de la lista
                            ESP_LOGW(MESH_TAG, "MAC LISTA %d: " MACSTR " Activo: %s", 
                                     i, 
                                     MAC2STR(lista_suscriptores[i].mac), 
                                     lista_suscriptores[i].activo ? "SI" : "NO");
                        }
                    }
                    break;
                }

                case CMD_SEND_LIST: //recepcion de la lista BLE
                {
                    if(is_for_me && !for_root){
                        
                        memset(g_buffer_list, 0, sizeof(g_buffer_list));    //cnstruimos un string grande concatenando los datos de los nodos
                        int puntero = 0;
                        for(int i=0;i<packet_rec->payload.nodos_lista.cont;i++){    //iteramos sobre los nodos que vienen en el paquete, con un formato especializado para la app ble
                            if (packet_rec->payload.nodos_lista.list[i].existe){
                                int len = snprintf(g_buffer_list + puntero,
                                sizeof(g_buffer_list)-puntero,
                                MACSTR"/%d/%.2f|",
                                MAC2STR(packet_rec->payload.nodos_lista.list[i].mac),
                                packet_rec->payload.nodos_lista.list[i].status_led,
                                packet_rec->payload.nodos_lista.list[i].volt);
                                //packet_rec->payload.nodos_lista.list[i].activo ? 1 : 0
                                struct os_mbuf *om = ble_hs_mbuf_from_flat(g_buffer_list + puntero, len);   //enviamos notificacion parcial por BLE (NimBLE maneja la fragmentacion si es necesario)
                                int rc = ble_gattc_notify_custom(conn_handle,notify_handle,om);
                                if (len>0){ //movemos el puntero
                                    puntero += len;
                                }
                                if (puntero >= sizeof(g_buffer_list)){  //evitar desbordamiento de buffer
                                    ESP_LOGW(MESH_TAG,"Sea recibio los datos en el hijo %s",g_buffer_list);
                                    break;
                                }
                            }
                        }
                        ESP_LOGW(MESH_TAG,"Se recibio los datos en el hijo %s",g_buffer_list);
                    }
                    break;
                }
                case CMD_REMOVE_LIST:   //desuscripcion de la lista BLE
                {
                    if(esp_mesh_is_root()){ 
                        for(int i = 0;i<10;i++){
                            if (memcmp(lista_suscriptores[i].mac, packet_rec->src.addr, 6) == 0) {
                                ESP_LOGW(MESH_TAG,"MAC DESUSCRITA: "MACSTR,MAC2STR(lista_suscriptores[i].mac));
                                lista_suscriptores[i].activo = false;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        } else if (err != ESP_ERR_MESH_TIMEOUT) {
            ESP_LOGE(MESH_TAG, "Error al recibir: %s", esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}

esp_err_t esp_mesh_comm_p2p_start(void)
{
    static bool is_comm_p2p_started = false;
    if (!is_comm_p2p_started) {
        is_comm_p2p_started = true;
        xTaskCreate(esp_mesh_p2p_rx_main, "MPRX", 4096, NULL, 5, NULL);
    }
    return ESP_OK;
}

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint16_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED: {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:"MACSTR"", MAC2STR(id.addr));
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        actualizar_lista_nodos(child_connected->mac,0,0,true);      //soy root y un nodo hijo se conecta, actualizo la lista de nodos
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR"",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        actualizar_lista_nodos(child_disconnected->mac,0,0,false);  //soy root y un nodo hijo se desconecta, actualizo la lista de nodos
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR"",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 no_parent->scan_times);
    }
    /* TODO handler for the failure */
    break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR"%s, ID:"MACSTR", duty:%d",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "", MAC2STR(id.addr), connected->duty);
        last_layer = mesh_layer;
        //mesh_connected_indicator(mesh_layer);
        is_mesh_connected = true;
        if (esp_mesh_is_root()) {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_dhcpc_start(netif_sta);
        }
        esp_mesh_comm_p2p_start();
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 disconnected->reason);
        is_mesh_connected = false;
        //mesh_disconnected_indicator();
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        actualizar_lista_nodos(root_addr->addr,0,0,true);   
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR"",
                 MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED: {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR"",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ: {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR"",
                 switch_req->reason,
                 MAC2STR( switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK: {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR"", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD: {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR"",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH: {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR"",
                 router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    case MESH_EVENT_PS_PARENT_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_PARENT_DUTY>duty:%d", ps_duty->duty);
    }
    break;
    case MESH_EVENT_PS_CHILD_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_CHILD_DUTY>cidx:%d, "MACSTR", duty:%d", ps_duty->child_connected.aid-1,
                MAC2STR(ps_duty->child_connected.mac), ps_duty->duty);
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));
    if (esp_mesh_is_root() && !mqtt_started) {  //inicio mqtt
        ESP_LOGI(MESH_TAG, "Root con IP asignada, inicializando MQTT");
        mqtt5_app_start();
        mqtt_started = true;
    }
}

esp_err_t mesh_app_start(char *SSID_MESH, char *PASSWORD_MESH)
{
    /*  tcpip initialization */
    //ESP_ERROR_CHECK(esp_netif_init());
    /*  event initialization */
    //ESP_ERROR_CHECK(esp_event_loop_create_default());
    /*  create network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, NULL));
    srand(time(NULL));
    /*  wifi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));

    /*  set mesh topology */
    ESP_ERROR_CHECK(esp_mesh_set_topology(CONFIG_MESH_TOPOLOGY));

    /*  set mesh max layer according to the topology */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(128));

    /* Disable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    /* mesh ID */
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    /* router */
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(SSID_MESH);
    memcpy((uint8_t *) &cfg.router.ssid,SSID_MESH, cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, PASSWORD_MESH,
           strlen(PASSWORD_MESH));
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD, strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    /* mesh start */
    xTaskCreate(task_boton, "btn_task", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%" PRId32 ", %s<%d>%s, ps:%d",
             esp_get_minimum_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed",
             esp_mesh_get_topology(), esp_mesh_get_topology() ? "(chain)":"(tree)", 
             esp_mesh_is_ps_enabled());
return ESP_OK;
}