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
