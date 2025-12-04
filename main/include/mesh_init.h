#include "esp_err.h"
#include "package.h"
esp_err_t mesh_app_start(void);
void send_mesh_packet(cmd_type_t cmd, const uint8_t *target_mac, uint8_t state);
