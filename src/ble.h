#ifndef _BLE_H
#define _BLE_H

#ifdef ENABLE_BLE

#ifdef __cplusplus
extern "C" {
#endif

void ble_init(void);
void ble_update(const char* basic_id, double lat, double lon, int alt, double pilot_lat, double pilot_lon);
void ble_stop(void);

#ifdef __cplusplus
}
#endif

#endif // ENABLE_BLE
#endif // _BLE_H