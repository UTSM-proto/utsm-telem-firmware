# T-A7670X LTE relay

This sketch receives the existing telemetry record over ESP-NOW and posts it
to the software dashboard over LTE. SD logging on the ESP32-C3 remains the
complete source of truth; this relay intentionally has no persistent buffer.

## Hardware

- LILYGO T-A7670E/G/SA ESP32-WROVER-E board
- LTE main antenna attached before powering the modem
- Activated nano-SIM with data service
- Stable USB/5 V supply capable of modem current peaks (use at least 2 A)

The ESP-NOW hop is wireless. Do not connect UART wires between the logger and
relay. Power both boards normally and keep them within radio range.

## Modem communication smoke test

Before installing LTE libraries or inserting a SIM, flash
`a7670_at_test/a7670_at_test.ino`. Open Serial Monitor at 115200 baud. The
automatic test should return `OK` for `AT` and print the exact modem identity
for `AT+SIMCOMATI`. The monitor then remains as a direct AT-command terminal.

## Arduino setup

Install:

- Espressif `esp32` board package
- LilyGO's bundled `TinyGSM` fork from the `lib` folder in
  [LilyGo-Modem-Series](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series)

Select an ESP32 WROVER-compatible board and enable PSRAM if that board profile
offers the option.

Copy `relay_config.example.h` to `relay_config.h`, then set the carrier APN,
the full dashboard ingestion URL, and the same API key used by the server.

For a first SIM test, disable the SIM PIN in a phone, confirm mobile data works,
then move the powered-off SIM into the T-A7670X. A normal phone SIM usually
works when the carrier permits modem/tethered data and the APN is correct.

## Prototype behavior

- Valid records are posted immediately.
- Records received while LTE is unavailable are dropped from the live stream.
- The original ESP32-C3 SD CSV is unaffected.
- The logger broadcasts at 1 Hz to avoid overwhelming LTE with the full sensor
  sample rate. Change `LIVE_TELEMETRY_MIN_SEND_INTERVAL_MS` if required.
- GPS fields are already part of packet version 1. The map activates when a
  future logger integration passes valid `latitude_e7` and `longitude_e7`.

## Level 2: LTE-only dummy test

This test uses only the T-A7670X and verifies SIM registration, LTE data,
HTTPS ingestion, WebSocket updates, gauges, charts, table, and map. The main
ESP32-C3 telemetry board is not required.

1. Copy `relay_config.example.h` to `relay_config.h`.
2. Set the carrier APN, public dashboard endpoint, and matching API key.
3. Set `LTE_DUMMY_TEST_MODE = true`.
4. Flash `lte_relay.ino` and open the serial monitor at 115200 baud.
5. Expect `Mode: LEVEL 2 LTE DUMMY TEST`, an assigned LTE IP, HTTP status 202,
   and repeating `DUMMY seq=N delivered` messages.
6. Return `LTE_DUMMY_TEST_MODE` to `false` before the ESP-NOW integration test.

## Level 3: full-path ESP-NOW dummy test

This proves `ESP32-C3 telemetry board -> ESP-NOW -> A7670X relay -> LTE -> dashboard`
without requiring live sensors.

1. Set `LTE_DUMMY_TEST_MODE = false` in the relay's ignored `relay_config.h`.
2. Flash `lte_relay/lte_relay.ino` to the T-A7670X and leave it powered.
3. Flash `telem-v1/espnow_dummy_sender/espnow_dummy_sender.ino` to the ESP32-C3
   SuperMini.
4. Open both serial monitors at 115200 if two USB ports are available.
5. The C3 prints `C3 ESP-NOW queued seq=N`; the relay prints
   `LIVE seq=N delivered`; the dashboard updates with the same sequence.

Both boards are explicitly pinned to ESP-NOW channel 1 for this demo. The
canonical telemetry logger uses the same sender channel and packet layout, so
returning from dummy data to real data only requires flashing
`telem-v1/telemetry_gpio1_led_sd_per_session.ino` back onto the C3.
