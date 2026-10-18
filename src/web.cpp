#include "web.h"

#include <WiFi.h>
#include <esp_http_server.h>

#include <atomic>

#include "camera.h"
#include "config.h"
#include "live_stream.h"
#include "motion.h"

namespace Web {

static httpd_handle_t webServer = nullptr;
static httpd_handle_t streamServer = nullptr;
static volatile float lastStreamFps = 0;
static std::atomic<int> clients{0};

static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 Cam</title>
<style>body{font-family:-apple-system,sans-serif;margin:0 auto;padding:16px;max-width:680px;background:#111;color:#eee}
img{width:100%%;border-radius:8px;background:#000}pre{background:#222;padding:8px;border-radius:6px;overflow:auto}
button{font-size:16px;margin:4px 8px 4px 0;padding:6px 14px}</style></head><body>
<h2>ESP32 Cam</h2><img id="v" alt="stream">
<p>XCLK: <button onclick="x(8)">8 MHz</button><button onclick="x(12)">12 MHz</button><button onclick="x(20)">20 MHz</button>
<a href="/jpg" style="color:#8cf">snapshot</a></p><pre id="s">loading...</pre>
<script>
document.getElementById('v').src=location.protocol+'//'+location.hostname+':%d/stream';
async function poll(){try{const r=await fetch('/stats');document.getElementById('s').textContent=JSON.stringify(await r.json(),null,1)}catch(e){}}
async function x(m){await fetch('/control?xclk='+m);poll()}
poll();setInterval(poll,2000);
</script></body></html>)HTML";

size_t statsJson(char *buf, size_t size) {
  FramePtr f = Camera::latest();
  char live[128];
  LiveStream::statsJson(live, sizeof(live));
  return snprintf(buf, size,
                  "{\"cap_fps\":%.1f,\"stream_fps\":%.1f,\"stream_clients\":%d,\"frame_bytes\":%u,"
                  "\"frame\":\"%ux%u\",\"cap_errors\":%u,\"motion\":%s,\"motion_pct\":%.1f,\"rssi\":%d,"
                  "\"heap\":%u,\"heap_min\":%u,\"psram_free\":%u,\"xclk_mhz\":%d,\"uptime_s\":%lu,\"live\":%s}",
                  Camera::captureFps(), streamFps(), streamClients(), f ? (unsigned)f->len : 0,
                  f ? f->width : 0, f ? f->height : 0, (unsigned)Camera::captureErrors(),
                  Motion::detected() ? "true" : "false", Motion::changedPct(), WiFi.RSSI(),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getFreePsram(),
                  Camera::xclkMhz(), (unsigned long)(millis() / 1000), live);
}

static esp_err_t indexHandler(httpd_req_t *req) {
  char html[sizeof(INDEX_HTML) + 8];
  snprintf(html, sizeof(html), INDEX_HTML, STREAM_PORT);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, html);
}

static esp_err_t jpgHandler(httpd_req_t *req) {
  FramePtr f = Camera::latest();
  if (!f) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame yet");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, (const char *)f->buf, f->len);
}

static esp_err_t statsHandler(httpd_req_t *req) {
  char json[640];
  statsJson(json, sizeof(json));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, json);
}

static esp_err_t controlHandler(httpd_req_t *req) {
  char query[64], val[8];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "xclk", val, sizeof(val)) == ESP_OK && Camera::setXclk(atoi(val))) {
      Serial.printf("[web] XCLK -> %d MHz\n", atoi(val));
      return httpd_resp_sendstr(req, "ok");
    }
    if (httpd_query_key_value(query, "profile", val, sizeof(val)) == ESP_OK && LiveStream::setProfile(atoi(val)))
      return httpd_resp_sendstr(req, "ok");
  }
  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use /control?xclk=<MHz> or /control?profile=<0..4>");
  return ESP_FAIL;
}

#define PART_BOUNDARY "frame"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";

static esp_err_t streamHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  clients++;
  Serial.printf("[web] stream client connected (%d)\n", clients.load());

  uint32_t lastSeq = 0, windowStart = millis(), windowFrames = 0;
  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    FramePtr f = Camera::waitNext(lastSeq, 2000);
    if (!f) continue;
    lastSeq = f->seq;

    char hdr[96];
    const int n = snprintf(hdr, sizeof(hdr), "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)f->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, hdr, n);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)f->buf, f->len);

    windowFrames++;
    const uint32_t now = millis();
    if (now - windowStart >= 1000) {
      lastStreamFps = windowFrames * 1000.0f / (now - windowStart);
      windowFrames = 0;
      windowStart = now;
    }
  }

  clients--;
  if (clients == 0) lastStreamFps = 0;
  Serial.printf("[web] stream client disconnected (%d)\n", clients.load());
  return res;
}

void begin() {
  if (webServer) return;  // connection callback fires again on Wi-Fi reconnect

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = WEB_PORT;
  cfg.ctrl_port = 32768;
  cfg.max_open_sockets = 3;
  cfg.lru_purge_enable = true;
  if (httpd_start(&webServer, &cfg) == ESP_OK) {
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr},
        {.uri = "/jpg", .method = HTTP_GET, .handler = jpgHandler, .user_ctx = nullptr},
        {.uri = "/stats", .method = HTTP_GET, .handler = statsHandler, .user_ctx = nullptr},
        {.uri = "/control", .method = HTTP_GET, .handler = controlHandler, .user_ctx = nullptr},
    };
    for (const auto &u : uris) httpd_register_uri_handler(webServer, &u);
  } else {
    Serial.println("[web] failed to start web server");
  }

  httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
  scfg.server_port = STREAM_PORT;
  scfg.ctrl_port = 32769;
  scfg.max_open_sockets = 2;
  scfg.lru_purge_enable = true;
  scfg.core_id = 0;
  if (httpd_start(&streamServer, &scfg) == ESP_OK) {
    const httpd_uri_t u = {.uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr};
    httpd_register_uri_handler(streamServer, &u);
  } else {
    Serial.println("[web] failed to start stream server");
  }

  Serial.printf("[web] http://%s.local:%d/  (MJPEG on :%d/stream)\n", HOST_NAME_BASE, WEB_PORT, STREAM_PORT);
}

float streamFps() { return lastStreamFps; }
int streamClients() { return clients; }

}  // namespace Web
