#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>

#include "live_telemetry_packet.h"

// Broadcast avoids a MAC-pairing step between the logger and vehicle relay.
// The LTE API key still protects the internet-facing ingestion endpoint.
static const uint8_t LIVE_TELEMETRY_BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

// SD retains every sample. LTE receives a best-effort update every second for
// a responsive dashboard. There is no persistent buffering, so a relay/network
// outage cannot block the logger or replay stale records later.
static const uint32_t LIVE_TELEMETRY_MIN_SEND_INTERVAL_MS = 1000;
static const uint8_t LIVE_TELEMETRY_ESPNOW_CHANNEL = 1;

class LiveTelemetryEspNowSender
{
public:
  bool begin()
  {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(LIVE_TELEMETRY_ESPNOW_CHANNEL,
                         WIFI_SECOND_CHAN_NONE);
    Serial.print("ESP-NOW sender MAC: ");
    Serial.println(WiFi.macAddress());

    if (esp_now_init() != ESP_OK) {
      Serial.println("ESP-NOW init failed; SD logging will continue");
      return false;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, LIVE_TELEMETRY_BROADCAST_MAC,
           sizeof(peer.peer_addr));
    peer.channel = LIVE_TELEMETRY_ESPNOW_CHANNEL;
    peer.encrypt = false;

    esp_err_t result = esp_now_add_peer(&peer);
    if (result != ESP_OK && result != ESP_ERR_ESPNOW_EXIST) {
      Serial.printf(
        "ESP-NOW peer setup failed: %d; SD logging will continue\n",
        result
      );
      return false;
    }

    _bootId = esp_random();
    _ready = true;
    Serial.printf(
      "ESP-NOW live telemetry ready (broadcast, channel %u, every %lu ms)\n",
      LIVE_TELEMETRY_ESPNOW_CHANNEL,
      (unsigned long)LIVE_TELEMETRY_MIN_SEND_INTERVAL_MS
    );
    return true;
  }

  bool send(
    uint32_t timestampMs,
    int16_t currentMa,
    int32_t voltageMv,
    int16_t axX100,
    int16_t ayX100,
    int16_t azX100,
    uint16_t amagX100,
    bool wheelSpeedValid,
    uint16_t wheelSpeedKmphX100,
    bool gpsValid,
    bool gpsUartActive,
    bool gpsTimeValid,
    bool gpsRxOnGpio21,
    uint8_t gpsSatellites,
    int32_t latitudeE7,
    int32_t longitudeE7,
    uint32_t gpsUartBaud,
    uint32_t gpsUartBytes)
  {
    if (!_ready) return false;
    if (_hasSent &&
        timestampMs - _lastSendMs < LIVE_TELEMETRY_MIN_SEND_INTERVAL_MS) {
      return true;
    }

    LiveTelemetryPacket packet = {};
    packet.magic = LIVE_TELEMETRY_MAGIC;
    packet.version = LIVE_TELEMETRY_VERSION;
    packet.flags = 0;
    if (gpsValid) packet.flags |= LIVE_TELEMETRY_FLAG_GPS_VALID;
    if (gpsUartActive) packet.flags |= LIVE_TELEMETRY_FLAG_GPS_UART_ACTIVE;
    if (gpsTimeValid) packet.flags |= LIVE_TELEMETRY_FLAG_GPS_TIME_VALID;
    if (gpsRxOnGpio21) packet.flags |= LIVE_TELEMETRY_FLAG_GPS_RX_GPIO21;
    uint8_t encodedSatellites = gpsSatellites > 15 ? 15 : gpsSatellites;
    packet.flags |= encodedSatellites << LIVE_TELEMETRY_GPS_SATS_SHIFT;
    packet.packet_size = sizeof(packet);
    packet.boot_id = _bootId;
    packet.sequence = _sequence++;
    packet.timestamp_ms = timestampMs;
    packet.current_mA = currentMa;
    packet.voltage_mV = voltageMv;
    packet.ax_x100 = axX100;
    packet.ay_x100 = ayX100;
    packet.az_x100 = azX100;
    packet.amag_x100 = amagX100;
    packet.wheel_speed_valid = wheelSpeedValid ? 1 : 0;
    packet.wheel_speed_kmph_x100 = wheelSpeedValid
                                       ? wheelSpeedKmphX100
                                       : 0;
    packet.latitude_e7 = latitudeE7;
    packet.longitude_e7 = longitudeE7;
    packet.gps_uart_baud = gpsUartBaud;
    packet.gps_uart_bytes = gpsUartBytes;

    esp_err_t result = esp_now_send(
      LIVE_TELEMETRY_BROADCAST_MAC,
      reinterpret_cast<const uint8_t *>(&packet),
      sizeof(packet)
    );

    if (result != ESP_OK) {
      Serial.printf("ESP-NOW queue failed: %d\n", result);
      return false;
    }

    _lastSendMs = timestampMs;
    _hasSent = true;
    return true;
  }

private:
  bool _ready = false;
  bool _hasSent = false;
  uint32_t _bootId = 0;
  uint32_t _sequence = 0;
  uint32_t _lastSendMs = 0;
};
