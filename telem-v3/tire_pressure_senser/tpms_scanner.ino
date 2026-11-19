#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

int scanTime = 0; // 0 = continuous scanning loop
BLEScan* pBLEScan;

const char* targetSensors[] = {
  "b9:41:fa:00:46:97", // Location (eg. Front-Left)
  "b9:41:fa:00:40:ba", //
  "b9:41:fa:00:42:07", //
  "b9:41:fa:00:43:db"  //
};
const int numSensors = 4;

double temp_decoder(String payload){
      if (payload.length() >= 11) {
          uint8_t tempByte = (uint8_t)payload[3];
          double temperature = (double)tempByte - 40; 
          Serial.printf("   Temperature : %.1f °C\n", temperature);
          return temperature;
        }
         else return -1;
}

double pressure_decoder(String payload){
      if (payload.length() >= 11) {
          uint16_t pressByte = ((uint16_t)payload[4] << 8) | payload[5];
          double pressure = (double)pressByte - 100.0; 
          Serial.printf("   Pressure    : %.1f kPa\n", pressure);
          return pressure;
        }
         else return -1;
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      String foundMac = advertisedDevice.getAddress().toString();
      int rssi = advertisedDevice.getRSSI();

      // STRICT MATCH CHECK: Compare lowercased/exact strings
      bool isMatch = false;
      for (int i = 0; i < numSensors; i++) {
        // strcasecmp ignores capitalization differences in hex letters
        if (strcasecmp(foundMac.c_str(), targetSensors[i]) == 0) {
          isMatch = true;
          break;
        }
      }

      // If it doesn't match our exact 4 sensor addresses, discard instantly
      if (!isMatch) {
        return; 
      }

      // --- ONLY TARGET SENSORS REACH THIS LINE ---
      Serial.println("========================================");
      Serial.printf(" Target TPMS Packet Received!\n");
      Serial.printf("   MAC Address  : %s\n", foundMac.c_str());
      Serial.printf("   Signal (RSSI): %d dBm\n", rssi);

      if (advertisedDevice.haveManufacturerData()) {
        String payload = advertisedDevice.getManufacturerData();
        Serial.printf("   Payload Size : %d bytes\n", payload.length());

        Serial.print("   Raw Hex      : ");
        for (size_t i = 0; i < payload.length(); i++) {
          Serial.printf("%02X ", (uint8_t)payload[i]);
        }
        Serial.println();
        temp_decoder(payload);
        pressure_decoder(payload);
         }
      Serial.println("========================================");
    }
};

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\nInitializing ESP32-C3 Target TPMS Receiver...");
  
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(false); // Passive scan not active
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

void loop() {
  // Scan for 5 seconds, clear buffer, wait 3 seconds to not overflow serial monitor with text
  BLEScanResults* foundDevices = pBLEScan->start(5, false);
  pBLEScan->clearResults();
  delay(3000); 
}
