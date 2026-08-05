# T-A7670X LTE relay

This sketch receives the existing telemetry record over ESP-NOW and posts it
to the software dashboard over LTE. SD logging on the ESP32-C3 remains the
complete source of truth; this relay intentionally has no persistent buffer.

## Hardware

- LILYGO T-A7670E/G/SA ESP32-WROVER-E board
- ESP32-C3 SuperMini on the TelemV2 telemetry board
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

> **Hardware status:** The throughput changes described below compile, but have
> not yet been tested on the TelemV2 C3, WROVER/A7670, SD card, or live carrier.

- Valid records are posted immediately.
- Records received while LTE is unavailable are dropped from the live stream.
- The original ESP32-C3 SD CSV is unaffected.
- The logger broadcasts at most once every 500 ms. If LTE is slower than the
  incoming stream, the relay keeps only the newest pending packet so the live
  page catches up instead of displaying an old FIFO backlog.
- The WROVER prints relay queue wait, LTE POST duration, and JSON payload size.
  Use those values to measure freshness and the carrier/modem's real sustainable
  rate, and watch data use against the vehicle SIM's 500 MB allowance; HTTP/TLS
  overhead is not included in the printed JSON byte count.
- After a one-time stale-service cleanup, the relay avoids issuing a redundant
  `HTTPTERM` immediately before every `HTTPINIT`. A failed initialization still
  performs cleanup and retries once.
- SD still records at the full sensor sample rate. The TelemV2 logger uses the
  ADS1115's 860 SPS conversion mode while retaining 20-sample averaging and a
  separate open/write/close for every SD row.
- TelemV2 sends valid GPS latitude/longitude with the live packet. The map
  activates automatically after the GPS obtains a fix.

## Throughput analysis and test targets

Two prior vehicle CSVs had median SD row intervals of 608 ms and 609 ms, about
1.64 rows/second. The critical path was the 20-sample current/voltage average:
40 ADS1115 conversions waited 10 ms each, plus 20 inter-sample delays of 3 ms,
for about 460 ms before MPU sampling and file writes. Current `main` already
uses 860 SPS conversions with a 2 ms wait and no redundant inter-sample delay,
reducing that planned wait to about 80 ms while retaining all 20 samples.

The first hardware target is at least 3 SD rows/second with stable current and
voltage values. The theoretical estimate is about 4 rows/second, based on
removing roughly 380 ms from the old 610 ms loop, but SD latency and sensor
noise must be measured rather than assumed.

The live target is the freshest packet the serialized modem can sustain, up to
2 ESP-NOW packets/second. The server rate cannot exceed `1000 / POST_ms`; for
example, a 1400 ms POST caps delivery near 0.7 rows/second. A low `queue=... ms`
with skipped sequence numbers is expected: it proves old pending samples were
replaced and the next POST used recent data.

At two 240-byte JSON bodies per second, JSON bodies alone total about 1.73
MB/hour. Real carrier usage is higher because this excludes IP, TCP, TLS, and
HTTP overhead, so confirm usage through the carrier account during testing.

## Level 2: LTE-only dummy test

This test uses only the T-A7670X and verifies SIM registration, LTE data,
HTTPS ingestion, WebSocket updates, gauges, charts, table, and map. The main
ESP32-C3 telemetry board is not required.

1. Copy `relay_config.example.h` to `relay_config.h`.
2. Set the carrier APN, public dashboard endpoint, and matching API key.
3. Set `LTE_DUMMY_TEST_MODE = true`.
4. Flash `lte_relay.ino` and open the serial monitor at 115200 baud.
5. Expect `Mode: LEVEL 2 LTE DUMMY TEST`, an assigned LTE IP, HTTP status 202,
   and repeating `DUMMY seq=N delivered in ... ms queue=... ms json=... B`
   messages.
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
   `LIVE seq=N delivered in ... ms queue=... ms json=... B`; the dashboard
   updates with the same sequence.

Expected relay output:

```text
Mode: LIVE ESP-NOW RELAY
Relay ESP-NOW channel: 1
LTE connected; IP: ...
Dashboard POST status=202
LIVE seq=0 delivered in 1430 ms queue=4 ms json=241 B
```

Expected C3 output:

```text
C3 ESP-NOW channel: 1
C3 dummy sender ready; transmitting every 2 seconds
C3 ESP-NOW queued seq=0 I=11000 mA V=49560 mV
```

Both boards are explicitly pinned to ESP-NOW channel 1 for this demo.

## Level 4: TelemV2 live vehicle demo

This is the real end-to-end path:
`TelemV2 -> ESP-NOW -> WROVER/A7670 -> LTE -> HTTPS -> live dashboard`.

1. Start the dashboard with `UTSM_TELEMETRY_API_KEY` set and open `/live`.
2. Start a Cloudflare quick tunnel and copy its current HTTPS hostname.
3. In the ignored `relay_config.h`, set `TELEMETRY_ENDPOINT` to that hostname
   followed by `/api/live/telemetry`, use the same API key as the server, and
   ensure `LTE_DUMMY_TEST_MODE = false`.
4. Flash `lte_relay/lte_relay.ino` to the WROVER/A7670 board.
5. Flash `telem-v2/telemetry_v2/telemetry_v2.ino` to the ESP32-C3 logger.
6. Attach the LTE and GPS antennas, insert the SIM, power both boards, and
   leave the laptop running the dashboard and tunnel.
7. Wait through TelemV2 initialization. Its LED becomes solid when SD logging
   starts. Outdoors, wait for the live page map to receive a GPS position.
8. The page targets up to two new rows per second. Actual rate is capped by
   the measured LTE POST duration printed by the relay. Move the car outdoors
   and the GPS marker/trail should move with the vehicle.

The relay's serial success line is
`LIVE seq=N delivered in ... ms queue=... ms json=... B`, but serial access is
not required in the vehicle: increasing sequence numbers on `/live` prove the
complete path. If rows update without a map marker, LTE is working but the GPS
does not yet have a valid fix.

When only the WROVER Serial Monitor is accessible, each real packet also shows:

```text
GPS seq=12 rx=GPIO20 baud=9600 bytes=1842 nmea=yes sats=7 utc=yes fix=yes
```

- `bytes=0`: the C3 has received no electrical UART data at the displayed pin/rate.
- Increasing `bytes` with `nmea=no`: bytes exist, but no checksum-valid NMEA sentence has been decoded yet.
- `rx=GPIO21` or `rx=GPIO20` and `baud=...`: the candidate currently being tested or locked.
- `nmea=yes sats=0`: the module is communicating but has not acquired satellites.
- `sats>0 fix=no`: keep the antenna stationary with a clear view of the sky.
- `fix=yes`: the next LTE post includes latitude/longitude for the live map.

If the C3 prints `queued` but the relay prints nothing, confirm both serial
monitors report ESP-NOW channel 1. If the relay reports HTTP 401, the dashboard
and relay API keys differ. HTTP 404 usually means the endpoint is missing
`/api/live/telemetry`.
