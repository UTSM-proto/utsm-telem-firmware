#define TINY_GSM_MODEM_A7670
#define TINY_GSM_RX_BUFFER 1024

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_arduino_version.h>
#include <TinyGsmClient.h>

#include "live_telemetry_packet.h"

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
static const uint32_t MODEM_START_WAIT_MS = 3000;

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);

// This small queue only transfers records safely out of the ESP-NOW callback.
// It is not persistent outage buffering; overflow drops the oldest live row.
static const size_t RX_QUEUE_SIZE = 12;
LiveTelemetryPacket rxQueue[RX_QUEUE_SIZE];
volatile size_t rxHead = 0;
volatile size_t rxTail = 0;
volatile uint32_t droppedPackets = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t lastNetworkAttemptMs = 0;
bool networkReady = false;

void enqueuePacket(const uint8_t *data, int length)
{
  if (length != sizeof(LiveTelemetryPacket)) return;

  LiveTelemetryPacket packet;
  memcpy(&packet, data, sizeof(packet));
  if (!isValidLiveTelemetryPacket(packet)) return;

  portENTER_CRITICAL(&rxMux);
  size_t next = (rxHead + 1) % RX_QUEUE_SIZE;
  if (next == rxTail) {
    rxTail = (rxTail + 1) % RX_QUEUE_SIZE;
    droppedPackets++;
  }
  rxQueue[rxHead] = packet;
  rxHead = next;
  portEXIT_CRITICAL(&rxMux);
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

bool dequeuePacket(LiveTelemetryPacket &packet)
{
  bool available = false;
  portENTER_CRITICAL(&rxMux);
  if (rxTail != rxHead) {
    packet = rxQueue[rxTail];
    rxTail = (rxTail + 1) % RX_QUEUE_SIZE;
    available = true;
  }
  portEXIT_CRITICAL(&rxMux);
  return available;
}

void powerOnModem()
{
  // GPIO 12 enables power to the modem and SD peripherals on T-A7670X.
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);

  // Follow LilyGO's A7670X reset sequence (active HIGH reset).
  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  delay(2600);
  digitalWrite(MODEM_RESET_PIN, LOW);

  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);

  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(MODEM_POWER_ON_PULSE_MS);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(MODEM_START_WAIT_MS);
}

bool connectLte()
{
  Serial.println("Initializing A7670X...");
  if (!modem.init()) {
    Serial.println("A7670X did not answer AT commands");
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

  Serial.printf("Connecting APN '%s'...\n", LTE_APN);
  if (!modem.gprsConnect(LTE_APN, LTE_USER, LTE_PASSWORD)) {
    Serial.println("Packet-data connection failed");
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

bool postJson(const String &json)
{
  // Clear a stale service from a previous failed request.
  atCommand("+HTTPTERM", 2000);
  if (!atCommand("+HTTPINIT")) return false;
  // A7670X selects HTTP versus HTTPS from the URL. HTTPSSL and the
  // HTTPPARA="CID" form are SIM7600-specific and return ERROR here.
  if (!atCommand(String("+HTTPPARA=\"URL\",\"") + TELEMETRY_ENDPOINT + "\"")) return false;
  if (!atCommand("+HTTPPARA=\"CONTENT\",\"application/json\"")) return false;
  if (!atCommand(String("+HTTPPARA=\"USERDATA\",\"X-Telemetry-Key: ") +
                 TELEMETRY_API_KEY + "\"")) return false;

  modem.sendAT("+HTTPDATA=", json.length(), ",10000");
  if (modem.waitResponse(10000L, GF("DOWNLOAD"), GF("ERROR")) != 1) {
    atCommand("+HTTPTERM", 2000);
    return false;
  }
  SerialAT.print(json);
  if (modem.waitResponse(10000L) != 1) {
    atCommand("+HTTPTERM", 2000);
    return false;
  }

  modem.sendAT("+HTTPACTION=1");
  if (modem.waitResponse(10000L) != 1) {
    atCommand("+HTTPTERM", 2000);
    return false;
  }

  String prefix;
  if (modem.waitResponse(65000L, prefix, GF("+HTTPACTION:"), GF("ERROR")) != 1) {
    atCommand("+HTTPTERM", 2000);
    return false;
  }
  String actionLine = SerialAT.readStringUntil('\n');
  int status = parseHttpStatus(actionLine);
  atCommand("+HTTPTERM", 2000);

  Serial.printf("Dashboard POST status=%d\n", status);
  return status >= 200 && status < 300;
}

String packetToJson(const LiveTelemetryPacket &packet)
{
  String json;
  json.reserve(360);
  json += "{\"device_id\":\"";
  json += TELEMETRY_DEVICE_ID;
  json += "\",\"source_boot_id\":";
  json += packet.boot_id;
  json += ",\"sequence\":";
  json += packet.sequence;
  json += ",\"timestamp_ms\":";
  json += packet.timestamp_ms;
  json += ",\"current_mA\":";
  json += packet.current_mA;
  json += ",\"voltage_mV\":";
  json += packet.voltage_mV;
  json += ",\"ax_x100\":";
  json += packet.ax_x100;
  json += ",\"ay_x100\":";
  json += packet.ay_x100;
  json += ",\"az_x100\":";
  json += packet.az_x100;
  json += ",\"amag_x100\":";
  json += packet.amag_x100;

  if (packet.flags & LIVE_TELEMETRY_FLAG_GPS_VALID) {
    json += ",\"latitude\":";
    json += String(packet.latitude_e7 / 10000000.0, 7);
    json += ",\"longitude\":";
    json += String(packet.longitude_e7 / 10000000.0, 7);
  }
  json += "}";
  return json;
}

LiveTelemetryPacket makeDummyPacket()
{
  static uint32_t dummyBootId = esp_random();
  static uint32_t dummySequence = 0;

  float phase = static_cast<float>(dummySequence % 40) / 40.0f * 2.0f * PI;
  LiveTelemetryPacket packet = {};
  packet.magic = LIVE_TELEMETRY_MAGIC;
  packet.version = LIVE_TELEMETRY_VERSION;
  packet.flags = LTE_DUMMY_INCLUDE_GPS ? LIVE_TELEMETRY_FLAG_GPS_VALID : 0;
  packet.packet_size = sizeof(packet);
  packet.boot_id = dummyBootId;
  packet.sequence = dummySequence++;
  packet.timestamp_ms = millis();
  packet.current_mA = static_cast<int16_t>(7000.0f + 2500.0f * sinf(phase));
  packet.voltage_mV = 24000 - packet.current_mA / 20;
  packet.ax_x100 = static_cast<int16_t>(80.0f * sinf(phase * 1.7f));
  packet.ay_x100 = static_cast<int16_t>(55.0f * cosf(phase * 1.3f));
  packet.az_x100 = 981;
  packet.amag_x100 = static_cast<uint16_t>(985.0f + 20.0f * sinf(phase));

  // Small fake loop near Indianapolis so the Level 2 test exercises the map.
  packet.latitude_e7 = 397991700 + static_cast<int32_t>(4500.0f * sinf(phase));
  packet.longitude_e7 = -862380100 + static_cast<int32_t>(6000.0f * cosf(phase));
  return packet;
}

bool sendLivePacket(const LiveTelemetryPacket &packet, const char *sourceLabel)
{
  String json = packetToJson(packet);
  if (postJson(json)) {
    Serial.printf("%s seq=%lu delivered\n",
                  sourceLabel,
                  static_cast<unsigned long>(packet.sequence));
    return true;
  }

  Serial.printf("%s seq=%lu POST failed\n",
                sourceLabel,
                static_cast<unsigned long>(packet.sequence));
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
  Serial.print("Relay ESP-NOW MAC: ");
  Serial.println(WiFi.macAddress());
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onEspNowReceive);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  powerOnModem();
  networkReady = connectLte();
  lastNetworkAttemptMs = millis();
}

void loop()
{
  if (!networkReady) {
    if (millis() - lastNetworkAttemptMs >= 10000) {
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

  LiveTelemetryPacket packet;
  if (!dequeuePacket(packet)) {
    delay(5);
    return;
  }

  if (!networkReady) {
    Serial.printf("Dropping live seq=%lu while LTE is offline\n",
                  static_cast<unsigned long>(packet.sequence));
    return;
  }

  sendLivePacket(packet, "LIVE");

  static uint32_t lastDropReport = 0;
  if (droppedPackets != lastDropReport) {
    lastDropReport = droppedPackets;
    Serial.printf("ESP-NOW transit queue drops=%lu\n",
                  static_cast<unsigned long>(lastDropReport));
  }
}
