# T-SIM7600G LTE relay

This sketch receives the existing telemetry record over ESP-NOW and posts it
to the software dashboard over LTE. SD logging on the ESP32-C3 remains the
complete source of truth; this relay intentionally has no persistent buffer.

## Hardware

- LILYGO/TTGO T-SIM7600G ESP32-WROVER board
- LTE main antenna attached before powering the modem
- Activated nano-SIM with data service
- Stable USB/5 V supply capable of modem current peaks (use at least 2 A)

The ESP-NOW hop is wireless. Do not connect UART wires between the logger and
relay. Power both boards normally and keep them within radio range.

## Arduino setup

Install:

- Espressif `esp32` board package
- `TinyGSM` by Volodymyr Shymanskyy

Select an ESP32 WROVER-compatible board and enable PSRAM if that board profile
offers the option.

Copy `relay_config.example.h` to `relay_config.h`, then set the carrier APN,
the full dashboard ingestion URL, and the same API key used by the server.

For a first SIM test, disable the SIM PIN in a phone, confirm mobile data works,
then move the powered-off SIM into the T-SIM7600G. A normal phone SIM usually
works when the carrier permits modem/tethered data and the APN is correct.

## Prototype behavior

- Valid records are posted immediately.
- Records received while LTE is unavailable are dropped from the live stream.
- The original ESP32-C3 SD CSV is unaffected.
- The logger broadcasts at 1 Hz to avoid overwhelming LTE with the full sensor
  sample rate. Change `LIVE_TELEMETRY_MIN_SEND_INTERVAL_MS` if required.
- GPS fields are already part of packet version 1. The map activates when a
  future logger integration passes valid `latitude_e7` and `longitude_e7`.
