#pragma once

#include <Arduino.h>

// Packet v1 mirror used by the standalone relay sketch. Keep this definition
// byte-for-byte compatible with telem-v1/live_telemetry_packet.h.
static const uint32_t LIVE_TELEMETRY_MAGIC = 0x5554534DUL;  // "UTSM"
static const uint8_t LIVE_TELEMETRY_VERSION = 1;
static const uint8_t LIVE_TELEMETRY_FLAG_GPS_VALID = 0x01;

struct LiveTelemetryPacket
{
  uint32_t magic;
  uint8_t version;
  uint8_t flags;
  uint16_t packet_size;
  uint32_t boot_id;
  uint32_t sequence;
  uint32_t timestamp_ms;
  int16_t current_mA;
  int32_t voltage_mV;
  int16_t ax_x100;
  int16_t ay_x100;
  int16_t az_x100;
  uint16_t amag_x100;
  int32_t latitude_e7;
  int32_t longitude_e7;
} __attribute__((packed));

static_assert(sizeof(LiveTelemetryPacket) == 42, "Live telemetry packet layout changed");
static_assert(sizeof(LiveTelemetryPacket) <= 250, "ESP-NOW packet is too large");

inline bool isValidLiveTelemetryPacket(const LiveTelemetryPacket &packet)
{
  return packet.magic == LIVE_TELEMETRY_MAGIC &&
         packet.version == LIVE_TELEMETRY_VERSION &&
         packet.packet_size == sizeof(LiveTelemetryPacket);
}
