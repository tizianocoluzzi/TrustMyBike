#pragma once

#include <BLEService.h>

/*
 * Call once from initBLE(), passing the already-created service.
 * Registers OTA control + data characteristics on that service.
 */
void ota_ble_init(BLEService *service);

/*
 * Return true while an OTA update session is active.
 */
bool ota_ble_is_active(void);