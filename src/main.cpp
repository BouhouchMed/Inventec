#include "Arduino.h"
#include "WiFi.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "camera_pins.h"
#include "wifi_config.h"

// Forward declaration from app_httpd.cpp
httpd_handle_t startCameraServer();

// ── Camera configuration ────────────────────────────────────────────────────
static bool initCamera() {
    camera_config_t config;

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    // Lower XCLK to reduce current spikes and improve stability on weak supplies.
    config.xclk_freq_hz = 10000000;
    config.pixel_format = PIXFORMAT_JPEG;

    // Use PSRAM for double-buffering; keep init size at VGA for stable streaming
    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;   // 640×480 — best for streaming
        config.jpeg_quality = 12;              // 0-63, lower = better quality
        config.fb_count     = 2;               // double buffer → grab latest frame
        config.grab_mode    = CAMERA_GRAB_LATEST; // discard stale frames
        Serial.println("[CAM] PSRAM found — double-buffer mode");
    } else {
        config.frame_size   = FRAMESIZE_QVGA;  // 320×240 — without PSRAM
        config.jpeg_quality = 15;
        config.fb_count     = 1;
        config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
        Serial.println("[CAM] No PSRAM — single-buffer mode");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init failed: 0x%x\n", err);
        return false;
    }

    // Fine-tune sensor settings
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_wb_mode(s, 0);          // auto
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 0);
        s->set_gain_ctrl(s, 1);
        s->set_agc_gain(s, 0);
        s->set_gainceiling(s, (gainceiling_t)0);
        s->set_bpc(s, 0);
        s->set_wpc(s, 1);
        s->set_raw_gma(s, 1);
        s->set_lenc(s, 1);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);
        s->set_colorbar(s, 0);
        // Keep frame size aligned with PSRAM availability for stability
        if (psramFound()) {
            s->set_framesize(s, FRAMESIZE_VGA);   // 640x480
        } else {
            s->set_framesize(s, FRAMESIZE_QVGA);  // 320x240
        }
    }

    Serial.println("[CAM] Initialized OK");
    return true;
}

// ── WiFi connection ─────────────────────────────────────────────────────────
static bool connectWiFi() {
    Serial.println("[WiFi] Init start");

    // Start from clean state before STA init.
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(100);

    Serial.println("[WiFi] Set STA mode");
    WiFi.mode(WIFI_STA);
    delay(50);

#ifdef STATIC_IP
    IPAddress ip(LOCAL_IP);
    IPAddress gw(GATEWAY);
    IPAddress sn(SUBNET);
    Serial.println("[WiFi] Apply static IP");
    WiFi.config(ip, gw, sn);
#endif

    Serial.println("[WiFi] Begin connect");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);

    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Connection FAILED");
        return false;
    }

    Serial.printf("[WiFi] Connected — IP: %s\n",
                  WiFi.localIP().toString().c_str());
    return true;
}

// ── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(false);
    Serial.println("\n[BOOT] ESP32-CAM AI Thinker starting...");
    Serial.printf("[BOOT] Reset reason: %d\n", (int)esp_reset_reason());

    // Common workaround for ESP32-CAM boards powered from weak USB adapters.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.println("[BOOT] Brownout detector disabled");

    // Status LED — active LOW
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);   // LED ON during init

    // Flash LED — PWM
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);    // OFF

    if (!initCamera()) {
        Serial.println("[BOOT] Camera init failed — halting");
        while (true) { delay(1000); }
    }

    // Small guard delay after camera init helps power rails settle.
    delay(250);

    if (!connectWiFi()) {
        Serial.println("[BOOT] WiFi failed — halting");
        while (true) { delay(1000); }
    }

    startCameraServer();

    Serial.println("=========================================");
    Serial.printf("  Stream  : http://%s/stream\n",
                  WiFi.localIP().toString().c_str());
    Serial.printf("  Snapshot: http://%s/capture\n",
                  WiFi.localIP().toString().c_str());
    Serial.println("=========================================");

    digitalWrite(STATUS_LED_PIN, HIGH);  // LED OFF — ready
}

// ── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    // Everything is handled by the HTTP server tasks.
    // Add your custom logic here if needed.
    delay(10000);
}
