#include "esp_err.h"

esp_err_t mesh_app_start(void);
void send_command_light(const char *target_mac, int on, int brillo);