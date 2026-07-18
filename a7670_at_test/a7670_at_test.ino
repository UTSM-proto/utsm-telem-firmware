#include <Arduino.h>

// LILYGO T-A7670E/G/SA ESP32-WROVER-E pin map.
static const int BOARD_POWERON_PIN = 12;
static const int MODEM_DTR_PIN = 25;
static const int MODEM_RX_PIN = 27;
static const int MODEM_TX_PIN = 26;
static const int MODEM_PWRKEY_PIN = 4;
static const int MODEM_RESET_PIN = 5;

HardwareSerial SerialAT(1);

void pulsePowerKey(uint32_t pulseMs)
{
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(pulseMs);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
}

void powerOnModem()
{
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);
  delay(500);

  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  delay(2600);
  digitalWrite(MODEM_RESET_PIN, LOW);

  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);

  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  pulsePowerKey(100);
}

bool modemRespondsAt(uint32_t baud)
{
  SerialAT.updateBaudRate(baud);
  while (SerialAT.available()) SerialAT.read();

  for (int attempt = 0; attempt < 3; attempt++) {
    SerialAT.print("AT\r\n");
    uint32_t deadline = millis() + 1000;
    String response;
    while (static_cast<int32_t>(deadline - millis()) > 0) {
      while (SerialAT.available()) response += static_cast<char>(SerialAT.read());
      if (response.indexOf("OK") >= 0) {
        Serial.printf("Modem responded at %lu baud: %s\n", baud, response.c_str());
        return true;
      }
      delay(1);
    }
  }
  return false;
}

uint32_t findModemBaud()
{
  static const uint32_t rates[] = {
    115200, 9600, 57600, 38400, 19200, 74880, 230400, 460800
  };
  for (uint32_t rate : rates) {
    Serial.printf("Trying modem UART at %lu baud...\n", rate);
    if (modemRespondsAt(rate)) return rate;
  }
  return 0;
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

  Serial.println("Waiting 15 seconds for the modem to boot...");
  delay(15000);
  uint32_t modemBaud = findModemBaud();

  if (modemBaud == 0) {
    Serial.println("No AT response. Retrying PWRKEY with a 1-second pulse...");
    pulsePowerKey(1000);
    delay(15000);
    modemBaud = findModemBaud();
  }

  if (modemBaud == 0) {
    Serial.println("MODEM TEST FAILED: no response at any tested baud rate.");
    Serial.println("Report whether the red modem LEDs are on or completely off.");
    return;
  }

  sendAndPrint("AT");
  sendAndPrint("AT+SIMCOMATI", 2500);
  sendAndPrint("AT+CPIN?");
  sendAndPrint("AT+CSQ");
  Serial.println("\nAutomatic test complete.");
  Serial.println("Serial passthrough active: Both NL & CR, 115200 baud.");
}

void loop()
{
  while (Serial.available()) {
    int value = Serial.read();
    Serial.write(value);  // Local echo so sent commands are visible.
    SerialAT.write(value);
  }
  while (SerialAT.available()) Serial.write(SerialAT.read());
}
