#define TINY_GSM_MODEM_A7670
#define TINY_GSM_RX_BUFFER 1024

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <TinyGsmClient.h>

#include "live_telemetry_packet.h"
#include "dyno_telemetry_packet.h"

#if __has_include("relay_config.h")
#include "relay_config.h"
#else
#warning "Using placeholder relay_config.example.h; copy it to relay_config.h"
#include "relay_config.example.h"
#endif

// Official LilyGO T-A7670X ESP32-WROVER-E pin map.
static const int BOARD_POWERON_PIN = 12;
static const int MODEM_DTR_PIN = 25;
static const int MODEM_RX_PIN = 27;
static const int MODEM_TX_PIN = 26;
static const int MODEM_PWRKEY_PIN = 4;
static const int MODEM_RESET_PIN = 5;
// The tested T-A7670X R2 hardware did not start with the generic 100 ms pulse.
// A 1-second pulse reliably starts the modem on this board revision.
static const uint32_t MODEM_POWER_ON_PULSE_MS = 1000;
static const uint32_t MODEM_START_WAIT_MS = 15000;
static const uint8_t LIVE_TELEMETRY_ESPNOW_CHANNEL = 1;
static const uint32_t LTE_RECONNECT_INTERVAL_MS = 30000;

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);

// A live dashboard values freshness over delivery of old rows. Keep only the
// newest packet while an LTE POST is in progress; a newer arrival supersedes
// the pending one instead of waiting behind it in a FIFO.
LiveTelemetryPacket latestCarPacket;
DynoTelemetryPacket latestDynoPacket;
volatile bool latestCarPacketAvailable = false;
volatile bool latestDynoPacketAvailable = false;
volatile uint32_t supersededCarPackets = 0;
volatile uint32_t supersededDynoPackets = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t lastNetworkAttemptMs = 0;
bool networkReady = false;
bool httpServiceReady = false;

void enqueuePacket(const uint8_t *data, int length)
{
  if (length == sizeof(LiveTelemetryPacket)) {
    LiveTelemetryPacket packet;
    memcpy(&packet, data, sizeof(packet));
    if (!isValidLiveTelemetryPacket(packet)) return;

    portENTER_CRITICAL(&rxMux);
    if (latestCarPacketAvailable) supersededCarPackets++;
    latestCarPacket = packet;
    latestCarPacketAvailable = true;
    portEXIT_CRITICAL(&rxMux);
    return;
  }

  if (length == sizeof(DynoTelemetryPacket)) {
    DynoTelemetryPacket packet;
    memcpy(&packet, data, sizeof(packet));
    if (!isValidDynoTelemetryPacket(packet)) return;

    portENTER_CRITICAL(&rxMux);
    if (latestDynoPacketAvailable) supersededDynoPackets++;
    latestDynoPacket = packet;
    latestDynoPacketAvailable = true;
    portEXIT_CRITICAL(&rxMux);
  }
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowReceive(const esp_now_recv_info_t *, const uint8_t *data, int length)
{
  enqueuePacket(data, length);
}
#else
void onEspNowReceive(const uint8_t *, const uint8_t *data, int length)
{
  enqueuePacket(data, length);
}
#endif

bool dequeueCarPacket(LiveTelemetryPacket &packet)
{
  bool available = false;
  portENTER_CRITICAL(&rxMux);
  if (latestCarPacketAvailable) {
    packet = latestCarPacket;
    latestCarPacketAvailable = false;
    available = true;
  }
  portEXIT_CRITICAL(&rxMux);
  return available;
}

bool dequeueDynoPacket(DynoTelemetryPacket &packet)
{
  bool available = false;
  portENTER_CRITICAL(&rxMux);
  if (latestDynoPacketAvailable) {
    packet = latestDynoPacket;
    latestDynoPacketAvailable = false;
    available = true;
  }
  portEXIT_CRITICAL(&rxMux);
  return available;
}

bool probeModemAt(uint32_t timeoutMs)
{
  while (SerialAT.available()) SerialAT.read();
  uint32_t deadline = millis() + timeoutMs;
  uint32_t nextAttempt = 0;
  String response;

  while (static_cast<int32_t>(deadline - millis()) > 0) {
    if (static_cast<int32_t>(millis() - nextAttempt) >= 0) {
      SerialAT.print("AT\r\n");
      nextAttempt = millis() + 500;
    }
    while (SerialAT.available()) {
      response += static_cast<char>(SerialAT.read());
      if (response.indexOf("OK") >= 0) return true;
      if (response.length() > 128) response.remove(0, 64);
    }
    delay(1);
  }
  return false;
}

bool powerOnModem()
{
  // GPIO 12 enables power to the modem and SD peripherals on T-A7670X.
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);

  // Keep reset inactive. Uploading a new ESP32 sketch does not necessarily
  // power-cycle the modem, so first check whether it is already running.
  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, LOW);

  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);

  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(3000);

  if (probeModemAt(3000)) {
    Serial.println("A7670X was already powered on");
    return true;
  }

  Serial.println("A7670X is off; applying 1-second PWRKEY pulse...");
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(MODEM_POWER_ON_PULSE_MS);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(MODEM_START_WAIT_MS);

  if (probeModemAt(5000)) {
    Serial.println("A7670X powered on and answered AT");
    return true;
  }

  Serial.println("A7670X did not answer after PWRKEY startup");
  return false;
}

bool connectLte()
{
  httpServiceReady = false;
  Serial.println("Initializing A7670X...");
  if (!probeModemAt(3000)) {
    Serial.println("A7670X is not answering AT commands");
    return false;
  }
  if (!modem.init()) {
    Serial.println("A7670X answered AT, but TinyGSM initialization failed");
    return false;
  }

  if (SIM_PIN[0] != '\0' && modem.getSimStatus() != 3) {
    if (!modem.simUnlock(SIM_PIN)) {
      Serial.println("SIM unlock failed");
      return false;
    }
  }

  Serial.println("Waiting for LTE registration...");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println("LTE registration timed out");
    return false;
  }

  String operatorName = modem.getOperator();
  int16_t signalQuality = modem.getSignalQuality();
  Serial.printf(
    "Registered: operator='%s', signal CSQ=%d (99 means unknown)\n",
    operatorName.c_str(),
    signalQuality
  );

  Serial.printf("Connecting APN '%s'...\n", LTE_APN);
  if (!modem.gprsConnect(LTE_APN, LTE_USER, LTE_PASSWORD)) {
    Serial.println("Packet-data connection failed");
    Serial.printf("SIM status=%d, network connected=%s\n",
                  (int)modem.getSimStatus(),
                  modem.isNetworkConnected() ? "yes" : "no");
    Serial.print("PDP context after failure: ");
    Serial.println(modem.getLocalIP());
    Serial.println(
      "Check SIM activation/data in a phone, SIM PIN, APN, antenna, and power."
    );
    return false;
  }

  Serial.print("LTE connected; IP: ");
  Serial.println(modem.getLocalIP());
  return true;
}

bool atCommand(const String &command, uint32_t timeoutMs = 5000)
{
  modem.sendAT(command);
  return modem.waitResponse(timeoutMs) == 1;
}

int parseHttpStatus(const String &actionLine)
{
  int firstComma = actionLine.indexOf(',');
  if (firstComma < 0) return -1;
  int secondComma = actionLine.indexOf(',', firstComma + 1);
  String status = secondComma < 0
    ? actionLine.substring(firstComma + 1)
    : actionLine.substring(firstComma + 1, secondComma);
  status.trim();
  return status.toInt();
}

void stopHttpService()
{
  atCommand("+HTTPTERM", 2000);
  httpServiceReady = false;
}

bool startHttpService()
{
  if (httpServiceReady) return true;

  // HTTPINIT can fail when a prior request left the modem service open. Only
  // pay the HTTPTERM recovery cost when initialization actually needs it.
  if (!atCommand("+HTTPINIT")) {
    atCommand("+HTTPTERM", 2000);
    if (!atCommand("+HTTPINIT")) return false;
  }
  // A7670X selects HTTP versus HTTPS from the URL. HTTPSSL and the
  // HTTPPARA="CID" form are SIM7600-specific and return ERROR here.
  if (!atCommand(String("+HTTPPARA=\"URL\",\"") + TELEMETRY_ENDPOINT + "\"")) {
    stopHttpService();
    return false;
  }
  if (!atCommand("+HTTPPARA=\"CONTENT\",\"application/json\"")) {
    stopHttpService();
    return false;
  }
  if (!atCommand(String("+HTTPPARA=\"USERDATA\",\"X-Telemetry-Key: ") +
                 TELEMETRY_API_KEY + "\"")) {
    stopHttpService();
    return false;
  }
  httpServiceReady = true;
  return true;
}

bool postJson(const String &json)
{
  if (!startHttpService()) return false;

  modem.sendAT("+HTTPDATA=", json.length(), ",10000");
  if (modem.waitResponse(10000L, GF("DOWNLOAD"), GF("ERROR")) != 1) {
    stopHttpService();
    return false;
  }
  SerialAT.print(json);
  if (modem.waitResponse(10000L) != 1) {
    stopHttpService();
    return false;
  }

  modem.sendAT("+HTTPACTION=1");
  if (modem.waitResponse(10000L) != 1) {
    stopHttpService();
    return false;
  }

  String prefix;
  if (modem.waitResponse(65000L, prefix, GF("+HTTPACTION:"), GF("ERROR")) != 1) {
    stopHttpService();
    return false;
  }
  String actionLine = SerialAT.readStringUntil('\n');
  int status = parseHttpStatus(actionLine);

  Serial.printf("Dashboard POST status=%d\n", status);
  return status >= 200 && status < 300;
}

String packetToJson(const LiveTelemetryPacket &packet)
{
  String json;
  json.reserve(420);
  json += "{\"device_id\":\"";
  json += TELEMETRY_DEVICE_ID;
  json += "\",\"source_type\":\"car\",\"source_boot_id\":";
  json += packet.boot_id;
  json += ",\"sequence\":";
  json += packet.sequence;
  json += ",\"timestamp_ms\":";
  json += packet.timestamp_ms;
  json += ",\"current_mA\":";
  json += packet.current_mA;
  json += ",\"voltage_mV\":";
  json += packet.voltage_mV;
  json += ",\"motor_temperature_valid\":";
  json += packet.motor_temperature_valid ? "true" : "false";
  json += ",\"motor_temperature_C\":";
  if (packet.motor_temperature_valid) {
    json += String(packet.motor_temperature_c_x100 / 100.0f, 2);
  } else {
    json += "null";
  }
  json += ",\"ax_x100\":";
  json += packet.ax_x100;
  json += ",\"ay_x100\":";
  json += packet.ay_x100;
  json += ",\"az_x100\":";
  json += packet.az_x100;
  json += ",\"amag_x100\":";
  json += packet.amag_x100;
  json += ",\"wheel_speed_valid\":";
  json += packet.wheel_speed_valid ? "true" : "false";
  json += ",\"wheel_speed_kph\":";
  if (packet.wheel_speed_valid) {
    json += String(packet.wheel_speed_kmph_x100 / 100.0f, 2);
  } else {
    json += "null";
  }

  if (packet.flags & LIVE_TELEMETRY_FLAG_GPS_VALID) {
    json += ",\"latitude\":";
    json += String(packet.latitude_e7 / 10000000.0, 7);
    json += ",\"longitude\":";
    json += String(packet.longitude_e7 / 10000000.0, 7);
  }
  json += "}";
  return json;
}

String dynoPacketToJson(const DynoTelemetryPacket &packet)
{
  String json;
  json.reserve(320);
  json += "{\"device_id\":\"";
  json += TELEMETRY_DEVICE_ID;
  json += "-dyno\",\"source_type\":\"dyno\",\"source_boot_id\":";
  json += packet.boot_id;
  json += ",\"sequence\":";
  json += packet.sequence;
  json += ",\"timestamp_ms\":";
  json += packet.timestamp_ms;
  json += ",\"current_mA\":";
  json += packet.current_mA;
  json += ",\"voltage_mV\":";
  json += packet.voltage_mV;
  json += ",\"ax_x100\":0,\"ay_x100\":0,\"az_x100\":0,\"amag_x100\":0";
  json += ",\"reported_power_W\":";
  json += String(packet.power_mW / 1000.0, 3);
  json += ",\"source_energy_Wh\":";
  json += String(packet.energy_mJ / 3600000.0, 6);
  json += ",\"dyno_state\":";
  json += packet.state;
  json += "}";
  return json;
}

void printGpsPacketStatus(const LiveTelemetryPacket &packet)
{
  uint8_t satellites =
    (packet.flags & LIVE_TELEMETRY_GPS_SATS_MASK) >>
    LIVE_TELEMETRY_GPS_SATS_SHIFT;

  Serial.printf(
    "GPS seq=%lu rx=GPIO%u baud=%lu bytes=%lu nmea=%s sats=%u utc=%s fix=%s\n",
    static_cast<unsigned long>(packet.sequence),
    (packet.flags & LIVE_TELEMETRY_FLAG_GPS_RX_GPIO21) ? 21 : 20,
    static_cast<unsigned long>(packet.gps_uart_baud),
    static_cast<unsigned long>(packet.gps_uart_bytes),
    (packet.flags & LIVE_TELEMETRY_FLAG_GPS_UART_ACTIVE) ? "yes" : "no",
    satellites,
    (packet.flags & LIVE_TELEMETRY_FLAG_GPS_TIME_VALID) ? "yes" : "no",
    (packet.flags & LIVE_TELEMETRY_FLAG_GPS_VALID) ? "yes" : "no"
  );
}

LiveTelemetryPacket makeDummyPacket()
{
  static uint32_t dummyBootId = esp_random();
  static uint32_t dummySequence = 0;

  float phase = static_cast<float>(dummySequence % 40) / 40.0f * 2.0f * PI;
  LiveTelemetryPacket packet = {};
  packet.magic = LIVE_TELEMETRY_MAGIC;
  packet.version = LIVE_TELEMETRY_VERSION;
  packet.flags = LTE_DUMMY_INCLUDE_GPS
    ? LIVE_TELEMETRY_FLAG_GPS_VALID |
      LIVE_TELEMETRY_FLAG_GPS_UART_ACTIVE |
      LIVE_TELEMETRY_FLAG_GPS_TIME_VALID |
      LIVE_TELEMETRY_FLAG_GPS_RX_GPIO21 |
      (12 << LIVE_TELEMETRY_GPS_SATS_SHIFT)
    : 0;
  packet.packet_size = sizeof(packet);
  packet.boot_id = dummyBootId;
  packet.sequence = dummySequence++;
  packet.timestamp_ms = millis();
  packet.current_mA = static_cast<int16_t>(7000.0f + 2500.0f * sinf(phase));
  packet.voltage_mV = 24000 - packet.current_mA / 20;
  packet.motor_temperature_valid = 1;
  packet.motor_temperature_c_x100 =
    static_cast<int16_t>(4200.0f + 800.0f * sinf(phase * 0.25f));
  packet.ax_x100 = static_cast<int16_t>(80.0f * sinf(phase * 1.7f));
  packet.ay_x100 = static_cast<int16_t>(55.0f * cosf(phase * 1.3f));
  packet.az_x100 = 981;
  packet.amag_x100 = static_cast<uint16_t>(985.0f + 20.0f * sinf(phase));
  packet.wheel_speed_valid = 1;
  packet.wheel_speed_kmph_x100 =
    static_cast<uint16_t>(1800.0f + 900.0f * (1.0f + sinf(phase)));
  packet.gps_uart_baud = 9600;
  packet.gps_uart_bytes = dummySequence * 960;

  // Small fake loop near Indianapolis so the Level 2 test exercises the map.
  packet.latitude_e7 = 397991700 + static_cast<int32_t>(4500.0f * sinf(phase));
  packet.longitude_e7 = -862380100 + static_cast<int32_t>(6000.0f * cosf(phase));
  return packet;
}

bool sendLivePacket(const LiveTelemetryPacket &packet, const char *sourceLabel)
{
  String json = packetToJson(packet);
  uint32_t postStartedMs = millis();
  if (postJson(json)) {
    uint32_t postElapsedMs = millis() - postStartedMs;
    Serial.printf("%s seq=%lu delivered in %lu ms json=%u B\n",
                  sourceLabel,
                  static_cast<unsigned long>(packet.sequence),
                  static_cast<unsigned long>(postElapsedMs),
                  static_cast<unsigned int>(json.length()));
    return true;
  }

  Serial.printf("%s seq=%lu POST failed after %lu ms json=%u B\n",
                sourceLabel,
                static_cast<unsigned long>(packet.sequence),
                static_cast<unsigned long>(millis() - postStartedMs),
                static_cast<unsigned int>(json.length()));
  networkReady = false;
  return false;
}

bool sendDynoPacket(const DynoTelemetryPacket &packet)
{
  String json = dynoPacketToJson(packet);
  uint32_t postStartedMs = millis();
  if (postJson(json)) {
    Serial.printf("DYNO seq=%lu delivered in %lu ms P=%.3f W json=%u B\n",
                  static_cast<unsigned long>(packet.sequence),
                  static_cast<unsigned long>(millis() - postStartedMs),
                  packet.power_mW / 1000.0f,
                  static_cast<unsigned int>(json.length()));
    return true;
  }

  Serial.printf("DYNO seq=%lu POST failed after %lu ms json=%u B\n",
                static_cast<unsigned long>(packet.sequence),
                static_cast<unsigned long>(millis() - postStartedMs),
                static_cast<unsigned int>(json.length()));
  networkReady = false;
  return false;
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("UTSM T-A7670X live telemetry relay");
  Serial.println(LTE_DUMMY_TEST_MODE
    ? "Mode: LEVEL 2 LTE DUMMY TEST (ESP-NOW input ignored)"
    : "Mode: LIVE ESP-NOW RELAY");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(LIVE_TELEMETRY_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.print("Relay ESP-NOW MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Relay ESP-NOW channel: %u\n", LIVE_TELEMETRY_ESPNOW_CHANNEL);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onEspNowReceive);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  bool modemReady = powerOnModem();
  networkReady = modemReady && connectLte();
  lastNetworkAttemptMs = millis();
}

void loop()
{
  if (!networkReady) {
    if (millis() - lastNetworkAttemptMs >= LTE_RECONNECT_INTERVAL_MS) {
      lastNetworkAttemptMs = millis();
      networkReady = connectLte();
    }
  }

  if (LTE_DUMMY_TEST_MODE) {
    static uint32_t lastDummySendMs = 0;
    uint32_t now = millis();
    if (networkReady && now - lastDummySendMs >= LTE_DUMMY_SEND_INTERVAL_MS) {
      lastDummySendMs = now;
      LiveTelemetryPacket dummy = makeDummyPacket();
      sendLivePacket(dummy, "DUMMY");
    }
    delay(5);
    return;
  }

  static bool preferDyno = false;
  LiveTelemetryPacket carPacket;
  DynoTelemetryPacket dynoPacket;
  bool haveCar = false;
  bool haveDyno = false;
  if (preferDyno) haveDyno = dequeueDynoPacket(dynoPacket);
  if (!haveDyno) haveCar = dequeueCarPacket(carPacket);
  if (!haveDyno && !haveCar) haveDyno = dequeueDynoPacket(dynoPacket);
  if (!haveCar && !haveDyno) {
    delay(5);
    return;
  }

  if (!networkReady) {
    if (haveDyno) {
      Serial.printf("Dropping dyno seq=%lu while LTE is offline\n",
                    static_cast<unsigned long>(dynoPacket.sequence));
    } else {
      Serial.printf("Dropping live seq=%lu while LTE is offline\n",
                    static_cast<unsigned long>(carPacket.sequence));
    }
    return;
  }

  if (haveDyno) {
    sendDynoPacket(dynoPacket);
    preferDyno = false;
  } else {
    printGpsPacketStatus(carPacket);
    sendLivePacket(carPacket, "LIVE");
    preferDyno = true;
  }

  static uint32_t lastCarSupersededReport = 0;
  static uint32_t lastDynoSupersededReport = 0;
  if (supersededCarPackets != lastCarSupersededReport ||
      supersededDynoPackets != lastDynoSupersededReport) {
    lastCarSupersededReport = supersededCarPackets;
    lastDynoSupersededReport = supersededDynoPackets;
    Serial.printf("ESP-NOW superseded car=%lu dyno=%lu\n",
                  static_cast<unsigned long>(lastCarSupersededReport),
                  static_cast<unsigned long>(lastDynoSupersededReport));
  }
}
