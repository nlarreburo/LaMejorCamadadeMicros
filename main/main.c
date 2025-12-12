#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mesh_init.h"
#include "BLE_init.h"
#include "esp_mesh.h"

static const char *TAG = "MAIN";

extern const struct ble_gatt_svc_def gatt_ctrl_led[];
extern const struct ble_gatt_svc_def gatt_ctrl_credencial[];


void app_main(void)
{   
    esp_err_t err;
    gpio_reset_pin(2);
    gpio_set_direction(2, GPIO_MODE_OUTPUT);
    
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupta o llena, borrando");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    nvs_handle_t my_handle;
    err = nvs_open("mesh_config", NVS_READONLY, &my_handle); //nos devuelve un error si no existe

    uint8_t config_finish = 0;
    if (err == ESP_OK) {
        nvs_get_u8(my_handle,"config_finish",&config_finish);
        nvs_close(my_handle);
    }
    if (config_finish == 1) {
        ESP_LOGI(TAG, "Iniciando mesh");
<<<<<<< HEAD
        ESP_ERROR_CHECK(mesh_app_start("FaIn-Privada", "radioactividad"));
=======
        ESP_ERROR_CHECK(mesh_app_start());
>>>>>>> 1225c67660a9e83df7a085ef09ecde4ff5c8e9a2
        if (esp_mesh_is_root()) {
            start_ble_service("Mesh_root", gatt_ctrl_led);
        } else {
            start_ble_service("Mesh_nodo", gatt_ctrl_led);
        }
    } else {
        ESP_LOGW(TAG, "Todavia no se configuro el modulo");
        start_ble_service("Mesh_config", gatt_ctrl_credencial);
    }

}