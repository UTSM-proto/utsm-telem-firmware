# T-A7670X LTE relay

This sketch receives the existing telemetry record over ESP-NOW and posts it
to the software dashboard over LTE. SD logging on the ESP32-C3 remains the
complete source of truth; this relay intentionally has no persistent buffer.

## Hardware

- LILYGO T-A7670E/G/SA ESP32-WROVER-E board
- ESP32-C3 SuperMini on the `telem-v1` telemetry board
- LTE main antenna attached before powering the modem
- Activated nano-SIM with data service
- Stable USB/5 V supply capable of modem current peaks (use at least 2 A)

The ESP-NOW hop is wireless. Do not connect UART wires between the logger and
relay. Power both boards normally and keep them within radio range.

### Verified bench configuration

The complete path was bench-tested with:

- LILYGO T-A7670G R2 with `A7670G-LLSE` modem
- A7670 firmware `A7670M7_B02V01_251111`
- Public Mobile SIM on the TELUS LTE network
- Public Mobile APN `sp.mb.com`, with blank username and password
- ESP32-C3 SuperMini transmitting ESP-NOW broadcast packets on channel 1
- FastAPI dashboard exposed temporarily through Cloudflare Tunnel

The modem registered on LTE, opened a packet-data session, received an IP,
and delivered matching C3 packet sequences to the dashboard.

### Physical setup

1. With power disconnected, attach the LTE antenna to the connector marked
   `SIM`/`LTE`; do not attach it to `GNSS` for the LTE test.
2. Disable the SIM PIN in a phone, power the board off, and insert the nano-SIM.
3. Power the A7670X through its USB-C port using a data-capable USB-A-to-USB-C
   cable and a source capable of 5 V/2 A peaks. This board can fail to power
   from USB-C-to-USB-C.
4. Power the C3 separately. No UART wiring is required between the boards.
5. Keep the two boards within a few metres for the bench test.

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

Tested A7670X Arduino IDE settings:

- Board: `ESP32 Dev Module`
- CPU frequency: `240 MHz (WiFi/BT)`
- Flash frequency: `80 MHz`
- Flash mode: `QIO`
- Flash size: `4 MB`
- Partition scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`
- PSRAM: enabled
- Serial Monitor: `115200` baud

For the C3 dummy sender, select `ESP32C3 Dev Module` and use a 115200-baud
Serial Monitor.

Copy `relay_config.example.h` to `relay_config.h`, then set the carrier APN,
the full dashboard ingestion URL, and the same API key used by the server.

Example Public Mobile configuration:

```cpp
static const char LTE_APN[] = "sp.mb.com";
static const char LTE_USER[] = "";
static const char LTE_PASSWORD[] = "";
static const char SIM_PIN[] = "";
static const char TELEMETRY_ENDPOINT[] =
  "https://YOUR-TUNNEL.trycloudflare.com/api/live/telemetry";
static const char TELEMETRY_API_KEY[] = "replace-with-demo-key";
static const char TELEMETRY_DEVICE_ID[] = "utsm-a7670g";
```

For a first SIM test, disable the SIM PIN in a phone, confirm mobile data works,
then move the powered-off SIM into the T-A7670X. A normal phone SIM usually
works when the carrier permits modem/tethered data and the APN is correct.

## Start the dashboard and tunnel

From the software repository in PowerShell:

```powershell
python -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
$env:UTSM_TELEMETRY_API_KEY = "replace-with-demo-key"
python -m uvicorn live_dashboard.app:app --host 0.0.0.0 --port 8000
```

Open `http://127.0.0.1:8000/live`. In a second PowerShell window:

```powershell
cloudflared tunnel --url http://localhost:8000
```

Put the generated HTTPS hostname plus `/api/live/telemetry` in
`TELEMETRY_ENDPOINT`. The API key must exactly match the environment variable.
Quick-tunnel hostnames change whenever the tunnel is restarted.

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

Expected relay output:

```text
Mode: LIVE ESP-NOW RELAY
Relay ESP-NOW channel: 1
LTE connected; IP: ...
Dashboard POST status=202
LIVE seq=0 delivered
```

Expected C3 output:

```text
C3 ESP-NOW channel: 1
C3 dummy sender ready; transmitting every 2 seconds
C3 ESP-NOW queued seq=0 I=11000 mA V=49560 mV
```

Both boards are explicitly pinned to ESP-NOW channel 1 for this demo. The
canonical telemetry logger uses the same sender channel and packet layout, so
returning from dummy data to real data only requires flashing
`telem-v1/telemetry_gpio1_led_sd_per_session.ino` back onto the C3.

If the C3 prints `queued` but the relay prints nothing, confirm both serial
monitors report ESP-NOW channel 1. If the relay reports HTTP 401, the dashboard
and relay API keys differ. HTTP 404 usually means the endpoint is missing
`/api/live/telemetry`.
