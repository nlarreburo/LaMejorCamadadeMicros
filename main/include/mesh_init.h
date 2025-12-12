#include "esp_err.h"
#include "package.h"
<<<<<<< HEAD
esp_err_t mesh_app_start(char *SSID_MESH, char *PASSWORD_MESH);
=======
esp_err_t mesh_app_start(void);
>>>>>>> 1225c67660a9e83df7a085ef09ecde4ff5c8e9a2
void send_mesh_packet(cmd_type_t cmd, const uint8_t *target_mac, uint8_t state);
