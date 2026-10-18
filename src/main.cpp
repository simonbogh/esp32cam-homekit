// AI-Thinker ESP32-CAM -> Apple Home, native HomeKit (HomeSpan), no bridge.
// HomeKit: camera tile with JPEG snapshots + motion sensor. Live view in a browser: MJPEG on :82/stream.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HomeSpan.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "camera.h"
#include "config.h"
#include "h264_pipe.h"
#include "hk_camera.h"
#include "live_stream.h"
#include "motion.h"
// Optional: Wi-Fi + HomeKit setup code baked in from include/secrets.h. Without it the firmware uses
// HomeSpan's setup instead (setup access point, or serial 'W' / 'S').
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include "web.h"

static bool cameraOk = false;

static void wifiBegin(const char *ssid, const char *pwd) {
  WiFi.setSleep(false);  // modem sleep adds latency and lowers throughput
  WiFi.begin(ssid, pwd);
  WiFi.setTxPower(WIFI_TX_POWER);
}

static void onConnected(int count) {
  esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("[wifi] connected #%d: IP %s, RSSI %d dBm, channel %d\n", count, WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), WiFi.channel());
  if (count == 1 && cameraOk) {
    Web::begin();
    MDNS.addService("http", "tcp", WEB_PORT);
  }
}

static void printStats() {
  char json[640];
  Web::statsJson(json, sizeof(json));
  Serial.printf("[stats] %s\n", json);
}

static void cmdXclk(const char *buf) {
  const int mhz = atoi(buf + 1);
  if (Camera::setXclk(mhz))
    Serial.printf("[cam] XCLK -> %d MHz\n", mhz);
  else
    Serial.printf("[cam] XCLK change to '%s' failed (use @x8 or @x20)\n", buf + 1);
}

static void cmdStats(const char *) { printStats(); }

static void cmdBench(const char *) {
  if (LiveStream::streaming())
    Serial.println("[bench] stop the live stream first (the benchmark shares the JPEG decoder)");
  else
    H264::runBenchmark();
}

static void cmdProfile(const char *buf) {
  if (!LiveStream::setProfile(atoi(buf + 1))) Serial.println("[live] use @p0..@p4 (160x120, 240x176, 320x240, 480x320, 640x480)");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n[boot] ESP32 Cam HomeKit firmware %s, PSRAM %u bytes\n", FW_VERSION, (unsigned)ESP.getPsramSize());

  cameraOk = Camera::begin();
  if (cameraOk) Motion::begin();

#ifdef WIFI_SSID
  homeSpan.setWifiCredentials(WIFI_SSID, WIFI_PASS);
#endif
#ifdef HK_PAIRING_CODE
  homeSpan.setPairingCode(HK_PAIRING_CODE);
#endif
  // Without stored Wi-Fi credentials HomeSpan opens the "HomeSpan-Setup" access point (password "homespan")
  // where Wi-Fi and the HomeKit setup code are entered; both are then kept in NVS.
  homeSpan.enableAutoStartAP()
      .setHostNameSuffix("")
      .setWifiBegin(wifiBegin)
      .setConnectionCallback(onConnected)
      .setSketchVersion(FW_VERSION);
  homeSpan.begin(Category::IPCameras, HK_DISPLAY_NAME, HOST_NAME_BASE, "ESP32-CAM");

  HomeKitCamera::begin();

  new SpanUserCommand('x', "<MHz> - set camera XCLK, e.g. @x8 or @x20", cmdXclk);
  new SpanUserCommand('s', "- print camera/Wi-Fi stats now", cmdStats);
  new SpanUserCommand('b', "- benchmark JPEG -> H.264 encoding", cmdBench);
  new SpanUserCommand('p', "<n> - HomeKit live profile: 0=160x120 1=240x176 2=320x240 3=480x320 4=640x480", cmdProfile);

#ifdef HK_PAIRING_CODE
  Serial.printf("[boot] HomeKit setup code: %.4s-%.4s\n", HK_PAIRING_CODE, HK_PAIRING_CODE + 4);
#else
  Serial.println("[boot] HomeKit setup code: the one chosen in the setup access point or with 'S <8 digits>'");
#endif
}

void loop() {
  homeSpan.poll();

  static uint32_t lastStats = 0;
  if (millis() - lastStats >= STATS_INTERVAL_MS) {
    lastStats = millis();
    printStats();
  }
}
