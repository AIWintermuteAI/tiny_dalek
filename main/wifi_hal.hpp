#if defined (ESP_IDF_VERSION)
#include "nvs_flash.h"
#include <WiFi.h>
#define RUN_ON_ESP32 1
#else
#include <WiFiNINA.h>
#define RUN_ON_RP2040 1
#endif

char ssid[] = "tiny_dalek";
char pass[] = "";
int status = WL_IDLE_STATUS;

IPAddress ipAddress = IPAddress(192, 168, 4, 2); // Smartphone ip
#define PORT 50123 //Change PORT if you need, alsop you have to set the same port in the mobile app

// Initialize the client library
WiFiClient client;

char last_code_received = ' ';
bool connection_status = false;

void init_wifi()
{
  // print the network name (SSID);
  Serial.print("Creating access point named: ");
  Serial.println(ssid);

#if (RUN_ON_RP2040 == 1)
  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true);
  }

  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  }

  // by default the local IP address will be 192.168.4.1
  // you can override it with the following:
  // WiFi.config(IPAddress(10, 0, 0, 1));

  // Create open network. Change this line if you want to create an WEP network:
  status = WiFi.beginAP(ssid);
  if (status != WL_AP_LISTENING) {
    Serial.println("Creating access point failed");
    // don't continue
    while (true);
  }
#else
  nvs_flash_init();
  WiFi.mode(WIFI_AP);
  status = WiFi.softAP(ssid);
  if (!status) {
    Serial.println("Creating access point failed");
    // don't continue
    while (true);
    delay(100);
  }
#endif
}


bool connect() {
#if (RUN_ON_RP2040 == 1)
  // compare the previous status to the current status
  while (WiFi.status() != WL_AP_CONNECTED) {
    // it has changed update the variable
    status = WiFi.status();

    if (status == WL_AP_CONNECTED) {
      // a device has connected to the AP
      Serial.println("Device connected to AP");
    } else {
      // a device has disconnected from the AP, and we are back in listening mode
      Serial.println("Device disconnected from AP");
    }
  }
#else
  while (WiFi.softAPgetStationNum() == 0) {
      Serial.println("Waiting for device connection");
      delay(100);
  }
  Serial.println("Device connected to AP");
#endif

  if (!client.connect(ipAddress, PORT)) {
      Serial.println("Connection to host failed");
      delay(1000);
      return false;
  }
  Serial.print("Connected to: ");
  Serial.println(client.remoteIP().toString());
  return true;
}

char get_code() {
    char code_received = ' ';
    String numericPart = "";

    while (client.available() > 0) {
      String line = client.readStringUntil('\n');
      numericPart = "";
      for (int i = 0; i < line.length(); i++) {
          int character = line[i];
          if (isDigit(character)) {
              numericPart += (char) character;
          } else if (character != '\n') {
              code_received = character;
          } else {
              break;
          }
      }
    }

    if (last_code_received == code_received) {
      return '.';
    }

    last_code_received = code_received;
    return code_received;
}