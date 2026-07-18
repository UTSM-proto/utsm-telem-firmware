from pathlib import Path

/*
  ESP32-C3 Vehicle Telemetry Logger
  ---------------------------------
  Collects:
    - Current from ADS1115 AIN0
    - Voltage from ADS1115 AIN1
    - Thermistor temperature from ADS1115 AIN2
    - Acceleration from MPU6050
    - GPS position, speed, altitude, HDOP, satellites, and UTC time
  Writes each recording session to an SD-card CSV.

  CSV filenames are derived directly from GPS UTC time:
      /YYYY-MM-DD-HH-MM.csv
  If that name already exists:
      /YYYY-MM-DD-HH-MM-001.csv

  Pin mapping (ESP32-C3 Super Mini):
    I2C SDA       GPIO8
    I2C SCL       GPIO9
    Start/stop    GPIO0, active LOW
    Status LED    GPIO1
    SD SCK        GPIO4
    SD MISO       GPIO5
    SD MOSI       GPIO6
    SD CS         GPIO7
    GPS RX        GPIO20 (connect to GPS TX)
    GPS TX        GPIO21 (connect to GPS RX)
*/

#include <Wire.h>
#include <LittleFS.h>
#include <math.h>
#include <TinyGPS++.h>

#include "FS.h"
#include "SD.h"
#include "SPI.h"

// =========================
// Pin mapping
// =========================
static const int PIN_I2C_SDA = 8;
static const int PIN_I2C_SCL = 9;
static const int BUTTON_PIN  = 0;
static const int LED_PIN     = 1;

static const int SD_CS   = 7;
static const int SD_MOSI = 6;
static const int SD_MISO = 5;
static const int SD_SCK  = 4;

static const int GPS_RX_PIN = 20;
static const int GPS_TX_PIN = 21;
static const uint32_t GPS_BAUD = 9600;

// =========================
// Device addresses
// =========================
static const uint8_t ADS1115_ADDR = 0x48;
static const uint8_t MPU6050_ADDR = 0x68;

// =========================
// Sampling constants
// =========================
#define CURRENT_NUM_SAMPLES  20
#define CURRENT_SAMPLE_DELAY 3

#define MPU_NUM_SAMPLES      20
#define MPU_SAMPLE_DELAY     3

// GPS is serviced during all long waits so its UART buffer does not overflow.
static const uint32_t GPS_FILENAME_WAIT_MS = 30000;
static const uint32_t GPS_DIAGNOSTIC_INTERVAL_MS = 1000;

// =========================
// ADS1115 registers/config
// =========================
#define ADS1115_REG_CONVERSION      0x00
#define ADS1115_REG_CONFIG          0x01

#define ADS1115_CONFIG_OS_SINGLE    0x8000
#define ADS1115_CONFIG_MUX_AIN0_GND 0x4000
#define ADS1115_CONFIG_MUX_AIN1_GND 0x5000
#define ADS1115_CONFIG_MUX_AIN2_GND 0x6000
#define ADS1115_CONFIG_PGA_4V096    0x0200
#define ADS1115_CONFIG_MODE_SINGLE  0x0100
#define ADS1115_CONFIG_DR_128SPS    0x0080
#define ADS1115_CONFIG_COMP_DISABLE 0x0003

#define ADS1115_CONFIG_WORD_AIN0 (ADS1115_CONFIG_OS_SINGLE     | \
                                  ADS1115_CONFIG_MUX_AIN0_GND  | \
                                  ADS1115_CONFIG_PGA_4V096     | \
                                  ADS1115_CONFIG_MODE_SINGLE   | \
                                  ADS1115_CONFIG_DR_128SPS     | \
                                  ADS1115_CONFIG_COMP_DISABLE)

#define ADS1115_CONFIG_WORD_AIN1 (ADS1115_CONFIG_OS_SINGLE     | \
                                  ADS1115_CONFIG_MUX_AIN1_GND  | \
                                  ADS1115_CONFIG_PGA_4V096     | \
                                  ADS1115_CONFIG_MODE_SINGLE   | \
                                  ADS1115_CONFIG_DR_128SPS     | \
                                  ADS1115_CONFIG_COMP_DISABLE)

#define ADS1115_CONFIG_WORD_AIN2 (ADS1115_CONFIG_OS_SINGLE     | \
                                  ADS1115_CONFIG_MUX_AIN2_GND  | \
                                  ADS1115_CONFIG_PGA_4V096     | \
                                  ADS1115_CONFIG_MODE_SINGLE   | \
                                  ADS1115_CONFIG_DR_128SPS     | \
                                  ADS1115_CONFIG_COMP_DISABLE)

static const float ADS1115_LSB_VOLTS = 4.096f / 32768.0f;

// =========================
// Current/voltage calibration
// =========================
static const float ADC_DIVIDER_RATIO   = 1.0f;
static const float ADC_ZERO_CURRENT_V  = 2.55f;
static const float ADC_SENSITIVITY     = 0.066f;
static const float VOLTAGE_INPUT_SCALE = 5.93f;

static const uint32_t CURRENT_ZERO_CAL_DELAY_MS = 15000;
static const uint32_t CURRENT_ZERO_CAL_TIME_MS  = 3000;
float g_currentOffsetA = 0.0f;

// =========================
// Thermistor calibration
// =========================
// Divider:
//   3.28 V -- 33.5k resistor -- AIN2 -- thermistor -- GND
static const float THERMISTOR_SUPPLY_V          = 3.28f;
static const float THERMISTOR_SERIES_R_OHMS     = 33500.0f;
static const float THERMISTOR_NOMINAL_R_OHMS    = 95000.0f;
static const float THERMISTOR_NOMINAL_TEMP_C    = 23.5f;
static const float THERMISTOR_B_COEFFICIENT     = 3950.0f;

// Ground AIN2 and use serial command 'c' to measure this offset.
// Copy the reported value here if a permanent correction is required.
static const float THERMISTOR_ADC_OFFSET_V = 0.0f;
float g_thermistorAdcOffsetV = THERMISTOR_ADC_OFFSET_V;
static const uint16_t THERMISTOR_OFFSET_CAL_SAMPLES = 64;

// =========================
// MPU6050 registers/config
// =========================
#define MPU6050_REG_PWR_MGMT_1   0x6B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_ACCEL_XOUT_H 0x3B

#define MPU6050_ACCEL_SENS_2G 16384.0f
#define GRAVITY_MPS2          9.80665f

// =========================
// GPS
// =========================
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

// =========================
// Logging state
// =========================
static const uint16_t SD_MAX_LOG_FILES = 999;
char g_sdLogFile[48] = "/telemetry_unknown.csv";
uint16_t g_sdLogIndex = 0;

bool g_sdReady = false;
bool g_loggingEnabled = false;

static const char *LITTLEFS_LOG_FILE = "/telemetry_v2.bin";

volatile bool buttonEvent = false;
uint32_t lastButtonHandledMs = 0;
uint32_t lastGpsDiagnosticMs = 0;

// =========================
// Logged record
// =========================
struct TelemetryRecord_t
{
  uint32_t timestamp_ms;

  int16_t current_mA;
  int32_t voltage_mV;

  int16_t temperature_c_x100;
  uint8_t temperature_valid;

  int16_t accel_x_mps2_x100;
  int16_t accel_y_mps2_x100;
  int16_t accel_z_mps2_x100;
  uint16_t accel_mag_mps2_x100;

  uint8_t gps_location_valid;
  int32_t gps_lat_e7;
  int32_t gps_long_e7;
  int32_t gps_alt_cm;
  uint16_t gps_speed_kmph_x100;
  uint16_t gps_hdop_x100;
  uint8_t gps_sats;

  uint8_t gps_time_valid;
  uint8_t gps_hour;
  uint8_t gps_minute;
  uint8_t gps_second;
  uint8_t gps_centisecond;
} __attribute__((packed));

// =========================
// Forward declarations
// =========================
void serviceGps();
void updateLoggingLed();
bool selectNextSDLogFile();
void startLoggingNewFile(const char *reason);

// =========================
// Interrupt
// =========================
void IRAM_ATTR buttonISR()
{
  buttonEvent = true;
}

// =========================
// GPS helpers
// =========================
void serviceGps()
{
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }
}

bool gpsDateTimeValid()
{
  return gps.date.isValid() &&
         gps.time.isValid() &&
         gps.date.year() >= 2020;
}

void printGpsDiagnosticIfNeeded()
{
  uint32_t now = millis();

  if (now < 5000 || now - lastGpsDiagnosticMs < GPS_DIAGNOSTIC_INTERVAL_MS) {
    return;
  }

  lastGpsDiagnosticMs = now;

  if (gps.charsProcessed() < 10) {
    Serial.println("ERROR: No GPS data received. Check GPS TX->GPIO20, GPS RX->GPIO21, power, and baud rate.");
  }
}

bool waitForGpsDateTime(uint32_t timeoutMs)
{
  if (gpsDateTimeValid()) {
    return true;
  }

  Serial.println("Waiting for valid GPS UTC date/time before creating CSV...");

  uint32_t startMs = millis();
  uint32_t lastToggleMs = 0;
  uint32_t lastStatusMs = 0;
  bool ledState = false;

  while (millis() - startMs < timeoutMs) {
    serviceGps();

    if (gpsDateTimeValid()) {
      setLed(false);
      Serial.printf(
        "GPS UTC time acquired: %04u-%02u-%02u %02u:%02u:%02u.%02u\n",
        gps.date.year(),
        gps.date.month(),
        gps.date.day(),
        gps.time.hour(),
        gps.time.minute(),
        gps.time.second(),
        gps.time.centisecond()
      );
      return true;
    }

    uint32_t now = millis();

    if (now - lastToggleMs >= 250) {
      lastToggleMs = now;
      ledState = !ledState;
      setLed(ledState);
    }

    if (now - lastStatusMs >= 2000) {
      lastStatusMs = now;
      Serial.printf(
        "GPS waiting: chars=%lu, sats=%lu, dateValid=%u, timeValid=%u\n",
        (unsigned long)gps.charsProcessed(),
        (unsigned long)(gps.satellites.isValid() ? gps.satellites.value() : 0),
        gps.date.isValid() ? 1 : 0,
        gps.time.isValid() ? 1 : 0
      );
    }

    delay(10);
  }

  setLed(false);
  Serial.println("GPS UTC date/time was not acquired. Logging was not started.");
  return false;
}

void fillGpsFields(TelemetryRecord_t &rec)
{
  rec.gps_location_valid = gps.location.isValid() ? 1 : 0;

  rec.gps_lat_e7 = rec.gps_location_valid
                     ? (int32_t)lround(gps.location.lat() * 10000000.0)
                     : 0;
  rec.gps_long_e7 = rec.gps_location_valid
                      ? (int32_t)lround(gps.location.lng() * 10000000.0)
                      : 0;

  rec.gps_alt_cm = gps.altitude.isValid()
                     ? (int32_t)lround(gps.altitude.meters() * 100.0)
                     : 0;
  rec.gps_speed_kmph_x100 = gps.speed.isValid()
                              ? (uint16_t)lround(gps.speed.kmph() * 100.0)
                              : 0;
  rec.gps_hdop_x100 = gps.hdop.isValid()
                        ? (uint16_t)gps.hdop.value()
                        : 0;
  rec.gps_sats = gps.satellites.isValid()
                   ? (uint8_t)gps.satellites.value()
                   : 0;

  rec.gps_time_valid = gps.time.isValid() ? 1 : 0;

  if (rec.gps_time_valid) {
    rec.gps_hour = gps.time.hour();
    rec.gps_minute = gps.time.minute();
    rec.gps_second = gps.time.second();
    rec.gps_centisecond = gps.time.centisecond();
  } else {
    rec.gps_hour = 0;
    rec.gps_minute = 0;
    rec.gps_second = 0;
    rec.gps_centisecond = 0;
  }
}

// =========================
// LED helpers
// =========================
void setLed(bool on)
{
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void updateLoggingLed()
{
  setLed(g_loggingEnabled);
}

void blinkLedFor(uint32_t durationMs, uint32_t intervalMs = 250)
{
  uint32_t startMs = millis();
  uint32_t lastToggleMs = 0;
  bool ledState = false;

  while (millis() - startMs < durationMs) {
    serviceGps();

    uint32_t now = millis();
    if (now - lastToggleMs >= intervalMs) {
      lastToggleMs = now;
      ledState = !ledState;
      setLed(ledState);
    }

    delay(10);
  }

  setLed(false);
}

// =========================
// I2C helpers
// =========================
bool i2cWrite8(uint8_t devAddr, uint8_t regAddr, uint8_t value)
{
  Wire.beginTransmission(devAddr);
  Wire.write(regAddr);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool i2cWrite16BE(uint8_t devAddr, uint8_t regAddr, uint16_t value)
{
  Wire.beginTransmission(devAddr);
  Wire.write(regAddr);
  Wire.write((uint8_t)((value >> 8) & 0xFF));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool i2cReadBytes(uint8_t devAddr, uint8_t regAddr, uint8_t *buf, size_t len)
{
  Wire.beginTransmission(devAddr);
  Wire.write(regAddr);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  size_t received = Wire.requestFrom((int)devAddr, (int)len);
  if (received != len) {
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    buf[i] = Wire.read();
  }

  return true;
}

// =========================
// ADS1115 helpers
// =========================
bool adc16Init()
{
  Wire.beginTransmission(ADS1115_ADDR);
  return Wire.endTransmission() == 0;
}

bool ads1115ReadRaw(uint16_t configWord, int16_t &raw)
{
  if (!i2cWrite16BE(ADS1115_ADDR, ADS1115_REG_CONFIG, configWord)) {
    return false;
  }

  delay(10);
  serviceGps();

  uint8_t rx[2];
  if (!i2cReadBytes(ADS1115_ADDR, ADS1115_REG_CONVERSION, rx, 2)) {
    return false;
  }

  raw = (int16_t)((rx[0] << 8) | rx[1]);
  return true;
}

bool adc16ReadCurrent(float &currentA, int16_t &rawOut)
{
  int16_t raw;
  if (!ads1115ReadRaw(ADS1115_CONFIG_WORD_AIN0, raw)) {
    return false;
  }

  float adcVoltage = (float)raw * ADS1115_LSB_VOLTS;
  float sensorVoltage = adcVoltage / ADC_DIVIDER_RATIO;

  currentA = (sensorVoltage - ADC_ZERO_CURRENT_V) / ADC_SENSITIVITY;
  rawOut = raw;
  return true;
}

bool adc16ReadVoltage(float &voltageV)
{
  int16_t raw;
  if (!ads1115ReadRaw(ADS1115_CONFIG_WORD_AIN1, raw)) {
    return false;
  }

  float adcVoltage = (float)raw * ADS1115_LSB_VOLTS;
  voltageV = adcVoltage * VOLTAGE_INPUT_SCALE;
  return true;
}

bool adc16ReadAverageCurrent(float &avgCurrentA, float &avgVoltageV, int16_t &avgRaw)
{
  float currentSum = 0.0f;
  float voltageSum = 0.0f;
  int32_t rawSum = 0;
  uint16_t validSamples = 0;

  for (uint16_t i = 0; i < CURRENT_NUM_SAMPLES; i++) {
    float currentA;
    float voltageV;
    int16_t raw;

    if (adc16ReadCurrent(currentA, raw) &&
        adc16ReadVoltage(voltageV)) {
      currentSum += currentA;
      voltageSum += voltageV;
      rawSum += raw;
      validSamples++;
    }

    serviceGps();
    delay(CURRENT_SAMPLE_DELAY);
  }

  if (validSamples == 0) {
    return false;
  }

  avgCurrentA = currentSum / validSamples;
  avgVoltageV = voltageSum / validSamples;
  avgRaw = (int16_t)(rawSum / validSamples);
  return true;
}

// =========================
// Thermistor helpers
// =========================
bool readThermistorVoltage(float &voltageV)
{
  int16_t raw;
  if (!ads1115ReadRaw(ADS1115_CONFIG_WORD_AIN2, raw)) {
    return false;
  }

  voltageV = ((float)raw * ADS1115_LSB_VOLTS) - g_thermistorAdcOffsetV;
  return true;
}

bool readThermistorTemperature(float &temperatureC)
{
  float voltageV;
  if (!readThermistorVoltage(voltageV)) {
    return false;
  }

  // Open/short/I2C sanity checks.
  if (voltageV >= 3.25f || voltageV <= 0.10f) {
    return false;
  }

  float thermistorResistance =
    (voltageV * THERMISTOR_SERIES_R_OHMS) /
    (THERMISTOR_SUPPLY_V - voltageV);

  if (thermistorResistance <= 0.0f) {
    return false;
  }

  float inverseTemperature =
    logf(thermistorResistance / THERMISTOR_NOMINAL_R_OHMS) /
    THERMISTOR_B_COEFFICIENT;

  inverseTemperature +=
    1.0f / (THERMISTOR_NOMINAL_TEMP_C + 273.15f);

  temperatureC = (1.0f / inverseTemperature) - 273.15f;
  return isfinite(temperatureC);
}

void calibrateThermistorAdcOffset()
{
  Serial.println("Thermistor ADC offset calibration:");
  Serial.println("Ground ADS1115 AIN2, then leave it grounded while samples are collected.");

  double sumV = 0.0;
  uint16_t validSamples = 0;

  for (uint16_t i = 0; i < THERMISTOR_OFFSET_CAL_SAMPLES; i++) {
    int16_t raw;

    if (ads1115ReadRaw(ADS1115_CONFIG_WORD_AIN2, raw)) {
      sumV += (double)raw * ADS1115_LSB_VOLTS;
      validSamples++;
    }

    delay(5);
  }

  if (validSamples == 0) {
    Serial.println("Thermistor ADC offset calibration failed: no valid samples.");
    return;
  }

  g_thermistorAdcOffsetV = (float)(sumV / validSamples);

  Serial.printf(
    "Thermistor ADC offset = %.6f V. Copy this value into THERMISTOR_ADC_OFFSET_V to make it permanent.\n",
    g_thermistorAdcOffsetV
  );
}

// =========================
// MPU6050 helpers
// =========================
bool mpuInit()
{
  if (!i2cWrite8(MPU6050_ADDR, MPU6050_REG_PWR_MGMT_1, 0x00)) {
    return false;
  }

  delay(50);

  if (!i2cWrite8(MPU6050_ADDR, MPU6050_REG_ACCEL_CONFIG, 0x00)) {
    return false;
  }

  return true;
}

bool mpuReadAccel(float &ax, float &ay, float &az)
{
  uint8_t rx[6];

  if (!i2cReadBytes(MPU6050_ADDR, MPU6050_REG_ACCEL_XOUT_H, rx, 6)) {
    return false;
  }

  int16_t rawX = (int16_t)((rx[0] << 8) | rx[1]);
  int16_t rawY = (int16_t)((rx[2] << 8) | rx[3]);
  int16_t rawZ = (int16_t)((rx[4] << 8) | rx[5]);

  ax = ((float)rawX / MPU6050_ACCEL_SENS_2G) * GRAVITY_MPS2;
  ay = ((float)rawY / MPU6050_ACCEL_SENS_2G) * GRAVITY_MPS2;
  az = ((float)rawZ / MPU6050_ACCEL_SENS_2G) * GRAVITY_MPS2;
  return true;
}

// =========================
// I2C scan
// =========================
void scanI2C()
{
  Serial.println("I2C probe start");

  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);

    if (Wire.endTransmission() == 0) {
      Serial.printf("Found I2C device at 0x%02X\n", address);
    }
  }

  Serial.println("I2C probe done");
}

// =========================
// Current offset calibration
// =========================
bool calibrateCurrentOffset()
{
  Serial.printf(
    "Waiting %lu ms before zero-current calibration...\n",
    (unsigned long)CURRENT_ZERO_CAL_DELAY_MS
  );

  blinkLedFor(CURRENT_ZERO_CAL_DELAY_MS, 250);

  Serial.println("Calibrating current offset for 3 seconds.");
  Serial.println("Keep the current sensor at resting current.");

  uint32_t startMs = millis();
  uint32_t lastToggleMs = 0;
  bool ledState = false;

  float sumA = 0.0f;
  int32_t rawSum = 0;
  uint32_t sampleCount = 0;

  while (millis() - startMs < CURRENT_ZERO_CAL_TIME_MS) {
    serviceGps();

    uint32_t now = millis();
    if (now - lastToggleMs >= 250) {
      lastToggleMs = now;
      ledState = !ledState;
      setLed(ledState);
    }

    int16_t raw;
    float currentA;

    if (adc16ReadCurrent(currentA, raw)) {
      sumA += currentA;
      rawSum += raw;
      sampleCount++;
    }

    delay(10);
  }

  setLed(false);

  if (sampleCount == 0) {
    Serial.println("Current offset calibration failed: no valid samples.");
    return false;
  }

  g_currentOffsetA = sumA / sampleCount;

  Serial.printf(
    "Current offset = %.6f A (%ld mA), average raw=%ld, samples=%lu\n",
    g_currentOffsetA,
    (long)lroundf(g_currentOffsetA * 1000.0f),
    (long)(rawSum / (int32_t)sampleCount),
    (unsigned long)sampleCount
  );

  return true;
}

// =========================
// LittleFS binary backup
// =========================
bool appendRecordLittleFS(const TelemetryRecord_t &rec)
{
  File file = LittleFS.open(LITTLEFS_LOG_FILE, FILE_APPEND);
  if (!file) {
    return false;
  }

  size_t bytesWritten =
    file.write((const uint8_t *)&rec, sizeof(TelemetryRecord_t));

  file.close();
  return bytesWritten == sizeof(TelemetryRecord_t);
}

void dumpLittleFSLogCsv()
{
  File file = LittleFS.open(LITTLEFS_LOG_FILE, FILE_READ);

  if (!file) {
    Serial.println("No LittleFS backup log found.");
    return;
  }

  Serial.println(
    "timestamp_ms,current_mA,voltage_mV,temperature_C,temperature_valid,"
    "ax_x100,ay_x100,az_x100,amag_x100,"
    "gps_location_valid,gps_lat,gps_long,gps_alt_m,gps_speed_kmph,"
    "gps_hdop,gps_sats,gps_time_valid,gps_time_utc,gps_centisecond"
  );

  TelemetryRecord_t rec;
  uint32_t count = 0;

  while (file.available() >= (int)sizeof(TelemetryRecord_t)) {
    size_t bytesRead =
      file.read((uint8_t *)&rec, sizeof(TelemetryRecord_t));

    if (bytesRead != sizeof(TelemetryRecord_t)) {
      Serial.println("Partial/corrupt LittleFS record encountered.");
      break;
    }

    Serial.printf(
      "%lu,%d,%ld,%.2f,%u,%d,%d,%d,%u,%u,%.7f,%.7f,%.2f,%.2f,%.2f,%u,%u,%02u:%02u:%02u,%02u\n",
      (unsigned long)rec.timestamp_ms,
      rec.current_mA,
      (long)rec.voltage_mV,
      rec.temperature_c_x100 / 100.0f,
      rec.temperature_valid,
      rec.accel_x_mps2_x100,
      rec.accel_y_mps2_x100,
      rec.accel_z_mps2_x100,
      rec.accel_mag_mps2_x100,
      rec.gps_location_valid,
      rec.gps_lat_e7 / 10000000.0,
      rec.gps_long_e7 / 10000000.0,
      rec.gps_alt_cm / 100.0f,
      rec.gps_speed_kmph_x100 / 100.0f,
      rec.gps_hdop_x100 / 100.0f,
      rec.gps_sats,
      rec.gps_time_valid,
      rec.gps_hour,
      rec.gps_minute,
      rec.gps_second,
      rec.gps_centisecond
    );

    count++;
  }

  file.close();
  Serial.printf("LittleFS dump complete: %lu records.\n", (unsigned long)count);
}

void eraseLittleFSLog()
{
  if (!LittleFS.exists(LITTLEFS_LOG_FILE)) {
    Serial.println("No LittleFS backup log to erase.");
    return;
  }

  if (LittleFS.remove(LITTLEFS_LOG_FILE)) {
    Serial.println("LittleFS backup log erased.");
  } else {
    Serial.println("Failed to erase LittleFS backup log.");
  }
}

// =========================
// SD card helpers
// =========================
bool initSDCard()
{
  Serial.println("Initializing SD card...");

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD card mount failed.");
    return false;
  }

  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached.");
    return false;
  }

  Serial.print("SD card type: ");

  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  Serial.printf(
    "SD card size: %llu MB\n",
    SD.cardSize() / (1024ULL * 1024ULL)
  );

  return true;
}

bool writeSDCsvHeader(const char *path)
{
  File file = SD.open(path, FILE_WRITE);

  if (!file) {
    Serial.printf("Failed to create %s\n", path);
    return false;
  }

  file.println(
    "timestamp_ms,current_mA,voltage_mV,temperature_C,temperature_valid,"
    "ax_x100,ay_x100,az_x100,amag_x100,"
    "gps_location_valid,gps_lat,gps_long,gps_alt_m,gps_speed_kmph,"
    "gps_hdop,gps_sats,gps_time_valid,gps_time_utc,gps_centisecond"
  );

  file.close();
  return true;
}

bool makeGpsTimestampLogFilename(char *out, size_t outSize)
{
  serviceGps();

  if (!gpsDateTimeValid()) {
    return false;
  }

  snprintf(
    out,
    outSize,
    "/%04u-%02u-%02u-%02u-%02u.csv",
    gps.date.year(),
    gps.date.month(),
    gps.date.day(),
    gps.time.hour(),
    gps.time.minute()
  );

  return true;
}

bool selectNextSDLogFile()
{
  if (!g_sdReady) {
    return false;
  }

  if (!makeGpsTimestampLogFilename(g_sdLogFile, sizeof(g_sdLogFile))) {
    return false;
  }

  if (!SD.exists(g_sdLogFile)) {
    if (!writeSDCsvHeader(g_sdLogFile)) {
      return false;
    }

    g_sdLogIndex = 0;
    Serial.printf("New SD log file: %s\n", g_sdLogFile);
    return true;
  }

  char baseName[40];
  strncpy(baseName, g_sdLogFile, sizeof(baseName));
  baseName[sizeof(baseName) - 1] = '\0';

  char *extension = strstr(baseName, ".csv");
  if (extension != nullptr) {
    *extension = '\0';
  }

  for (uint16_t index = 1; index <= SD_MAX_LOG_FILES; index++) {
    snprintf(
      g_sdLogFile,
      sizeof(g_sdLogFile),
      "%s-%03u.csv",
      baseName,
      index
    );

    if (!SD.exists(g_sdLogFile)) {
      if (!writeSDCsvHeader(g_sdLogFile)) {
        return false;
      }

      g_sdLogIndex = index;
      Serial.printf("New SD log file: %s\n", g_sdLogFile);
      return true;
    }
  }

  Serial.println("No free timestamped SD filename remains for this minute.");
  return false;
}

bool appendRecordSD(const TelemetryRecord_t &rec)
{
  if (!g_sdReady) {
    return false;
  }

  File file = SD.open(g_sdLogFile, FILE_APPEND);

  if (!file) {
    Serial.printf("Failed to open SD log file %s\n", g_sdLogFile);
    return false;
  }

  size_t bytesWritten = file.printf(
    "%lu,%d,%ld,%.2f,%u,%d,%d,%d,%u,%u,%.7f,%.7f,%.2f,%.2f,%.2f,%u,%u,%02u:%02u:%02u,%02u\n",
    (unsigned long)rec.timestamp_ms,
    rec.current_mA,
    (long)rec.voltage_mV,
    rec.temperature_c_x100 / 100.0f,
    rec.temperature_valid,
    rec.accel_x_mps2_x100,
    rec.accel_y_mps2_x100,
    rec.accel_z_mps2_x100,
    rec.accel_mag_mps2_x100,
    rec.gps_location_valid,
    rec.gps_lat_e7 / 10000000.0,
    rec.gps_long_e7 / 10000000.0,
    rec.gps_alt_cm / 100.0f,
    rec.gps_speed_kmph_x100 / 100.0f,
    rec.gps_hdop_x100 / 100.0f,
    rec.gps_sats,
    rec.gps_time_valid,
    rec.gps_hour,
    rec.gps_minute,
    rec.gps_second,
    rec.gps_centisecond
  );

  file.close();
  return bytesWritten > 0;
}

// =========================
// Logging control
// =========================
void startLoggingNewFile(const char *reason)
{
  if (!g_sdReady) {
    g_loggingEnabled = false;
    updateLoggingLed();
    Serial.println("Logging not started because the SD card is unavailable.");
    return;
  }

  if (!waitForGpsDateTime(GPS_FILENAME_WAIT_MS)) {
    g_loggingEnabled = false;
    updateLoggingLed();
    return;
  }

  if (!selectNextSDLogFile()) {
    g_loggingEnabled = false;
    updateLoggingLed();
    Serial.println("Logging not started because a timestamped CSV could not be created.");
    return;
  }

  g_loggingEnabled = true;
  updateLoggingLed();

  Serial.printf(
    "%s logging STARTED -> %s\n",
    reason != nullptr ? reason : "",
    g_sdLogFile
  );
}

void printHelp()
{
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  s = start logging in a new GPS-timestamped CSV");
  Serial.println("  x = stop logging");
  Serial.println("  d = stop logging and dump LittleFS backup as CSV");
  Serial.println("  e = stop logging and erase LittleFS backup");
  Serial.println("  c = calibrate thermistor ADS1115 offset; AIN2 must be grounded");
  Serial.println("  h = print help");
  Serial.println();
}

void handleSerialCommands()
{
  while (Serial.available()) {
    char command = Serial.read();

    if (command == 's') {
      if (g_loggingEnabled) {
        Serial.printf("Already logging to %s\n", g_sdLogFile);
      } else {
        startLoggingNewFile("Serial");
      }
    } else if (command == 'x') {
      g_loggingEnabled = false;
      updateLoggingLed();
      Serial.println("Logging STOPPED.");
    } else if (command == 'd') {
      g_loggingEnabled = false;
      updateLoggingLed();
      Serial.println("Logging stopped for LittleFS dump.");
      dumpLittleFSLogCsv();
    } else if (command == 'e') {
      g_loggingEnabled = false;
      updateLoggingLed();
      Serial.println("Logging stopped for LittleFS erase.");
      eraseLittleFSLog();
    } else if (command == 'c') {
      if (g_loggingEnabled) {
        Serial.println("Stop logging with 'x' before thermistor offset calibration.");
      } else {
        calibrateThermistorAdcOffset();
      }
    } else if (command == 'h') {
      printHelp();
    }
  }
}

// =========================
// Setup
// =========================
void setup()
{
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  // Start GPS before lengthy initialization so GPS data is parsed throughout.
  gpsSerial.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX_PIN,
    GPS_TX_PIN
  );

  blinkLedFor(1000, 150);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(
    digitalPinToInterrupt(BUTTON_PIN),
    buttonISR,
    FALLING
  );

  Serial.println();
  Serial.println("ESP32-C3 combined telemetry logger");

  g_sdReady = initSDCard();
  if (!g_sdReady) {
    Serial.println("SD initialization failed; logging will remain disabled.");
  }

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  scanI2C();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
    while (true) {
      serviceGps();
      delay(1000);
    }
  }
  Serial.println("LittleFS mounted.");

  if (!adc16Init()) {
    Serial.println("ADS1115 initialization failed.");
    while (true) {
      serviceGps();
      delay(1000);
    }
  }
  Serial.println("ADS1115 initialized.");

  if (!mpuInit()) {
    Serial.println("MPU6050 initialization failed.");
    while (true) {
      serviceGps();
      delay(1000);
    }
  }
  Serial.println("MPU6050 initialized.");

  if (!calibrateCurrentOffset()) {
    while (true) {
      serviceGps();
      delay(1000);
    }
  }

  printHelp();

  // Auto-start after setup. This waits for valid GPS UTC date/time.
  startLoggingNewFile("AUTO");

  Serial.println("LED ON = recording; OFF = stopped; BLINKING = initialization/GPS wait/calibration.");
}

// =========================
// Main loop
// =========================
void loop()
{
  // Always service GPS, including while logging is stopped.
  serviceGps();
  printGpsDiagnosticIfNeeded();
  handleSerialCommands();

  if (buttonEvent) {
    buttonEvent = false;

    uint32_t now = millis();

    if (now - lastButtonHandledMs > 250) {
      lastButtonHandledMs = now;

      if (g_loggingEnabled) {
        g_loggingEnabled = false;
        updateLoggingLed();
        Serial.println("Button: logging STOPPED.");
      } else {
        startLoggingNewFile("Button");
      }
    }
  }

  if (!g_loggingEnabled) {
    delay(20);
    return;
  }

  float currentA = 0.0f;
  float averageVoltageV = 0.0f;
  int16_t adcRaw = 0;

  if (!adc16ReadAverageCurrent(currentA, averageVoltageV, adcRaw)) {
    Serial.println("ADS1115 current/voltage read failed.");
    delay(50);
    return;
  }

  float temperatureC = 0.0f;
  bool temperatureValid = readThermistorTemperature(temperatureC);

  float axSum = 0.0f;
  float aySum = 0.0f;
  float azSum = 0.0f;
  uint16_t accelValidSamples = 0;

  for (uint16_t i = 0; i < MPU_NUM_SAMPLES; i++) {
    float ax;
    float ay;
    float az;

    if (mpuReadAccel(ax, ay, az)) {
      axSum += ax;
      aySum += ay;
      azSum += az;
      accelValidSamples++;
    }

    serviceGps();
    delay(MPU_SAMPLE_DELAY);
  }

  if (accelValidSamples == 0) {
    Serial.println("MPU6050 acceleration read failed.");
    delay(50);
    return;
  }

  float axAverage = axSum / accelValidSamples;
  float ayAverage = aySum / accelValidSamples;
  float azAverage = azSum / accelValidSamples;

  float accelerationMagnitude = sqrtf(
    axAverage * axAverage +
    ayAverage * ayAverage +
    azAverage * azAverage
  );

  float correctedCurrentA = currentA - g_currentOffsetA;

  TelemetryRecord_t rec = {};

  rec.timestamp_ms = millis();

  rec.current_mA = (int16_t)lroundf(correctedCurrentA * 1000.0f);
  rec.voltage_mV = (int32_t)lroundf(averageVoltageV * 1000.0f);

  rec.temperature_valid = temperatureValid ? 1 : 0;
  rec.temperature_c_x100 = temperatureValid
                             ? (int16_t)lroundf(temperatureC * 100.0f)
                             : 0;

  rec.accel_x_mps2_x100 = (int16_t)lroundf(axAverage * 100.0f);
  rec.accel_y_mps2_x100 = (int16_t)lroundf(ayAverage * 100.0f);
  rec.accel_z_mps2_x100 = (int16_t)lroundf(azAverage * 100.0f);
  rec.accel_mag_mps2_x100 =
    (uint16_t)lroundf(accelerationMagnitude * 100.0f);

  fillGpsFields(rec);

  if (!appendRecordLittleFS(rec)) {
    Serial.println("WARNING: LittleFS backup write failed.");
  }

  if (appendRecordSD(rec)) {
    Serial.printf(
      "SD LOG %s t=%lu ms I=%d mA V=%ld mV Temp=%s%.2f C "
      "Ax=%d Ay=%d Az=%d Mag=%u GPS=%u sats=%u raw=%d\n",
      g_sdLogFile,
      (unsigned long)rec.timestamp_ms,
      rec.current_mA,
      (long)rec.voltage_mV,
      rec.temperature_valid ? "" : "INVALID/",
      rec.temperature_c_x100 / 100.0f,
      rec.accel_x_mps2_x100,
      rec.accel_y_mps2_x100,
      rec.accel_z_mps2_x100,
      rec.accel_mag_mps2_x100,
      rec.gps_location_valid,
      rec.gps_sats,
      adcRaw
    );
  } else {
    Serial.println("SD write failed.");
  }

  delay(20);
}

out = Path('/mnt/data/combined_telemetry_logger.ino')
out.write_text(code)
print(f"Created {out} ({out.stat().st_size} bytes, {len(code.splitlines())} lines)")
