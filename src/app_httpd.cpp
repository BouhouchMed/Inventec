#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_pins.h"

// Target frame interval in ms (33 ≈ 30 fps, 50 ≈ 20 fps)
#define STREAM_FRAME_MS 40

// ── MJPEG streaming boundary ───────────────────────────────────────────────
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART        =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ── Flash LED brightness (0-255) ────────────────────────────────────────────
static int flashBrightness = 0;

// ── MJPEG stream handler ────────────────────────────────────────────────────
static esp_err_t stream_handler(httpd_req_t* req) {
    camera_fb_t*  fb         = nullptr;
    esp_err_t     res        = ESP_OK;
    size_t        _jpg_buf_len;
    uint8_t*      _jpg_buf;
    char          part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[CAM] Frame capture failed");
            res = ESP_FAIL;
        } else {
            if (fb->format != PIXFORMAT_JPEG) {
                bool converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                esp_camera_fb_return(fb);
                fb = nullptr;
                if (!converted) {
                    Serial.println("[CAM] JPEG conversion failed");
                    res = ESP_FAIL;
                }
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf     = fb->buf;
            }
        }

        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf),
                                   _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY,
                                        strlen(_STREAM_BOUNDARY));
            if (res == ESP_OK)
                res = httpd_resp_send_chunk(req, part_buf, hlen);
            if (res == ESP_OK)
                res = httpd_resp_send_chunk(
                    req, (const char*)_jpg_buf, _jpg_buf_len);
        }

        if (fb) {
            esp_camera_fb_return(fb);
            fb = nullptr;
        } else if (_jpg_buf) {
            free(_jpg_buf);
            _jpg_buf = nullptr;
        }

        if (res != ESP_OK) break;

        // Yield to WiFi stack and cap frame rate to prevent buffer overflow
        vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_MS));
    }
    return res;
}

// ── Snapshot (single JPEG) handler ─────────────────────────────────────────
static esp_err_t capture_handler(httpd_req_t* req) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

// ── Flash LED control handler (/flash?val=0-255) ────────────────────────────
static esp_err_t flash_handler(httpd_req_t* req) {
    char buf[32];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1 && buf_len <= sizeof(buf)) {
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char val[8];
            if (httpd_query_key_value(buf, "val", val, sizeof(val)) == ESP_OK) {
                flashBrightness = constrain(atoi(val), 0, 255);
                analogWrite(FLASH_LED_PIN, flashBrightness);
            }
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// ── Root page handler ───────────────────────────────────────────────────────
static esp_err_t index_handler(httpd_req_t* req) {
    const char* html = R"rawhtml(
<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-CAM</title>
<style>
  body{background:#111;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:16px}
  h1{color:#4af}
  img{max-width:100%;border:2px solid #4af;border-radius:8px}
  .btn{background:#4af;border:none;color:#000;padding:8px 20px;
       margin:6px;border-radius:6px;cursor:pointer;font-size:14px}
  .btn:hover{background:#2df}
  input[type=range]{width:200px;vertical-align:middle}
</style>
</head>
<body>
<h1>ESP32-CAM — AI Thinker</h1>
<img id="stream" src="/stream" alt="Stream">
<br><br>
<button class="btn" onclick="snap()">التقاط صورة</button>
<br><br>
<label>إضاءة الفلاش:
  <input type="range" min="0" max="255" value="0"
         oninput="setFlash(this.value)">
</label>
<script>
  function snap(){window.open('/capture','_blank')}
  function setFlash(v){fetch('/flash?val='+v)}
</script>
</body>
</html>
)rawhtml";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, html);
}

// ── Start HTTP server ───────────────────────────────────────────────────────
httpd_handle_t startCameraServer() {
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = 80;
    config.max_uri_handlers = 8;
    config.stack_size      = 8192;   // larger stack for MJPEG chunked sends
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_uri_t index_uri   = { "/",        HTTP_GET, index_handler,   nullptr };
    httpd_uri_t stream_uri  = { "/stream",  HTTP_GET, stream_handler,  nullptr };
    httpd_uri_t capture_uri = { "/capture", HTTP_GET, capture_handler, nullptr };
    httpd_uri_t flash_uri   = { "/flash",   HTTP_GET, flash_handler,   nullptr };

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &stream_uri);
        httpd_register_uri_handler(server, &capture_uri);
        httpd_register_uri_handler(server, &flash_uri);
        Serial.println("[HTTP] Server started on port 80");
    }
    return server;
}
