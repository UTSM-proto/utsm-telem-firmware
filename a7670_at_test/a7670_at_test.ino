#include <Arduino.h>

// LILYGO T-A7670E/G/SA ESP32-WROVER-E pin map.
static const int BOARD_POWERON_PIN = 12;
static const int MODEM_DTR_PIN = 25;
static const int MODEM_RX_PIN = 27;
static const int MODEM_TX_PIN = 26;
static const int MODEM_PWRKEY_PIN = 4;
static const int MODEM_RESET_PIN = 5;

HardwareSerial SerialAT(1);

void powerOnModem()
{
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);

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
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(3000);
}

void sendAndPrint(const char *command, uint32_t waitMs = 1500)
{
  Serial.print("\n> ");
  Serial.println(command);
  SerialAT.print(command);
  SerialAT.print("\r\n");

  uint32_t deadline = millis() + waitMs;
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    while (SerialAT.available()) Serial.write(SerialAT.read());
    delay(1);
  }
}

void setup()
{
  Serial.begin(115200);
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(1000);

  Serial.println("UTSM T-A7670X direct AT test");
  Serial.println("Powering modem; red modem LEDs should activate...");
  powerOnModem();

  sendAndPrint("AT");
  sendAndPrint("AT+SIMCOMATI", 2500);
  sendAndPrint("AT+CPIN?");
  sendAndPrint("AT+CSQ");
  Serial.println("\nAutomatic test complete.");
  Serial.println("Serial passthrough active: Both NL & CR, 115200 baud.");
}

void loop()
{
  while (Serial.available()) SerialAT.write(Serial.read());
  while (SerialAT.available()) Serial.write(SerialAT.read());
}
