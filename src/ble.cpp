#ifdef ENABLE_BLE

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "ble.h"
#include "opendroneid.h"

#define ASTM_AD_TYPE    0xFF
static const uint8_t ASTM_OUI[3] = {0x60, 0x60, 0x1F};
#define CRC16_POLY 0x8005

static portMUX_TYPE ble_mux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t calculateCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    if (data == nullptr || len == 0) return crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ CRC16_POLY;
            else
                crc >>= 1;
        }
    }
    return crc;
}

static NimBLEServer* pBLEServer = nullptr;
static NimBLEAdvertising* pBLEAdvertising = nullptr;
static bool bleInitialized = false;

void ble_mux_init(void) {
    portEXIT_CRITICAL(&ble_mux);
}

void ble_init(void) {
    NimBLEDevice::init("SzDjiTech", true, 16, 1, NIMBLE_CFG_MAX_CONNECTIONS);
    pBLEServer = NimBLEDevice::createServer();
    
    if (pBLEServer == nullptr) {
        Serial.println("BLE Error: Server create failed");
        return;
    }
    
    pBLEAdvertising = pBLEServer->getAdvertising();
    if (pBLEAdvertising == nullptr) {
        Serial.println("BLE Error: Advertising create failed");
        return;
    }
    
    pBLEAdvertising->setAdvertisementType(0x03);  // ADV_TYPE_NONCONN_IND
    pBLEAdvertising->setMinInterval(0x06);
    pBLEAdvertising->setMaxInterval(0x10);
    
    NimBLEAdvertisementData advData;
    advData.setFlags(0x06);  // LE General Discoverable Mode + BR/EDR Not Supported
    pBLEAdvertising->setAdvertisementData(advData);
    
    if (pBLEAdvertising->start()) {
        bleInitialized = true;
        Serial.println("BLE initialized");
    } else {
        Serial.println("BLE Error: Advertising start failed");
    }
}

static void fill_uas_data(ODID_UAS_Data *uas, const char *basic_id,
                          double lat, double lon, int alt,
                          double pilot_lat, double pilot_lon) {

    if (uas == nullptr || basic_id == nullptr) return;
    
    memset(uas, 0, sizeof(*uas));
    
    // Basic ID
    odid_initBasicIDData(&uas->BasicID[0]);
    uas->BasicID[0].UAType = ODID_UATYPE_OTHER;
    uas->BasicID[0].IDType = ODID_IDTYPE_SERIAL_NUMBER;
    strncpy(uas->BasicID[0].UASID, basic_id, ODID_ID_SIZE);
    uas->BasicID[0].UASID[ODID_ID_SIZE] = '\0';
    uas->BasicIDValid[0] = 1;
    
    // Location/Vector
    odid_initLocationData(&uas->Location);
    uas->Location.Status        = ODID_STATUS_AIRBORNE;
    uas->Location.Latitude      = lat;
    uas->Location.Longitude     = lon;
    uas->Location.AltitudeGeo   = alt;
    uas->Location.AltitudeBaro  = alt;
    uas->Location.Height        = (float)alt;
    uas->Location.HeightType    = ODID_HEIGHT_REF_OVER_TAKEOFF;
    uas->Location.HorizAccuracy = ODID_HOR_ACC_10_METER;
    uas->Location.VertAccuracy  = ODID_VER_ACC_10_METER;
    uas->Location.SpeedAccuracy = ODID_SPEED_ACC_1_METERS_PER_SECOND;
    uas->Location.TSAccuracy    = ODID_TIME_ACC_1_0_SECOND;
    uas->LocationValid = 1;
    
    // System
    odid_initSystemData(&uas->System);
    uas->System.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS;
    uas->System.OperatorLatitude     = pilot_lat;
    uas->System.OperatorLongitude    = pilot_lon;
    uas->SystemValid = 1;
}

static size_t build_astm_messages(ODID_UAS_Data *uas, uint8_t *buf, size_t buf_size) {
    if (uas == nullptr || buf == nullptr || buf_size == 0) return 0;
    
    size_t offset = 0;
    // Basic ID
    if (offset + 1 + sizeof(ODID_BasicID_encoded) <= buf_size) {
        buf[offset++] = 0x0;
        memcpy(buf + offset, &uas->BasicID[0], sizeof(ODID_BasicID_encoded));
        offset += sizeof(ODID_BasicID_encoded);
    }
    
    // Location/Vector
    if (offset + 1 + sizeof(ODID_Location_encoded) <= buf_size) {
        buf[offset++] = 0x1;
        memcpy(buf + offset, &uas->Location, sizeof(ODID_Location_encoded));
        offset += sizeof(ODID_Location_encoded);
    }
    
    // System
    if (offset + 1 + sizeof(ODID_System_encoded) <= buf_size) {
        buf[offset++] = 0x2;
        memcpy(buf + offset, &uas->System, sizeof(ODID_System_encoded));
        offset += sizeof(ODID_System_encoded);
    }
    
    return offset;
}

void ble_update(const char* basic_id, double lat, double lon, int alt,
                double pilot_lat, double pilot_lon) {
    if (!bleInitialized) return;
    if (basic_id == nullptr) return;
    
    ODID_UAS_Data uas;
    fill_uas_data(&uas, basic_id, lat, lon, alt, pilot_lat, pilot_lon);
    
    uint8_t astmData[256];
    size_t dataLen = build_astm_messages(&uas, astmData, sizeof(astmData));
    if (dataLen == 0) return;
    
    std::string header;
    header += (char)ASTM_AD_TYPE;
    header.append((const char*)ASTM_OUI, 3);
    
    const uint8_t maxPayloadPerFragment = 31 - header.length() - 2 - 2;

    if (maxPayloadPerFragment <= 0) return;

    if (dataLen > maxPayloadPerFragment) {
        uint8_t totalFragments = (dataLen + maxPayloadPerFragment - 1) / maxPayloadPerFragment;
        for (uint8_t fragmentNum = 0; fragmentNum < totalFragments; fragmentNum++) {
            std::string payload = header;
            payload += (char)fragmentNum;
            payload += (char)totalFragments;
            
            uint16_t offset = fragmentNum * maxPayloadPerFragment;
            uint16_t fragLen = min((uint16_t)maxPayloadPerFragment, (uint16_t)(dataLen - offset));
            payload.append((const char*)astmData + offset, fragLen);
            
            uint16_t crc = calculateCRC16((const uint8_t*)payload.c_str(), payload.length());
            payload += (char)(crc >> 8);
            payload += (char)(crc & 0xFF);
            
            portENTER_CRITICAL(&ble_mux);
            pBLEAdvertising->stop();
            pBLEAdvertising->setAdvertisementData(NimBLEAdvertisementData().addData(payload));
            pBLEAdvertising->start();
            portEXIT_CRITICAL(&ble_mux);
            
            if (fragmentNum < totalFragments - 1) {
                delay(10);
            }
        }
    } else {
        std::string payload = header;
        payload.append((const char*)astmData, dataLen);
        
        uint16_t crc = calculateCRC16((const uint8_t*)payload.c_str(), payload.length());
        payload += (char)(crc >> 8);
        payload += (char)(crc & 0xFF);
        
        portENTER_CRITICAL(&ble_mux);
        pBLEAdvertising->stop();
        pBLEAdvertising->setAdvertisementData(NimBLEAdvertisementData().addData(payload));
        pBLEAdvertising->start();
        portEXIT_CRITICAL(&ble_mux);
    }
}

void ble_stop(void) {
    if (!bleInitialized || pBLEAdvertising == nullptr) return;
    
    portENTER_CRITICAL(&ble_mux);
    pBLEAdvertising->stop();
    NimBLEAdvertisementData empty;
    pBLEAdvertising->setAdvertisementData(empty);
    pBLEAdvertising->start();
    portEXIT_CRITICAL(&ble_mux);
}
#endif // ENABLE_BLE
