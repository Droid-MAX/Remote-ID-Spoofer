#ifdef ENABLE_BLE

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "ble.h"
#include "opendroneid.h"

#define ASTM_AD_TYPE    0xFF
static const uint8_t ASTM_OUI[3] = {0x60, 0x60, 0x1F};

#define CRC16_POLY 0x8005

static uint16_t calculateCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
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

void ble_init(void) {
    NimBLEDevice::init("SzDjiTech");
    pBLEServer = NimBLEDevice::createServer();
    pBLEAdvertising = pBLEServer->getAdvertising();

    pBLEAdvertising->setAdvertisementType(0x03);  // ADV_TYPE_NONCONN_IND
    pBLEAdvertising->setMinInterval(0x06);
    pBLEAdvertising->setMaxInterval(0x10);
    pBLEAdvertising->setChannelMap(0x07);         // 37,38,39

    NimBLEAdvertisementData advData;
    advData.setFlags(0x06);  // LE General Discoverable Mode + BR/EDR Not Supported
    pBLEAdvertising->setAdvertisementData(advData);
    pBLEAdvertising->start();

    bleInitialized = true;
    Serial.println("BLE initialized");
}

static void fill_uas_data(ODID_UAS_Data *uas, const char *basic_id,
                          double lat, double lon, int alt,
                          double pilot_lat, double pilot_lon) {
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

    ODID_UAS_Data uas;
    fill_uas_data(&uas, basic_id, lat, lon, alt, pilot_lat, pilot_lon);

    uint8_t astmData[256];
    size_t dataLen = build_astm_messages(&uas, astmData, sizeof(astmData));

    std::string header;
    header += (char)ASTM_AD_TYPE;
    header.append((const char*)ASTM_OUI, 3);

    const uint8_t maxPayloadPerFragment = 31 - header.length() - 2 - 2;

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

            NimBLEAdvertisementData advData;
            advData.setFlags(0x06);
            advData.addData(payload);
            pBLEAdvertising->stop();
            pBLEAdvertising->setAdvertisementData(advData);
            pBLEAdvertising->start();

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

        NimBLEAdvertisementData advData;
        advData.setFlags(0x06);
        advData.addData(payload);
        pBLEAdvertising->stop();
        pBLEAdvertising->setAdvertisementData(advData);
        pBLEAdvertising->start();
    }
}

void ble_stop(void) {
    if (!bleInitialized) return;
    pBLEAdvertising->stop();
    NimBLEAdvertisementData empty;
    pBLEAdvertising->setAdvertisementData(empty);
    pBLEAdvertising->start();
}

#endif // ENABLE_BLE