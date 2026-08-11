#pragma once

#include <Arduino.h>

static const uint32_t LIVE_TELEMETRY_MAGIC = 0x5554534DUL;
static const uint8_t LIVE_TELEMETRY_VERSION = 3;
static const uint8_t LIVE_TELEMETRY_FLAG_GPS_VALID = 0x01;
static const uint8_t LIVE_TELEMETRY_FLAG_GPS_UART_ACTIVE = 0x02;
static const uint8_t LIVE_TELEMETRY_FLAG_GPS_TIME_VALID = 0x04;
static const uint8_t LIVE_TELEMETRY_FLAG_GPS_RX_GPIO21 = 0x08;
static const uint8_t LIVE_TELEMETRY_GPS_SATS_SHIFT = 4;

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
  uint32_t gps_uart_baud;
  uint32_t gps_uart_bytes;
  uint16_t wheel_speed_kmph_x100;
  uint8_t wheel_speed_valid;
  uint8_t reserved;
} __attribute__((packed));

static_assert(sizeof(LiveTelemetryPacket) == 54,
              "Live telemetry packet layout changed");
