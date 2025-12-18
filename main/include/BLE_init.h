#include "host/ble_hs.h"

void start_ble_service(const char* device_name, const struct ble_gatt_svc_def* services);
void notify_nodo(int index_nodo);
extern uint16_t notify_handle;
extern uint16_t conn_handle;