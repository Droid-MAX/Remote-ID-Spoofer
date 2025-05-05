#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include "esp_netif.h"
#include "esp_event.h"
#include "opendroneid.h"
#include "odid_wifi.h"
#include <cstring>  // for memset
#include <ArduinoJson.h>
#include <Arduino.h>  // for millis() and Serial
#include "esp_system.h"  // for esp_random()
#include "esp_wifi.h"
#include "esp_wifi_types.h"

// Fallback SSID when no RID is available
static const char* BEACON_SSID     = "Starbucks WiFI";
static const size_t BEACON_SSID_LEN = 14;
static const uint8_t AP_CHANNEL = 6;  // match the channel set in setup()

// === User‑configurable spoof MAC ===
// To override the random MAC, set CONFIG_SPOOF_MAC to a 17‑char "XX:XX:XX:XX:XX:XX" string.
static const char* CONFIG_SPOOF_MAC = "60:60:1f:d3:B2:6a"; 

// Last-received Remote ID data
static char    g_basic_id[ODID_ID_SIZE+1] = "";
static double  g_drone_lat = 0.0, g_drone_lon = 0.0;
static int     g_drone_alt = 0;
static double  g_pilot_lat = 0.0, g_pilot_lon = 0.0;
static bool    g_has_data  = false;
static bool    broadcastEnabled = true;

// Dynamic override for source MAC from JSON
static bool    g_dynamic_override = false;
static uint8_t g_override_src_mac[6] = {0};

// Optional override for the source MAC in the beacon frame:
// Set USE_OVERRIDE_MAC to true and OVERRIDE_SRC_MAC to your desired MAC bytes.
static const bool USE_OVERRIDE_MAC = false;
static const uint8_t OVERRIDE_SRC_MAC[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };

static const uint16_t BEACON_INTERVAL = 100;
// sequence counter for cycling ODID message types
static uint8_t g_send_counter = 0;


// Build and inject binary ODID messages as a Vendor IE
static void inject_vendor_ie(const char *basic_id,
                             double drone_lat, double drone_lon,
                             int drone_alt,
                             double pilot_lat, double pilot_lon) {
    // Initialize UAS data struct
    ODID_UAS_Data uas;
    memset(&uas, 0, sizeof(uas));

    // Populate BasicID
    odid_initBasicIDData(&uas.BasicID[0]);
    uas.BasicID[0].UAType = ODID_UATYPE_OTHER;
    uas.BasicID[0].IDType = ODID_IDTYPE_SERIAL_NUMBER;
    strncpy(uas.BasicID[0].UASID, basic_id, ODID_ID_SIZE);
    uas.BasicID[0].UASID[ODID_ID_SIZE] = '\0';
    uas.BasicIDValid[0] = 1;

    // Populate Location
    odid_initLocationData(&uas.Location);
    uas.Location.Latitude    = drone_lat;
    uas.Location.Longitude   = drone_lon;
    uas.Location.AltitudeGeo = drone_alt;
    uas.LocationValid        = 1;

    // Populate System (Operator)
    odid_initSystemData(&uas.System);
    uas.System.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS;
    uas.System.OperatorLatitude     = pilot_lat;
    uas.System.OperatorLongitude    = pilot_lon;
    uas.SystemValid                 = 1;

    // choose source MAC: dynamic override takes priority
    uint8_t ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    uint8_t *src_mac = g_dynamic_override ? g_override_src_mac : ap_mac;

    // Build SSID for beacon IE: fallback to static until real RID arrives
    char ssidBuf[ODID_ID_SIZE + 10];
    if (basic_id[0] == '\0') {
        memcpy(ssidBuf, BEACON_SSID, BEACON_SSID_LEN);
        ssidBuf[BEACON_SSID_LEN] = '\0';
    } else {
        snprintf(ssidBuf, sizeof(ssidBuf), "RID-%s", basic_id);
    }
    size_t ssidLen = strlen(ssidBuf);

    // Build a full beacon frame with dynamic SSID
    uint8_t frame_buf[512];
    int frame_len = odid_wifi_build_message_pack_beacon_frame(
        &uas,
        (char*)src_mac,
        ssidBuf,
        ssidLen,
        BEACON_INTERVAL,
        g_send_counter,
        frame_buf,
        sizeof(frame_buf)
    );
    if (frame_len > 0) {
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame_buf, frame_len, true);
        // Serial.printf("Beacon tx err=%d, len=%d\n", err, frame_len);
        // cycle to the next message type for the next beacon (wrap every 3)
        g_send_counter = (g_send_counter + 1) % 3;
    } else {
        // Serial.printf("Beacon build failed: %d\n", frame_len);
    }
    return;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    // Serial.println("Starting AP with Vendor IE...");
    // Determine default override MAC: config takes priority, else random 60:60:1f:XX:YY:ZZ
    if (CONFIG_SPOOF_MAC && strlen(CONFIG_SPOOF_MAC) == 17) {
        unsigned int b[6];
        if (sscanf(CONFIG_SPOOF_MAC, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (int i = 0; i < 6; ++i) {
                g_override_src_mac[i] = (uint8_t)b[i];
            }
            g_dynamic_override = true;
            // Serial.printf("Using configured spoof MAC %s\n", CONFIG_SPOOF_MAC);
        }
    } else {
        uint32_t rnd = esp_random();
        g_override_src_mac[0] = 0x60;
        g_override_src_mac[1] = 0x60;
        g_override_src_mac[2] = 0x1f;
        g_override_src_mac[3] = (rnd      ) & 0xFF;
        g_override_src_mac[4] = (rnd >>  8) & 0xFF;
        g_override_src_mac[5] = (rnd >> 16) & 0xFF;
        g_dynamic_override   = true;
        // Serial.printf("Using randomized spoof MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
        //     g_override_src_mac[0], g_override_src_mac[1], g_override_src_mac[2],
        //     g_override_src_mac[3], g_override_src_mac[4], g_override_src_mac[5]);
    }

    // Initialize TCP/IP network interface and default event loop
    ESP_ERROR_CHECK( esp_netif_init() );
    ESP_ERROR_CHECK( esp_event_loop_create_default() );
    // Create default Wi-Fi AP network interface
    esp_netif_create_default_wifi_ap();
    // Initialize Wi-Fi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_APSTA) );
    // disable power-save for raw TX
    ESP_ERROR_CHECK( esp_wifi_set_ps(WIFI_PS_NONE) );
    // Configure AP settings
    // Build fallback SSID: use static until we get a real RID
    wifi_config_t ap_config = { 0 };
    char ssidBuf[ODID_ID_SIZE + 10];
    if (g_basic_id[0] == '\0') {
        memcpy(ssidBuf, BEACON_SSID, BEACON_SSID_LEN);
        ssidBuf[BEACON_SSID_LEN] = '\0';
    } else {
        snprintf(ssidBuf, sizeof(ssidBuf), "RID-%s", g_basic_id);
    }
    size_t ssidLen = strlen(ssidBuf);
    memcpy(ap_config.ap.ssid, ssidBuf, ssidLen);
    ap_config.ap.ssid_len = ssidLen;
    ap_config.ap.channel = AP_CHANNEL;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &ap_config) );
    // Start Wi-Fi
    ESP_ERROR_CHECK( esp_wifi_start() );
    // Serial.println("ESP32 AP started on channel " + String(ap_config.ap.channel));
    // Re-assert the channel for raw TX
    ESP_ERROR_CHECK( esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE) );
}

void loop() {
    static char json_buf[512];
    static size_t json_idx = 0;
    static unsigned long lastTx = 0;
    // Transmit roughly once per BEACON_INTERVAL (TU ≈ ms)
    const unsigned long TX_INTERVAL = BEACON_INTERVAL; // approx. 100 TU ≈ 100 ms

    // 1) Read incoming JSON
    while (Serial.available()) {
        char c = Serial.read();
        // Skip carriage returns
        if (c == '\r') continue;
        if (c == '\n' || json_idx >= sizeof(json_buf)-1) {
            json_buf[json_idx] = '\0';
            json_idx = 0;
            // Ignore any lines that don’t look like JSON
            if (json_buf[0] != '{') {
                // not JSON, discard
                continue;
            }
            Serial.print("Parsing JSON: ");
            Serial.println(json_buf);
            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, json_buf);
            if (!err) {
                // Skip mission-start JSON that only defines the path but no coordinates (unless it carries an action)
                if (doc.containsKey("path") && !doc.containsKey("drone_lat") && !doc.containsKey("action")) {
                    // Serial.print("Skipping mission-start JSON: ");
                    // Serial.println(json_buf);
                    continue;
                }
                // Update globals
                if (doc.containsKey("basic_id")) {
                    const char* id = doc["basic_id"];
                    strncpy(g_basic_id, id, ODID_ID_SIZE);
                    g_basic_id[ODID_ID_SIZE] = '\0';
                }
                if (doc.containsKey("drone_lat") && doc.containsKey("drone_long") && doc.containsKey("drone_altitude")) {
                    g_drone_lat  = doc["drone_lat"];
                    g_drone_lon  = doc["drone_long"];
                    g_drone_alt  = doc["drone_altitude"];
                }
                if (doc.containsKey("pilot_lat") && doc.containsKey("pilot_long")) {
                    g_pilot_lat  = doc["pilot_lat"];
                    g_pilot_lon  = doc["pilot_long"];
                }
                g_has_data   = true;
                // Serial.println("JSON updated");
                // Serial.println("Immediate beacon after JSON update");
                // update ESP32 AP SSID to match new RID or fallback
                if (broadcastEnabled) {
                    wifi_config_t ap_cfg = {};
                    char apSsidBuf[ODID_ID_SIZE + 10];
                    size_t apSsidLen;
                    // fallback to static SSID if no Basic ID
                    if (g_basic_id[0] == '\0') {
                        memcpy(apSsidBuf, BEACON_SSID, BEACON_SSID_LEN);
                        apSsidBuf[BEACON_SSID_LEN] = '\0';
                        apSsidLen = BEACON_SSID_LEN;
                    } else {
                        apSsidLen = snprintf(apSsidBuf, sizeof(apSsidBuf), "RID-%s", g_basic_id);
                    }
                    memcpy(ap_cfg.ap.ssid, apSsidBuf, apSsidLen);
                    ap_cfg.ap.ssid_len       = uint8_t(apSsidLen);
                    ap_cfg.ap.channel        = AP_CHANNEL;
                    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
                    ap_cfg.ap.max_connection = 4;
                    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) );
                    ESP_ERROR_CHECK( esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE) );
                }

                // Check for dynamic MAC override
                if (doc.containsKey("mac")) {
                    const char* jsonMac = doc["mac"] | "";
                    if (strlen(jsonMac) == 17) {
                        unsigned int b[6];
                        if (sscanf(jsonMac, "%02x:%02x:%02x:%02x:%02x:%02x",
                                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
                            for (int i = 0; i < 6; ++i) g_override_src_mac[i] = (uint8_t)b[i];
                            g_dynamic_override = true;
                        }
                    } else {
                        // malformed or empty override disables override
                        g_dynamic_override = false;
                    }
                }
                // Check for stop command or empty path to disable broadcasting
                const char* action = doc["action"] | nullptr;
                if (action) {
                    if (strcmp(action, "stop") == 0) {
                        // Stop all broadcasting
                        broadcastEnabled = false;
                        g_has_data = false;
                        Serial.println("STOP received: broadcasts disabled");
                    }
                    else if (strcmp(action, "start") == 0) {
                        // Restart broadcasting on new mission
                        broadcastEnabled = true;
                        g_has_data = true;
                        Serial.println("START received: broadcasts enabled");
                    }
                    else if (strcmp(action, "pause") == 0) {
                        // Freeze location but continue broadcasting last-known position
                        Serial.println("PAUSE received: position frozen");
                    }
                }
                // If mission path is empty array, also stop
                if (doc.containsKey("path")) {
                    JsonArray path = doc["path"].as<JsonArray>();
                    if (path && path.size() == 0) {
                        g_has_data = false;
                    }
                }

                // only inject immediately if still broadcasting
                if (broadcastEnabled && g_has_data) {
                    inject_vendor_ie(
                        g_basic_id,
                        g_drone_lat, g_drone_lon,
                        g_drone_alt,
                        g_pilot_lat, g_pilot_lon
                    );
                    lastTx = millis(); // reset interval timer
                }
            } else {
                // Serial.print("JSON parse error: ");
                // Serial.print(err.c_str());
                // Serial.print(" | Input: ");
                // Serial.println(json_buf);
            }
        } else {
            json_buf[json_idx++] = c;
        }
    }

    // 2) Periodically transmit using last JSON data
    // only transmit if broadcasting is enabled and we have data
    if (!broadcastEnabled || !g_has_data) {
        delay(10);
        return;
    }
    if (millis() - lastTx >= TX_INTERVAL) {
        inject_vendor_ie(
            g_basic_id,
            g_drone_lat, g_drone_lon,
            g_drone_alt,
            g_pilot_lat, g_pilot_lon
        );
        lastTx = millis();
        // Serial.println("Beacon injected");
    }
}
