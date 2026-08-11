#pragma once

#include <Arduino.h>

static const uint32_t DYNO_TELEMETRY_MAGIC = 0x44594E4FUL;  // "DYNO"
static const uint8_t DYNO_TELEMETRY_VERSION = 1;

struct DynoTelemetryPacket
{
  uint32_t magic;
  uint8_t version;
  uint8_t state;
  uint16_t packet_size;
  uint32_t boot_id;
  uint32_t sequence;
  uint32_t timestamp_ms;
  int32_t voltage_mV;
  int32_t current_mA;
  int32_t power_mW;
  uint64_t energy_mJ;
} __attribute__((packed));

static_assert(sizeof(DynoTelemetryPacket) == 40, "Dyno telemetry packet layout changed");
static_assert(sizeof(DynoTelemetryPacket) <= 250, "ESP-NOW packet is too large");

inline bool isValidDynoTelemetryPacket(const DynoTelemetryPacket &packet)
{
  return packet.magic == DYNO_TELEMETRY_MAGIC &&
         packet.version == DYNO_TELEMETRY_VERSION &&
         packet.packet_size == sizeof(DynoTelemetryPacket);
}
