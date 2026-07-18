#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <math.h>

#include "live_telemetry_packet.h"

static const uint8_t ESPNOW_CHANNEL = 1;
static const uint32_t SEND_INTERVAL_MS = 2000;
static const uint8_t BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

uint32_t bootId = 0;
uint32_t sequence = 0;
uint32_t lastSendMs = 0;

LiveTelemetryPacket makeDummyPacket(uint32_t now)
{
  float phase = static_cast<float>(sequence % 40) / 40.0f * 2.0f * PI;
  LiveTelemetryPacket packet = {};
  packet.magic = LIVE_TELEMETRY_MAGIC;
  packet.version = LIVE_TELEMETRY_VERSION;
  packet.flags = LIVE_TELEMETRY_FLAG_GPS_VALID;
  packet.packet_size = sizeof(packet);
  packet.boot_id = bootId;
  packet.sequence = sequence++;
  packet.timestamp_ms = now;

  // Obvious moving demo values generated on the ESP32-C3 telemetry board.
  packet.current_mA = static_cast<int16_t>(11000.0f + 3000.0f * sinf(phase));
  packet.voltage_mV = 50000 - packet.current_mA / 25;
  packet.ax_x100 = static_cast<int16_t>(90.0f * sinf(phase * 1.7f));
  packet.ay_x100 = static_cast<int16_t>(65.0f * cosf(phase * 1.3f));
  packet.az_x100 = 981;
  packet.amag_x100 = static_cast<uint16_t>(988.0f + 24.0f * sinf(phase));

  // Small fake lap around the existing dashboard demo location.
  packet.latitude_e7 = 397991700 + static_cast<int32_t>(4500.0f * sinf(phase));
  packet.longitude_e7 = -862380100 + static_cast<int32_t>(6000.0f * cosf(phase));
  return packet;
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("UTSM ESP32-C3 -> ESP-NOW -> LTE full-path dummy test");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_err_t channelResult =
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (channelResult != ESP_OK) {
    Serial.printf("Failed to set ESP-NOW channel: %d\n", channelResult);
  }

  Serial.print("C3 sender MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("C3 ESP-NOW channel: %u\n", ESPNOW_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, sizeof(peer.peer_addr));
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_err_t peerResult = esp_now_add_peer(&peer);
  if (peerResult != ESP_OK && peerResult != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("ESP-NOW broadcast peer failed: %d\n", peerResult);
    while (true) delay(1000);
  }

  bootId = esp_random();
  Serial.println("C3 dummy sender ready; transmitting every 2 seconds");
}

void loop()
{
  uint32_t now = millis();
  if (now - lastSendMs < SEND_INTERVAL_MS) {
    delay(5);
    return;
  }
  lastSendMs = now;

  LiveTelemetryPacket packet = makeDummyPacket(now);
  esp_err_t result = esp_now_send(
    BROADCAST_MAC,
    reinterpret_cast<const uint8_t *>(&packet),
    sizeof(packet));

  if (result == ESP_OK) {
    Serial.printf("C3 ESP-NOW queued seq=%lu I=%d mA V=%ld mV\n",
                  static_cast<unsigned long>(packet.sequence),
                  packet.current_mA,
                  static_cast<long>(packet.voltage_mV));
  } else {
    Serial.printf("C3 ESP-NOW send failed seq=%lu error=%d\n",
                  static_cast<unsigned long>(packet.sequence),
                  result);
  }
}
