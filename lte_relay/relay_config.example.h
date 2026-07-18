#pragma once

// Copy this file to relay_config.h and fill in your carrier/server values.
// relay_config.h is ignored by Git so SIM and API credentials stay local.

static const char LTE_APN[] = "YOUR_CARRIER_APN";
static const char LTE_USER[] = "";
static const char LTE_PASSWORD[] = "";
static const char SIM_PIN[] = "";  // Prefer disabling the PIN for first tests.

// The LTE network cannot reach a laptop's localhost address. Run the server
// locally, expose it through a tunnel, and paste its full /api path here.
static const char TELEMETRY_ENDPOINT[] =
  "https://YOUR-PUBLIC-TUNNEL.example/api/live/telemetry";
static const char TELEMETRY_API_KEY[] = "change-me";
static const char TELEMETRY_DEVICE_ID[] = "utsm-car-1";

// Level 2 bench test: generate fake telemetry on the T-SIM7600G and send it
// directly over LTE, without the ESP32-C3 logger or ESP-NOW sender.
static const bool LTE_DUMMY_TEST_MODE = false;
static const uint32_t LTE_DUMMY_SEND_INTERVAL_MS = 2000;
static const bool LTE_DUMMY_INCLUDE_GPS = true;
