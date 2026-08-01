# TelemV2 firmware

`telemetry_v2.ino` is the primary ESP32-C3 vehicle telemetry firmware. It keeps
the TelemV1 current, voltage, acceleration, SD-card, start/stop button, and
status LED connections, then adds temperature, GPS, and wheel speed.

## Beam-break speedometer

The TelemV2 PCB routes the speedometer connector's digital output to GPIO3.
The firmware counts one rising edge whenever a wheel spoke breaks the beam.

- Wheel diameter: 20 inches (`0.508 m`)
- Spokes/pulses per revolution: 12
- Wheel circumference: `pi * 0.508 m`, approximately `1.596 m`
- Chatter rejection: edges less than `500 us` after the previous accepted edge
- Stopped-wheel timeout: `2 s` without an accepted edge

For an average interval `dt_us` between accepted spoke edges:

```text
wheel_rpm = 60,000,000 / (12 * dt_us)
wheel_speed_km_h = (pi * 0.508) * (wheel_rpm / 60) * 3.6
```

The CSV contains `wheel_speed_valid`, `wheel_speed_kmph`, `wheel_rpm`, and a
cumulative `wheel_spoke_count`. GPS speed remains a separate field so the two
measurements can be compared during testing.

## Indoor operation without GPS

At startup the logger waits up to 30 seconds for valid GPS UTC time. If GPS is
available, it creates the usual timestamped file such as
`/2026-08-01-15-30.csv`. If GPS is unavailable indoors, logging still starts
using the first free TelemV1-style filename from `/telemetry_001.csv` through
`/telemetry_999.csv`. GPS continues running in the background and its fields
begin populating if a fix becomes available later in the same session.

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
