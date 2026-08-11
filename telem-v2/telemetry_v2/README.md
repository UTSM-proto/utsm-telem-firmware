# TelemV2 firmware

`telemetry_v2.ino` is the primary ESP32-C3 vehicle telemetry firmware. It keeps
the TelemV1 current, voltage, acceleration, SD-card, start/stop button, and
status LED connections, then adds temperature, GPS, and wheel speed.

## Beam-break speedometer

The TelemV2 PCB routes the speedometer connector's digital output to GPIO3.
The sensor output is LOW while a spoke blocks the beam and HIGH while the beam
is clear. The firmware counts one debounced blocked-to-clear cycle per spoke.

- Wheel diameter: 20 inches (`0.508 m`)
- Spokes/pulses per revolution: 12
- Wheel circumference: `pi * 0.508 m`, approximately `1.596 m`
- State debounce: both blocked and clear must remain unchanged for `2 ms`
- Physical-speed guard: accepted cycles less than `8 ms` apart are ignored;
  this supports approximately `60 km/h`, above the vehicle's `40 km/h` maximum
- Stopped-wheel timeout: `2 s` without an accepted edge

For an average interval `dt_us` between accepted spoke edges:

```text
wheel_rpm = 60,000,000 / (12 * dt_us)
wheel_speed_km_h = (pi * 0.508) * (wheel_rpm / 60) * 3.6
```

The CSV contains `wheel_speed_valid`, `wheel_speed_kmph`, `wheel_rpm`, and a
cumulative `wheel_spoke_count`. GPS speed remains a separate field so the two
measurements can be compared during testing.

## Live LTE tracking

While SD logging is active, TelemV2 broadcasts a best-effort update every five
seconds over ESP-NOW channel 1. The WROVER/A7670 relay in `lte_relay/` receives
each packet and immediately posts it over LTE to the existing live dashboard.
The reduced rate conserves the vehicle SIM's 500 MB data allowance. No UART
wires are required between the boards.

The live packet contains current, voltage, acceleration, beam-break wheel
speed, and valid GPS latitude/longitude. Wheel speed includes an explicit
validity flag, so the dashboard can distinguish a valid stopped-wheel `0.00`
from a sensor that has not produced a usable interval yet. The dashboard map
begins tracking as soon as the GPS has a fix. SD remains the complete source
of truth: ESP-NOW, relay, LTE, or server failure never stops or delays the CSV
logger, and missed live records are not backfilled.

The vehicle wiring connects the GPS module's TX pin to C3 GPIO20 and its RX pin
to C3 GPIO21. The logger only needs to receive GPS data, so it leaves UART TX
disconnected and first listens on GPIO20 at the normal NEO-M8N rate of 9600
baud. If it does not receive a checksum-valid NMEA sentence, it continuously
tests GPIO21 and GPIO20 at 9600, 38400, 115200, and 4800 baud until it locks.
The C3 serial monitor prints each attempted pin/rate and its raw byte count.
On ESP32-C3, hardware UART0 also defaults to GPIO20/GPIO21. The firmware does
not initialize that console unless **USB CDC On Boot** is enabled, preventing
`Serial.begin()` and GPS UART1 from competing for the same physical pins. GPS
probe results are available from the WROVER relay even without a C3 monitor.

`raw bytes=0` for every attempt means this is an electrical path problem, not
a satellite-fix problem: verify 3.3 V and ground at the GPS header, then check
continuity from the module's TX pin to C3 GPIO20. Raw bytes without an NMEA
lock indicate a non-NMEA/proprietary configuration or corrupted UART signal.

For the full setup and demo sequence, see `../../lte_relay/README.md`.

## Indoor operation without GPS

At startup the logger waits up to 30 seconds for valid GPS UTC time. If GPS is
available, it creates the usual timestamped file such as
`/2026-08-01-15-30.csv`. If GPS is unavailable indoors, logging still starts
using the first free TelemV1-style filename from `/telemetry_001.csv` through
`/telemetry_999.csv`. GPS continues running in the background and its fields
begin populating if a fix becomes available later in the same session.

## Status LED and fault codes

Normal startup flashes rapidly for about one second, then flashes during the
18-second current-sensor warmup/calibration and the optional 30-second GPS
wait. A solid LED means the logger is recording. An unlit LED after successful
startup means logging was manually stopped with the button or serial command.

A fatal fault repeats a numbered group of 200 ms flashes followed by a
1.5-second pause. Count the flashes in one group:

| Flashes | Fault | First checks |
| ---: | --- | --- |
| 2 | SD card initialization/mount | Card inserted, FAT32, CS 7, MOSI 6, MISO 5, SCK 4 |
| 3 | LittleFS mount | Reflash with the correct ESP32-C3 flash/partition settings |
| 4 | ADS1115 initialization | Address `0x48`, SDA 8, SCL 9, 3.3 V and ground |
| 5 | MPU6050 initialization | Address `0x68`, SDA 8, SCL 9, 3.3 V and ground |
| 6 | Current offset calibration | ADS1115/current-sensor wiring and resting current |
| 7 | SD log-file creation or write | FAT32/free space/card contacts; inspect or replace card |
| 8 | Speed-sensor task startup | Reset or reflash the ESP32-C3; report if the fault repeats |

The fault pattern continues until power is removed or the board is reset.

## Hardware-test checklist

1. Confirm the LM393 digital output presented to ESP32-C3 GPIO3 never exceeds
   3.3 V. The PCB connector supplies 5 V, and a module whose output is pulled
   up to its supply needs level shifting or a 3.3 V pull-up before GPIO3.
2. With the wheel stopped, confirm the serial log reports `0.00 km/h` after the
   two-second timeout.
3. Turn the wheel exactly once and confirm `wheel_spoke_count` increases by 12.
4. Compare `wheel_speed_kmph` with `gps_speed_kmph` during a low-speed test.
5. Inspect the SD CSV for stable RPM/speed and false pulses before merging.

## Arduino dependencies

- ESP32 Arduino core with ESP32-C3 support
- TinyGPSPlus
- Built-in ESP32 `Wire`, `LittleFS`, `FS`, `SD`, and `SPI` libraries
